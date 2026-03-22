# MugServ — Anope IRC Services Economy Module

A standalone IRC economy service for **Anope IRC Services 2.0**, ported from a Sopel IRC bot plugin. Players collect coins, mug each other, gamble, place bounties, and buy items from a crime shop — all via private messages to the `MugServ` bot.

---

## Features

- **Coin collection** with wealth-scaling rewards
- **Mugging system** with success, fail, and critical fail outcomes
- **Gambling** (`BET`)
- **Bounty system** — put a price on someone's head
- **Item shop** with passive and consumable items (stackable up to 3x)
- **Jail system** with auto-bail support
- **Leaderboards** (top 5 and top 10)
- **NickServ-gated access** — auto-enroll on first use, no manual registration
- **Per-account data** — all grouped nicks under one NickServ account share one balance
- **Flat-file database** — human-readable, saved every 5 minutes and on shutdown
- **Announce channel** support — public results posted to a configured channel
- **Admin commands** for IRCops and configured admins

---

## Requirements

- Anope IRC Services **2.0.x**
- A C++11 (or later) compiler
- NickServ module loaded (standard in any Anope install)

---

## Installation

1. Copy `ms_mugserv.cpp` to your Anope third-party modules directory:
   ```
   cp ms_mugserv.cpp /path/to/anope/modules/third/ms_mugserv.cpp
   ```

2. Build the module:
   ```
   cd /path/to/anope-build
   make ms_mugserv
   ```

3. Add the following to your `services.conf` (see Configuration section below).

4. Load the module (or restart services):
   ```
   /msg OperServ MODLOAD ms_mugserv
   ```
   or via the shell:
   ```
   anoperc modload ms_mugserv
   ```

---

## Configuration

Add to `services.conf`:

```
module { name = "ms_mugserv"; }

service
{
    nick  = "MugServ"
    user  = "mugserv"
    host  = "services.example.net"
    gecos = "Mugging Service"
}

ms_mugserv
{
    /* Name of the service bot defined above. Default: MugServ */
    client = "MugServ"

    /* Channel to announce public mug/bet/bounty results.
     * Leave unset to send results only to the user via PM. */
    announce_channel = "#general"

    /* Space-separated list of nicks granted admin access to MugServ
     * admin commands. IRCops are always admins regardless of this list.
     * Example: admin_nicks = "Alice Bob" */
    # admin_nicks = "Nick1 Nick2"
}
```

### Options

| Option | Default | Description |
|---|---|---|
| `client` | `MugServ` | The service bot nick to bind commands to |
| `announce_channel` | *(empty)* | Channel for public game output; omit for PM-only |
| `admin_nicks` | *(empty)* | Space-separated extra admin nicks (beyond IRCops) |

---

## Database

Data is stored in `<anope datadir>/mugserv.db` as a flat text file. It is:

- Auto-saved every **5 minutes**
- Saved on module unload / services shutdown
- Saved whenever Anope triggers its own database save cycle

**Format:**
```
USER <account> <nick> <coins> <last_coins> <last_mug> <jail_until> <last_bet> <last_give> <last_bounty> <daily_given> <daily_reset> <inv0> <inv1> <inv2> <inv3> <inv4> <inv5> <inv6>
BOUNTY <account_key> <amount>
```

Timestamps are Unix epoch integers. Inventory values are item counts (0–3) in ITEMS table order: `mask knucks luckycoin vest cloak banana bail`.

> **Note:** The database is safe to back up at any time by copying the file. Do not edit it while services are running.

---

## Access Control

MugServ **requires a NickServ account**. Unidentified users receive:

> `MugServ: You must be identified with NickServ to play. Use: /msg NickServ IDENTIFY <password>`

Players are **automatically enrolled** the first time they successfully use any command — no `REGISTER` step is needed. Because data is keyed to the NickServ account (not the IRC nick), all grouped nicks share the same balance, inventory, and cooldowns.

---

## Player Commands

All commands are sent via `/msg MugServ <COMMAND>`.

### Economy

| Command | Description |
|---|---|
| `COINS` | Collect coins. Base gain: 15–75. Richer players also earn 5–15% of their current balance (capped at +1,500 extra). 10-minute cooldown. |
| `BALANCE [nick]` | Check your balance, or another player's balance. |
| `GIVE <nick> <amount>` | Transfer coins to another player. 5-minute cooldown; 500,000 coin daily cap per sender. |

### Mugging

| Command | Description |
|---|---|
| `MUG <nick>` | Attempt to mug a player. Costs 2 coins as a fee. |
| `ROB <nick>` | Alias for `MUG`. |
| `JAIL` | Check how long you remain in jail. |

**Mug outcomes (1–100 roll):**

| Outcome | Roll range | Effect |
|---|---|---|
| Success | 1–60 (base) | Steal 10–30% of victim's coins |
| Normal fail | 61–85 (base) | Lose 5–15% of your coins to the victim; +2 min extra cooldown |
| Critical fail | 86–100 (base) | Lose 20–40% of your coins; jailed for 10 minutes |
| Mega heist | 1% of successes | Extra +25% steal on top of normal steal |
| Oops-jail | 1% chance pre-roll | Instantly jailed before the mug even happens |

