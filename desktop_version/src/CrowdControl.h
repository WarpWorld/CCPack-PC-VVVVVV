#ifndef CROWDCONTROL_H
#define CROWDCONTROL_H

/* Crowd Control (crowdcontrol.live) integration.
 *
 * The game acts as a TCP client ("SimpleTCPServerConnector") connecting to
 * the Crowd Control app's server on 127.0.0.1:28379. A background thread
 * owns the socket; all effect logic runs on the main thread via update().
 * Messages in both directions are NUL-terminated UTF-8 JSON strings.
 */

namespace cc
{
    /* How effect announcement popups are shown. */
    enum AnnounceMode
    {
        ANNOUNCE_WITH_VIEWER = 0, /* "viewer used Effect!" */
        ANNOUNCE_EFFECT_ONLY = 1, /* "Effect!" */
        ANNOUNCE_DISABLED = 2,

        NUM_ANNOUNCE_MODES
    };

    int get_announce_mode(void);
    void set_announce_mode(int mode);

    /* Start the socket thread and attempt a first connection. */
    void init(void);

    /* Stop the socket thread and clean up. */
    void shutdown(void);

    /* Process requests and tick timed effects. Call once per fixed frame. */
    void update(void);

    /* Retry connecting if not connected (F9). */
    void request_connect(void);

    /* Revert all active timed effects (e.g. before the final settings save). */
    void revert_all(void);

    bool is_connected(void);

    /* Applies input-modifying effects (invert left/right, auto walk).
     * Called from gameinput() after the movement flags are set. */
    void modify_movement_input(void);

    /* Cursed Mode: the flip button acts as a (wall) jump button instead.
     * Called from gameinput() in place of the flip processing; returns
     * true if the effect is active and flipping should be suppressed. */
    bool modify_flip_input(void);

    /* Draws the "lights out" overlay, if active. Called from gamerender()
     * after the world and entities are drawn. */
    void draw_lights_out(void);

    /* The values the settings file should persist, with any active
     * Crowd Control effect stripped out. */
    int real_slowdown(void);
    bool real_invincibility(void);
}

#endif /* CROWDCONTROL_H */
