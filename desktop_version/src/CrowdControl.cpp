#include "CrowdControl.h"

/* Socket headers must come before SDL (which may pull in windows.h). */
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET cc_socket;
#define CC_INVALID_SOCKET INVALID_SOCKET
#define cc_closesocket closesocket
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int cc_socket;
#define CC_INVALID_SOCKET (-1)
#define cc_closesocket close
#endif

#include <SDL.h>
#include <cJSON/cJSON.h>
#include <string>
#include <vector>

#include "Entity.h"
#include "Enums.h"
#include "Game.h"
#include "Graphics.h"
#include "KeyPoll.h"
#include "Map.h"
#include "Maths.h"
#include "Music.h"
#include "Script.h"
#include "UtilityClass.h"
#include "Vlogging.h"

#define CC_PORT 28379

/* Fallback for timed requests missing a duration (shouldn't happen). */
#define CC_DEFAULT_DURATION_MS 30000

namespace cc
{

enum RequestType
{
    REQ_TEST = 0,
    REQ_START = 1,
    REQ_STOP = 2,
    REQ_GAMEUPDATE = 0xFD,
    REQ_KEEPALIVE = 0xFF
};

#define RESP_GAMEUPDATE 0xFD

enum EffectStatus
{
    STATUS_SUCCESS = 0,
    STATUS_FAILURE = 1,
    STATUS_RETRY = 3,
    STATUS_PAUSED = 6,
    STATUS_RESUMED = 7,
    STATUS_FINISHED = 8
};

struct Request
{
    Uint32 id;
    int type;
    std::string code;
    std::string viewer;
    Sint32 duration_ms;
};

struct EffectDef
{
    const char* code;
    const char* display_name;
    bool timed;
    bool (*can_run)(void); /* NULL = default in_gameplay() */
    void (*apply)(void);
    void (*tick)(void);    /* timed only, may be NULL: per-frame reassert */
    void (*revert)(void);  /* timed only */
};

struct ActiveEffect
{
    Uint32 id;
    const EffectDef* def;
    Sint32 remaining_ms;
    bool paused;
};

/* Main thread only */
static std::vector<ActiveEffect> active;
static int announce_mode = ANNOUNCE_EFFECT_ONLY;

/* Cross-thread state */
static SDL_Thread* thread = NULL;
static SDL_mutex* in_mutex = NULL;
static SDL_mutex* out_mutex = NULL;
static std::vector<Request> incoming;
static std::vector<std::string> outgoing;
static SDL_atomic_t want_connect;
static SDL_atomic_t connected;
static SDL_atomic_t quit_flag;

/* ------------------------------------------------------------------ */
/* Effect implementations (main thread)                               */
/* ------------------------------------------------------------------ */

static bool in_gameplay(void)
{
    return key.isActive
        && game.gamestate == GAMEMODE
        && !script.running
        && game.deathseq == -1
        && game.lifeseq == 0
        && !game.completestop
        && !game.physics_frozen()
        && INBOUNDS_VEC(obj.getplayer(), obj.entities);
}

static bool is_active(const char* code);

/* kill_player */
static bool can_kill(void)
{
    return in_gameplay() && !map.invincibility;
}
static void apply_kill(void)
{
    game.deathseq = 30;
}

/* flip_gravity */
static void apply_flipgravity(void)
{
    game.gravitycontrol = !game.gravitycontrol;
    game.totalflips++;
    music.playef(game.gravitycontrol ? Sound_FLIP : Sound_UNFLIP);
}

/* change_music */
static void apply_changemusic(void)
{
    /* Gameplay tracks only: no menu/pause music or jingles. */
    static const int tracks[] = {
        Music_PUSHINGONWARDS,
        Music_POSITIVEFORCE,
        Music_POTENTIALFORANYTHING,
        Music_PASSIONFOREXPLORING,
        Music_PREDESTINEDFATE,
        Music_POSITIVEFORCEREVERSED,
        Music_POPULARPOTPOURRI,
        Music_PIPEDREAM,
        Music_PRESSURECOOKER,
        Music_PACEDENERGY,
        Music_PIERCINGTHESKY,
        Music_PREDESTINEDFATEREMIX
    };
    int track;
    do
    {
        track = tracks[(int) (fRandom() * SDL_arraysize(tracks)) % SDL_arraysize(tracks)];
    }
    while (track == music.currentsong);
    music.niceplay(track);
}

/* invincibility */
static bool orig_invincibility = false;
static bool can_noncompetitive(void)
{
    return in_gameplay() && !game.incompetitive();
}
static void apply_invincibility(void)
{
    orig_invincibility = map.invincibility;
    map.invincibility = true;
}
static void revert_invincibility(void)
{
    map.invincibility = orig_invincibility;
}

/* slow_motion / fast_motion: both repurpose game.slowdown (fast motion
 * uses a Crowd Control-specific value handled in get_framerate()), so
 * they share the saved original and refuse to run while the other one
 * is active. */
static int orig_slowdown = 30;
static bool can_slowmotion(void)
{
    return can_noncompetitive() && !is_active("fast_motion");
}
static bool can_fastmotion(void)
{
    return can_noncompetitive() && !is_active("slow_motion");
}
static void apply_slowmotion(void)
{
    orig_slowdown = game.slowdown;
    game.slowdown = 12;
}
static void apply_fastmotion(void)
{
    orig_slowdown = game.slowdown;
    game.slowdown = 40;
}
static void revert_speed(void)
{
    game.slowdown = orig_slowdown;
}

/* kill_enemies */
static bool has_enemies(void)
{
    for (size_t i = 0; i < obj.entities.size(); i++)
    {
        if (obj.entities[i].rule == 1 && !obj.entities[i].invis)
        {
            return true;
        }
    }
    return false;
}
static bool can_killenemies(void)
{
    return in_gameplay() && has_enemies();
}
static void apply_killenemies(void)
{
    for (size_t i = 0; i < obj.entities.size(); i++)
    {
        if (obj.entities[i].rule == 1 && !obj.entities[i].invis)
        {
            obj.disableentity((int) i);
        }
    }
    music.playef(Sound_DISAPPEAR);
}

/* spawn_enemies: enemies vanish with the room reload or room
 * change, so no cleanup is needed. Placement works in 8x8 tile coords;
 * an enemy is 16x16 (2x2 tiles) and wants solid ground below it. */
static bool find_enemy_spot(int* out_xp, int* out_yp)
{
    const int player = obj.getplayer();
    if (!INBOUNDS_VEC(player, obj.entities))
    {
        return false;
    }
    for (int attempt = 0; attempt < 200; attempt++)
    {
        const int tx = 1 + (int) (fRandom() * 37.0f) % 37; /* 1..37 */
        const int ty = 1 + (int) (fRandom() * 25.0f) % 25; /* 1..25 */
        const int xp = tx * 8;
        const int yp = ty * 8;

        /* Not too close to the player. */
        if (SDL_abs(xp - obj.entities[player].xp) < 56
        && SDL_abs(yp - obj.entities[player].yp) < 56)
        {
            continue;
        }

        /* The 2x2 tile body must be free... */
        bool blocked = false;
        for (int dx = 0; dx < 2 && !blocked; dx++)
        {
            for (int dy = 0; dy < 2 && !blocked; dy++)
            {
                blocked = map.collide(tx + dx, ty + dy, false);
            }
        }
        if (blocked)
        {
            continue;
        }

        /* ...and stood on solid ground. */
        if (!map.collide(tx, ty + 2, false) || !map.collide(tx + 1, ty + 2, false))
        {
            continue;
        }

        *out_xp = xp;
        *out_yp = yp;
        return true;
    }
    return false;
}
static bool can_spawnenemies(void)
{
    int xp, yp;
    return in_gameplay() && !map.towermode && find_enemy_spot(&xp, &yp);
}
static void apply_spawnenemies(void)
{
    int spawned = 0;
    for (int i = 0; i < 3; i++)
    {
        int xp, yp;
        if (!find_enemy_spot(&xp, &yp))
        {
            break;
        }
        /* Simple enemy, bouncing horizontally (behave 2/3), speed 3. */
        obj.createentity(xp, yp, 1, 2 + (fRandom() < 0.5f ? 0 : 1), 3);
        spawned++;
    }
    if (spawned > 0)
    {
        music.playef(Sound_TELEPORT);
    }
}

/* screen_shake */
static void tick_screenshake(void)
{
    game.screenshake = 20;
}
static void revert_screenshake(void)
{
    game.screenshake = 0;
}

/* flip_mode: only touch the runtime flag, never the persistent setting
 * (graphics.setflipmode is saved to the settings file). The game clobbers
 * graphics.flipmode on various paths, so reassert it every frame. */
static void tick_flipmode(void)
{
    graphics.flipmode = !graphics.setflipmode;
}
static void revert_flipmode(void)
{
    graphics.flipmode = graphics.setflipmode;
}

/* recolor_player: Death/respawn resets the
 * entity colour, so reassert it every frame (but let the death flash show
 * while deathseq is running). If already recolored, there's a 1 in 10
 * chance the effect resets the colour back to default instead. */
static int colour_override = -1;
static void restore_colour(void)
{
    const int player = obj.getplayer();
    colour_override = -1;
    if (INBOUNDS_VEC(player, obj.entities))
    {
        obj.entities[player].colour = game.savecolour;
    }
}
static void apply_recolor(void)
{
    static const int colours[] = {
        EntityColour_CREW_GREEN,
        EntityColour_CREW_YELLOW,
        EntityColour_CREW_RED,
        EntityColour_CREW_BLUE,
        EntityColour_CREW_PURPLE
    };
    if (colour_override != -1 && fRandom() < 0.1f)
    {
        restore_colour();
        return;
    }
    const int previous = colour_override != -1 ? colour_override : game.savecolour;
    do
    {
        colour_override = colours[(int) (fRandom() * SDL_arraysize(colours)) % SDL_arraysize(colours)];
    }
    while (colour_override == previous);
}
static void tick_recolor(void)
{
    const int player = obj.getplayer();
    if (colour_override != -1
    && game.deathseq == -1
    && INBOUNDS_VEC(player, obj.entities))
    {
        obj.entities[player].colour = colour_override;
    }
}

static const EffectDef effects[] = {
    /* code             display name      timed  can_run              apply               tick             revert */
    { "kill_player",    "Kill Player",    false, can_kill,            apply_kill,         NULL,            NULL },
    { "flip_gravity",   "Flip Gravity",   false, NULL,                apply_flipgravity,  NULL,            NULL },
    { "change_music",   "Change Music",   false, NULL,                apply_changemusic,  NULL,            NULL },
    { "invincibility",  "Invincibility",  true,  can_noncompetitive,  apply_invincibility, NULL,           revert_invincibility },
    { "slow_motion",    "Slow Motion",    true,  can_slowmotion,      apply_slowmotion,   NULL,            revert_speed },
    { "fast_motion",    "Fast Motion",    true,  can_fastmotion,      apply_fastmotion,   NULL,            revert_speed },
    { "screen_shake",   "Screen Shake",   true,  NULL,                NULL,               tick_screenshake, revert_screenshake },
    { "flip_mode",      "Flip Screen",    true,  NULL,                NULL,               tick_flipmode,   revert_flipmode },
    { "recolor_player", "Recolor Player", false, NULL,                apply_recolor,      NULL,            NULL },
    { "kill_enemies",   "Kill Enemies",   false, can_killenemies,     apply_killenemies,  NULL,            NULL },
    { "spawn_enemies",  "Spawn Enemies",  false, can_spawnenemies,    apply_spawnenemies, NULL,            NULL },
    /* Marker effects: their behavior lives in the exported hooks
     * (modify_movement_input / modify_flip_input / draw_lights_out),
     * driven by is_active(). */
    { "invert_lr",      "Invert Left/Right", true, NULL,              NULL,               NULL,            NULL },
    { "auto_walk_right", "Auto Walk Right", true, NULL,               NULL,               NULL,            NULL },
    { "lights_out",     "Lights Out",     true,  NULL,                NULL,               NULL,            NULL },
    { "cursed_mode",    "Cursed Mode",    true,  NULL,                NULL,               NULL,            NULL }
};

static const EffectDef* find_effect(const std::string& code)
{
    for (size_t i = 0; i < SDL_arraysize(effects); i++)
    {
        if (code == effects[i].code)
        {
            return &effects[i];
        }
    }
    return NULL;
}

static bool is_active(const char* code)
{
    for (size_t i = 0; i < active.size(); i++)
    {
        if (SDL_strcmp(active[i].def->code, code) == 0)
        {
            return true;
        }
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* Responses                                                          */
/* ------------------------------------------------------------------ */

/* time_remaining_ms < 0 means "omit the field". */
static void respond(const Uint32 id, const int status, const Sint32 time_remaining_ms)
{
    if (!SDL_AtomicGet(&connected))
    {
        return;
    }

    cJSON* root = cJSON_CreateObject();
    if (root == NULL)
    {
        return;
    }
    cJSON_AddNumberToObject(root, "id", (double) id);
    cJSON_AddNumberToObject(root, "type", 0); /* ResponseType EffectRequest */
    cJSON_AddNumberToObject(root, "status", status);
    if (time_remaining_ms >= 0)
    {
        cJSON_AddNumberToObject(root, "timeRemaining", (double) time_remaining_ms);
    }

    char* printed = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (printed == NULL)
    {
        return;
    }

    SDL_LockMutex(out_mutex);
    outgoing.push_back(std::string(printed));
    SDL_UnlockMutex(out_mutex);

    cJSON_free(printed);
}

/* The Crowd Control GameState string best describing what the game is
 * doing right now, for GameUpdate (0xFD) requests. "ready" is the only
 * state in which effects are processed. */
static const char* current_game_state(void)
{
    if (in_gameplay())
    {
        return "ready";
    }
    if (!key.isActive)
    {
        return "notFocused";
    }
    switch (game.gamestate)
    {
    case PRELOADER:
        return "loading";
    case TITLEMODE:
    case EDITORMODE:
        return "wrongMode";
    case MAPMODE:
    case TELEPORTERMODE:
        return "paused";
    case GAMECOMPLETE:
    case GAMECOMPLETE2:
        return "cutscene";
    case GAMEMODE:
        if (script.running || game.completestop)
        {
            return "cutscene";
        }
        if (game.deathseq != -1 || game.lifeseq != 0
        || !INBOUNDS_VEC(obj.getplayer(), obj.entities))
        {
            return "badPlayerState";
        }
        if (game.physics_frozen())
        {
            return "paused";
        }
        break;
    }
    return "unknown";
}

static void respond_gamestate(const Uint32 id)
{
    if (!SDL_AtomicGet(&connected))
    {
        return;
    }

    cJSON* root = cJSON_CreateObject();
    if (root == NULL)
    {
        return;
    }
    cJSON_AddNumberToObject(root, "id", (double) id);
    cJSON_AddNumberToObject(root, "type", RESP_GAMEUPDATE);
    cJSON_AddStringToObject(root, "state", current_game_state());

    char* printed = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (printed == NULL)
    {
        return;
    }

    SDL_LockMutex(out_mutex);
    outgoing.push_back(std::string(printed));
    SDL_UnlockMutex(out_mutex);

    cJSON_free(printed);
}

static void announce(const Request& req, const EffectDef* def)
{
    char buffer[128];
    switch (announce_mode)
    {
    case ANNOUNCE_WITH_VIEWER:
        SDL_snprintf(
            buffer, sizeof(buffer), "%s used %s!",
            req.viewer.empty() ? "The crowd" : req.viewer.c_str(),
            def->display_name
        );
        break;
    case ANNOUNCE_EFFECT_ONLY:
        SDL_snprintf(buffer, sizeof(buffer), "%s!", def->display_name);
        break;
    default: /* ANNOUNCE_DISABLED */
        return;
    }
    graphics.createtextbox(buffer, -1, 12, 174, 174, 174);
    graphics.textboxcenterx();
    graphics.textboxtimer(60);
}

/* ------------------------------------------------------------------ */
/* Main-thread frame logic                                            */
/* ------------------------------------------------------------------ */

static void execute(const Request& req, const EffectDef* def)
{
    if (def->apply != NULL)
    {
        def->apply();
    }

    if (def->timed)
    {
        ActiveEffect effect;
        effect.id = req.id;
        effect.def = def;
        effect.remaining_ms = req.duration_ms;
        effect.paused = false;
        active.push_back(effect);
        respond(req.id, STATUS_SUCCESS, req.duration_ms);
    }
    else
    {
        respond(req.id, STATUS_SUCCESS, -1);
    }

    announce(req, def);
}

static void stop_all(const Request& req)
{
    /* Stop all running and pending instances of the given effect code. */
    for (size_t i = active.size(); i > 0; i--)
    {
        ActiveEffect& effect = active[i - 1];
        if (req.code == effect.def->code)
        {
            if (effect.def->revert != NULL)
            {
                effect.def->revert();
            }
            respond(effect.id, STATUS_FINISHED, 0);
            active.erase(active.begin() + (i - 1));
        }
    }
    respond(req.id, STATUS_SUCCESS, -1);
}

static void process_request(const Request& req)
{
    switch (req.type)
    {
    case REQ_TEST:
    {
        const EffectDef* def = find_effect(req.code);
        if (def == NULL)
        {
            respond(req.id, STATUS_FAILURE, -1);
        }
        else if ((def->timed && is_active(def->code))
        || !(def->can_run != NULL ? def->can_run() : in_gameplay()))
        {
            respond(req.id, STATUS_RETRY, -1);
        }
        else
        {
            respond(req.id, STATUS_SUCCESS, -1);
        }
        break;
    }
    case REQ_START:
    {
        const EffectDef* def = find_effect(req.code);
        if (def == NULL)
        {
            vlog_warn("Crowd Control: unknown effect \"%s\"", req.code.c_str());
            respond(req.id, STATUS_FAILURE, -1);
            break;
        }
        /* The app waits a few seconds before re-sending on Retry, so
         * refusals (duplicates, not in gameplay) are answered directly. */
        if ((def->timed && is_active(def->code))
        || !(def->can_run != NULL ? def->can_run() : in_gameplay()))
        {
            respond(req.id, STATUS_RETRY, -1);
            break;
        }
        execute(req, def);
        break;
    }
    case REQ_STOP:
        stop_all(req);
        break;
    case REQ_GAMEUPDATE:
        respond_gamestate(req.id);
        break;
    default:
        /* KeepAlives are dropped on the socket thread; ignore the rest. */
        break;
    }
}

static void tick_active(void)
{
    const bool gameplay = in_gameplay();

    for (size_t i = active.size(); i > 0; i--)
    {
        ActiveEffect& effect = active[i - 1];

        if (!gameplay)
        {
            if (!effect.paused)
            {
                effect.paused = true;
                respond(effect.id, STATUS_PAUSED, effect.remaining_ms);
            }
            continue;
        }

        if (effect.paused)
        {
            effect.paused = false;
            respond(effect.id, STATUS_RESUMED, effect.remaining_ms);
        }

        /* get_timestep() reflects slowdown, keeping timers wall-clock
         * accurate even while the slow motion effect is running. */
        effect.remaining_ms -= game.get_timestep();

        if (effect.def->tick != NULL)
        {
            effect.def->tick();
        }

        if (effect.remaining_ms <= 0)
        {
            if (effect.def->revert != NULL)
            {
                effect.def->revert();
            }
            respond(effect.id, STATUS_FINISHED, 0);
            active.erase(active.begin() + (i - 1));
        }
    }
}

void update(void)
{
    if (thread == NULL)
    {
        return;
    }

    /* Drain freshly received requests. */
    std::vector<Request> requests;
    SDL_LockMutex(in_mutex);
    requests.swap(incoming);
    SDL_UnlockMutex(in_mutex);

    for (size_t i = 0; i < requests.size(); i++)
    {
        process_request(requests[i]);
    }

    tick_active();

    /* The recolor override is indefinite, so it lives outside the
     * active-effect timers. */
    tick_recolor();
}

void revert_all(void)
{
    for (size_t i = 0; i < active.size(); i++)
    {
        if (active[i].def->revert != NULL)
        {
            active[i].def->revert();
        }
        respond(active[i].id, STATUS_FINISHED, 0);
    }
    active.clear();

    if (colour_override != -1)
    {
        restore_colour();
    }
}

bool is_connected(void)
{
    return SDL_AtomicGet(&connected) != 0;
}

int get_announce_mode(void)
{
    return announce_mode;
}

void set_announce_mode(const int mode)
{
    announce_mode = (mode % NUM_ANNOUNCE_MODES + NUM_ANNOUNCE_MODES) % NUM_ANNOUNCE_MODES;
}

void request_connect(void)
{
    if (thread != NULL && !SDL_AtomicGet(&connected))
    {
        vlog_info("Crowd Control: retrying connection...");
        SDL_AtomicSet(&want_connect, 1);
    }
}

int real_slowdown(void)
{
    return is_active("slow_motion") || is_active("fast_motion")
        ? orig_slowdown
        : game.slowdown;
}

/* Cursed Mode tuning. Gravity is +-3/frame with vy clamped to +-10
 * (see applyfriction()/updateentitylogic()), so a jump can rise about
 * 2.5 tiles before gravity wins. Air jumps are always allowed so rooms
 * that need a flip to traverse don't become softlocks. */
#define CC_JUMP_VY 10.0f

bool modify_flip_input(void)
{
    if (!is_active("cursed_mode"))
    {
        return false;
    }

    /* Away from the floor, relative to the current gravity direction. */
    const float up = game.gravitycontrol == 0 ? -1.0f : 1.0f;
    bool jumped = false;

    for (size_t i = 0; i < obj.entities.size(); i++)
    {
        entclass& ent = obj.entities[i];
        if (ent.rule != 0 || game.jumppressed <= 0)
        {
            continue;
        }

        ent.vy = up * CC_JUMP_VY;
        ent.ay = up * 3.0f; /* same kick the flip code gives */
        jumped = true;
    }

    if (game.jumppressed > 0)
    {
        game.jumppressed--;
        if (jumped)
        {
            game.jumppressed = 0;
            music.playef(game.gravitycontrol == 0 ? Sound_FLIP : Sound_UNFLIP);
        }
    }

    return true;
}

void modify_movement_input(void)
{
    if (is_active("invert_lr"))
    {
        const bool left = game.press_left;
        game.press_left = game.press_right;
        game.press_right = left;
    }
    if (is_active("auto_walk_right") && !game.press_left)
    {
        game.press_right = true;
    }
}

void draw_lights_out(void)
{
    if (!is_active("lights_out"))
    {
        return;
    }

    const int player = obj.getplayer();
    if (!INBOUNDS_VEC(player, obj.entities))
    {
        graphics.fill_rect(0, 0, 0);
        return;
    }

    const entclass& ent = obj.entities[player];
    const int yoff = map.towermode ? (int) graphics.lerp(map.oldypos, map.ypos) : 0;
    const int cx = (int) graphics.lerp(ent.lerpoldxp, ent.xp) + ent.w / 2;
    const int cy = (int) graphics.lerp(ent.lerpoldyp, ent.yp) + ent.h / 2 - yoff;
    const float radius = 60.0f;

    /* Black out everything except a circle of light around the player,
     * drawn as horizontal strips. */
    for (int y = 0; y < 240; y++)
    {
        const float dy = (float) (y - cy);
        if (SDL_fabsf(dy) >= radius)
        {
            graphics.fill_rect(0, y, 320, 1, 0, 0, 0);
            continue;
        }
        const int halfw = (int) SDL_sqrtf(radius * radius - dy * dy);
        if (cx - halfw > 0)
        {
            graphics.fill_rect(0, y, cx - halfw, 1, 0, 0, 0);
        }
        if (cx + halfw < 320)
        {
            graphics.fill_rect(cx + halfw, y, 320 - (cx + halfw), 1, 0, 0, 0);
        }
    }
}

bool real_invincibility(void)
{
    return is_active("invincibility") ? orig_invincibility : map.invincibility;
}

/* ------------------------------------------------------------------ */
/* Socket thread                                                      */
/* ------------------------------------------------------------------ */

static void parse_message(const char* str)
{
    cJSON* root = cJSON_Parse(str);
    if (root == NULL)
    {
        vlog_warn("Crowd Control: dropping malformed message: %s", str);
        return;
    }

    const cJSON* type = cJSON_GetObjectItemCaseSensitive(root, "type");
    const int type_value = cJSON_IsNumber(type) ? type->valueint : -1;

    if (type_value == REQ_KEEPALIVE)
    {
        cJSON_Delete(root);
        return;
    }

    Request req;
    const cJSON* id = cJSON_GetObjectItemCaseSensitive(root, "id");
    const cJSON* code = cJSON_GetObjectItemCaseSensitive(root, "code");
    const cJSON* viewer = cJSON_GetObjectItemCaseSensitive(root, "viewer");
    const cJSON* duration = cJSON_GetObjectItemCaseSensitive(root, "duration");

    req.id = cJSON_IsNumber(id) ? (Uint32) id->valuedouble : 0;
    req.type = type_value;
    if (cJSON_IsString(code) && code->valuestring != NULL)
    {
        req.code = code->valuestring;
    }
    if (cJSON_IsString(viewer) && viewer->valuestring != NULL)
    {
        req.viewer = viewer->valuestring;
    }
    req.duration_ms = cJSON_IsNumber(duration)
        ? (Sint32) duration->valuedouble
        : CC_DEFAULT_DURATION_MS;

    cJSON_Delete(root);

    SDL_LockMutex(in_mutex);
    incoming.push_back(req);
    SDL_UnlockMutex(in_mutex);
}

static cc_socket try_connect(void)
{
    cc_socket sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == CC_INVALID_SOCKET)
    {
        return CC_INVALID_SOCKET;
    }

    struct sockaddr_in addr;
    SDL_zero(addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons(CC_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (connect(sock, (struct sockaddr*) &addr, sizeof(addr)) != 0)
    {
        cc_closesocket(sock);
        return CC_INVALID_SOCKET;
    }

    const int nodelay = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (const char*) &nodelay, sizeof(nodelay));

    return sock;
}

/* Send every queued response, each terminated by a NUL byte.
 * Returns false on socket failure. */
static bool flush_outgoing(const cc_socket sock)
{
    std::vector<std::string> queued;
    SDL_LockMutex(out_mutex);
    queued.swap(outgoing);
    SDL_UnlockMutex(out_mutex);

    for (size_t i = 0; i < queued.size(); i++)
    {
        /* c_str() gives us the trailing NUL terminator for free. */
        const char* data = queued[i].c_str();
        size_t remaining = queued[i].size() + 1;
        while (remaining > 0)
        {
            const int sent = send(sock, data, (int) remaining, 0);
            if (sent <= 0)
            {
                return false;
            }
            data += sent;
            remaining -= sent;
        }
    }
    return true;
}

static int thread_func(void* data)
{
    (void) data;

    cc_socket sock = CC_INVALID_SOCKET;
    std::string readbuf;

    while (!SDL_AtomicGet(&quit_flag))
    {
        if (sock == CC_INVALID_SOCKET)
        {
            if (!SDL_AtomicGet(&want_connect))
            {
                SDL_Delay(50);
                continue;
            }
            SDL_AtomicSet(&want_connect, 0);

            sock = try_connect();
            if (sock == CC_INVALID_SOCKET)
            {
                vlog_info(
                    "Crowd Control: could not connect to 127.0.0.1:%d"
                    " (press F9 to retry)",
                    CC_PORT
                );
                continue;
            }

            readbuf.clear();
            SDL_LockMutex(out_mutex);
            outgoing.clear();
            SDL_UnlockMutex(out_mutex);
            SDL_AtomicSet(&connected, 1);
            vlog_info("Crowd Control: connected");
            continue;
        }

        bool failed = false;

        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(sock, &readfds);
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 100 * 1000;

        const int sel = select((int) sock + 1, &readfds, NULL, NULL, &tv);
        if (sel < 0)
        {
            failed = true;
        }
        else if (sel > 0 && FD_ISSET(sock, &readfds))
        {
            char buffer[1024];
            const int received = recv(sock, buffer, sizeof(buffer), 0);
            if (received <= 0)
            {
                failed = true;
            }
            else
            {
                for (int i = 0; i < received; i++)
                {
                    if (buffer[i] == '\0')
                    {
                        parse_message(readbuf.c_str());
                        readbuf.clear();
                    }
                    else
                    {
                        readbuf += buffer[i];
                    }
                }
            }
        }

        if (!failed)
        {
            failed = !flush_outgoing(sock);
        }

        if (failed)
        {
            cc_closesocket(sock);
            sock = CC_INVALID_SOCKET;
            SDL_AtomicSet(&connected, 0);
            readbuf.clear();
            SDL_LockMutex(out_mutex);
            outgoing.clear();
            SDL_UnlockMutex(out_mutex);
            vlog_info("Crowd Control: disconnected (press F9 to reconnect)");
        }
    }

    if (sock != CC_INVALID_SOCKET)
    {
        cc_closesocket(sock);
        SDL_AtomicSet(&connected, 0);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                          */
/* ------------------------------------------------------------------ */

void init(void)
{
#ifdef _WIN32
    WSADATA wsa_data;
    if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0)
    {
        vlog_error("Crowd Control: WSAStartup failed");
        return;
    }
#endif

    in_mutex = SDL_CreateMutex();
    out_mutex = SDL_CreateMutex();
    SDL_AtomicSet(&want_connect, 1);
    SDL_AtomicSet(&connected, 0);
    SDL_AtomicSet(&quit_flag, 0);

    thread = SDL_CreateThread(thread_func, "CrowdControl", NULL);
    if (thread == NULL)
    {
        vlog_error("Crowd Control: could not create thread: %s", SDL_GetError());
    }
}

void shutdown(void)
{
    if (thread != NULL)
    {
        SDL_AtomicSet(&quit_flag, 1);
        SDL_WaitThread(thread, NULL);
        thread = NULL;
    }
    if (in_mutex != NULL)
    {
        SDL_DestroyMutex(in_mutex);
        in_mutex = NULL;
    }
    if (out_mutex != NULL)
    {
        SDL_DestroyMutex(out_mutex);
        out_mutex = NULL;
    }
#ifdef _WIN32
    WSACleanup();
#endif
}

} /* namespace cc */