**Modifiers:**
- Success chance and steal % are increased by items (see Shop).
- Victims with >10,000 coins are whale-protected: steal is capped at 25% per mug.
- Critical fail loss is hard-capped at 250,000 coins; normal fail at 100,000.
- If you successfully mug someone with an active bounty, you claim the entire bounty pool as bonus coins.

### Gambling

| Command | Description |
|---|---|
| `BET <amount>` | 40% base chance to double your bet. Win = 2×; lose = 0. 60-second cooldown. |

### Bounties

| Command | Description |
|---|---|
| `BOUNTY <nick> <amount>` | Place a coin bounty on a player (costs you the coins). Min: 10, max: 100,000 per placement. 60-second cooldown. The bounty pool accumulates if multiple players pile on. |
| `BOUNTIES` | Show the top 10 active bounties. |

### Shop & Inventory

| Command | Description |
|---|---|
| `SHOP` | List all available items with prices and descriptions. |
| `BUY <key>` | Purchase an item. Maximum 3 of each item per player. |
| `INV` | View your current inventory. |
| `USE <key>` | Use a consumable item (currently only `bail`). Passive items activate automatically. |

---

## Items

All passive items stack up to 3 per player. Bonuses multiply with stack count.

| Key | Name | Price | Type | Effect |
|---|---|---|---|---|
| `mask` | Heist Mask | 120 | Passive (attacker) | +7% mug success chance per copy |
| `knucks` | Brass Knuckles | 250 | Passive (attacker) | +6% steal percentage per copy on a successful mug |
| `luckycoin` | Lucky Coin | 180 | Passive | +3 flat coins from `COINS` per copy; +7% `BET` win chance per copy |
| `vest` | Kevlar Vest | 220 | Passive (defender) | -20% coins stolen per copy (capped at 60% total reduction) |
| `cloak` | Shadow Cloak | 500 | Passive (defender) | 15% chance per copy to dodge a successful mug entirely (capped at 50%) |
| `banana` | Banana Peel | 50 | Passive (trap) | 5% chance per copy that an incoming mug triggers a critical fail on the attacker (capped at 25%) |
| `bail` | Bail Bondsman | 5,000 | Consumable | Frees you from jail instantly when used or purchased while jailed. Mug cooldown still applies. |

> **Stacking example:** 3× Kevlar Vests = 60% steal reduction (hard cap). 3× masks = +21% mug success (capped at 95% effective chance).

---

## Admin Commands

Admin access is granted to **IRCops** and any nicks listed in `admin_nicks` in `services.conf`.

| Command | Description |
|---|---|
| `MUGADD <nick> <amount>` | Add coins to a player's balance. |
| `MUGSET <nick> <amount>` | Set a player's balance to an exact amount. |
| `MUGTAKE <nick> <amount>` | Remove coins from a player (floored at 0). |
| `MUGRESET [confirm]` | **Destructive.** Resets all balances, inventories, cooldowns, and bounties to zero. Requires `confirm` argument to proceed. |
| `MUGSTATS` | Economy overview: total players, active bounties, total coins in circulation, top 5 leaderboard, announce channel, DB path. |

---

## Leaderboards

| Command | Description |
|---|---|
| `TOP5` | Top 5 richest players. |
| `TOP10` | Top 10 richest players (split into two lines). |

---

## Cooldown Reference

| Action | Cooldown |
|---|---|
| `COINS` | 10 minutes |
| `MUG` (success or crit fail) | 5 minutes |
| `MUG` (normal fail) | 5 minutes + 2 minutes extra = 7 minutes |
| `BET` | 60 seconds |
| `GIVE` | 5 minutes |
| `BOUNTY` | 60 seconds |
| Jail | 10 minutes (released by bail) |

---

## Tuning

All game constants are defined near the top of `ms_mugserv.cpp` as `static const` values. No recompile-time config file is needed — edit the constants and recompile:

```cpp
static const int  CD_COINS       = 600;   // COINS cooldown (seconds)
static const int  CD_MUG         = 300;   // MUG cooldown (seconds)
static const int  CD_JAIL        = 600;   // Jail duration (seconds)
static const int  COINS_MIN      = 15;    // Minimum base COINS gain
static const int  COINS_MAX      = 75;    // Maximum base COINS gain
static const int  SUCCESS_CHANCE = 60;    // % base mug success
static const int  FAIL_CHANCE    = 25;    // % normal fail (rest = crit)
static const int  BET_WIN_BASE   = 40;    // % base BET win chance
// ... and more
```

---

## Unloading / Reloading

```
/msg OperServ MODRELOAD ms_mugserv   # reload (re-reads services.conf)
/msg OperServ MODUNLOAD ms_mugserv   # unload (saves DB on shutdown)
```

The database is saved automatically on unload so no data is lost during a reload.

---

## Notes

- Player data is keyed to the **NickServ account name**, not the IRC nick. Nick changes and grouped nicks are transparent — the same account always has the same balance.
- The `announce_channel` receives one line per mug/bet/give/bounty action. If not set, results go only to the involved players via PM.
- There is no economy-wide inflation control; the admin `MUGSTATS` command shows total coins in circulation for manual oversight.
- The Bail Bondsman is the only consumable item. All other items are permanent passive bonuses until the admin resets them.
