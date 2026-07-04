using System;
using System.Collections.Generic;
using System.Diagnostics.CodeAnalysis;
using System.Text.RegularExpressions;
using ConnectorLib;
using ConnectorLib.JSON;
using ConnectorLib.SimpleTCP;
using CrowdControl.Common;
using Newtonsoft.Json;
using ConnectorType = CrowdControl.Common.ConnectorType;
using EffectResponse = ConnectorLib.JSON.EffectResponse;
using EffectStatus = CrowdControl.Common.EffectStatus;
using Log = CrowdControl.Common.Log;
using LogLevel = CrowdControl.Common.LogLevel;
using static System.Linq.Enumerable;

namespace CrowdControl.Games.Packs.VVVVVV;

public class VVVVVV : SimpleTCPPack<SimpleTCPServerConnector>
{
	public override string Host => "127.0.0.1";
	public override ushort Port => 28379;

	public VVVVVV(UserRecord player, Func<CrowdControlBlock, bool> responseHandler, Action<object> statusUpdateHandler) : base(player, responseHandler, statusUpdateHandler) { }

	public override Game Game => new("VVVVVV", "VVVVVV", "PC", ConnectorType.SimpleTCPServerConnector);

	public override EffectList Effects => new Effect[]
	{
        // price is default cost in US cents
        // descriptions are short
		new("Flip Gravity", "flip_gravity") { Price = 10, Category = "Movement", Description = "Flip the player's gravity upside down" },
		new("Kill Player", "kill_player") { Price = 75, Category = "Player", Description = "" }, // TODO: default price scaling would be great on this one to account for the long timed level
		new("Change Music", "change_music") { Price = 1, Category = "Aesthetic", Description = "Swap the current music track" },
        new("Invincibility", "invincibility") { Price = 30, Duration = 15, Category = "Player", Description = "Make the player temporarily immune to death" },
        new("Slow Motion", "slow_motion") { Price = 25, Duration = 15, Category = "World", Description = "Make the game move slowly" },
        new("Screen Shake", "screen_shake") { Price = 20, Duration = 10, Category = "Aesthetic", Description = "Briefly shake the screen around" },
        new("Flip Screen", "flip_mode") { Price = 20, Duration = 10, Category = "Aesthetic", Description = "Briefly flip the screen upside down" },
        new("Recolor Player", "recolor_player") { Price = 1, Category = "Aesthetic", Description = "Randomly change the player's color" },
        new("Fast Motion", "fast_motion") { Price = 25, Duration = 15, Category = "World", Description = "Make the game move quickly" },
        new("Invert Left/Right", "invert_lr") { Price = 50, Duration = 15, Category = "Movement", Description = "Swaps the left and right movement keys" },
        new("Kill Enemies", "kill_enemies") { Price = 20, Category = "Enemies", Description = "Kills all on-screen enemies" },
        new("Lights Out", "lights_out") { Price = 50, Duration = 10, Category = "World", Description = "Casts a small circle of light on the player, hiding everything else with black" },
        new("Auto Walk Right", "auto_walk_right") { Price = 30, Duration = 10, Category = "Movement", Description = "Force the player to walk to the right, unless they press left" },
        new("Spawn Enemies", "spawn_enemies") { Price = 25, Category = "World", Description = "Spawns random enemies on ground tiles (if available) that last until the player leaves the room" },
        new("Cursed Mode", "cursed_mode") { Price = 30, Duration = 15, Category = "Movement", Description = "The gravity swap button temporarily acts as a jump button instead (air jumps allowed)" },
	};
}