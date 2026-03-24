# mug_game.py - Coin, mugging, gambling & item shop for Sopel (FULL EMOJI CHAOS)
# Storage: Sopel bot.db (with merge import from old JSON if present)
from __future__ import annotations

import os
import json
import random
import threading
import time
from contextlib import contextmanager
import unicodedata
import re

from sopel import module
from sopel.config.types import StaticSection, ValidatedAttribute


# ============================================================
# ===============  SOPEL CONFIG SECTION  =====================
# ============================================================

class MugGameSection(StaticSection):
    """[mug_game] config section for Sopel's .cfg file.

    Example::

        [mug_game]
        enabled = true
    """
    enabled = ValidatedAttribute('enabled', bool, default=True)


def setup(bot):
    """Called by Sopel when the plugin is loaded."""
    bot.config.define_section('mug_game', MugGameSection)


def configure(config):
    """Called by ``sopel-plugins configure``."""
    config.define_section('mug_game', MugGameSection)
    config.mug_game.configure_setting(
        'enabled',
        'Enable the mug game plugin? (true/false)',
    )


# ============================================================
# ===============  CONFIG & TUNING (EDIT ME)  ================
# ============================================================

PLUGIN_NAME = 'mug_game'
OLD_DATA_FILENAME = 'mug_game_coins.json'  # optional legacy import (in Sopel homedir)

# PM-admin allowlist (lowercase nicks)
# Add whoever should be able to use $mugadd/$mugset/$mugtake/$mugreset in PM.
ADMIN_NICKS = {"end3r"}

# Cooldowns (seconds)
COINS_COOLDOWN = 10 * 60
MUG_BASE_COOLDOWN = 5 * 60
MUG_EXTRA_FAIL_CD = 2 * 60
JAIL_TIME = 10 * 60
BET_COOLDOWN = 60
BOUNTY_COOLDOWN = 60  # prevents spam-bounty

# $coins base + scaling
COINS_MIN_GAIN = 15
COINS_MAX_GAIN = 75
COINS_SCALE_MIN_PCT = 5        # richer players get 5–15% of current money
COINS_SCALE_MAX_PCT = 15
COINS_SCALE_MAX_EXTRA = 1500   # cap the scaling gain so whales don't instantly go supernova

# Mugging economy
MUG_FEE = 2

# Mugging percent ranges
SUCCESS_STEAL_MIN = 10
SUCCESS_STEAL_MAX = 30
FAIL_LOSS_MIN = 5
FAIL_LOSS_MAX = 15
CRIT_LOSS_MIN = 20
CRIT_LOSS_MAX = 40

# Safeguards: cap how much a failed mug (or critical fail) can cost
# This prevents catastrophic multi-million losses on a single bad roll.
MAX_FAIL_LOSS = 100_000
MAX_CRIT_LOSS = 250_000

# Mugging chances (1–100 roll)
SUCCESS_CHANCE = 60
NORMAL_FAIL_CHANCE = 25
# remaining = critical fail

# Gambling
GAMBLE_MIN_BET = 1
BET_BASE_WIN_CHANCE = 40

# Anti-bullying / whale protection
RICH_VICTIM_THRESHOLD = 10_000
# Increase max steal pct so very wealthy victims are slightly easier to mug
# (was 10%). Adjust this if muggings against whales feel too weak.
RICH_VICTIM_MAX_STEAL_PCT = 25  # max steal per mug if victim is mega-rich

# Banana Peel item behavior
BANANA_SLIP_PER_ITEM_PCT = 5
BANANA_SLIP_MAX_PCT = 25
BANANA_SLIP_TRIGGERS_CRIT_FAIL = True

# Ultra-rare mug outcomes (turn these up if you want chaos)
MUG_MEGA_STEAL_CHANCE_PCT = 1      # 1% chance: mega steal mode triggers on a "success"
MUG_MEGA_STEAL_BONUS_PCT = 25      # adds +25% steal pct (still subject to caps/vest/etc)
MUG_OOPS_JAIL_CHANCE_PCT = 1       # 1% chance: instant oops-jail (even before roll)

# ============================================================
# ===================== TITLES (SANE STYLE) ==================
# ============================================================
# No title is shown for 0–49 coins.
# Titles are used in narration as: "{nick}, the {title}"
TITLE_THRESHOLDS = [
    (50, "🐀 Street Rat"),
    (200, "🔪 Pickpocket"),
    (750, "🧨 Menace"),
    (2500, "🦹 Thug"),
    (10000, "🕵️ Shadow Operative"),
    (50000, "💼 Crime Boss"),
    (250000, "🦈 Loan Shark"),
    (1000000, "👑 Underworld Kingpin"),
    (5000000, "🏛️ Crime Lord"),
]

# Anti-cheat settings
REQUIRE_IDENTIFIED = True      # Require NickServ identification for economy commands
GLOBAL_CMD_COOLDOWN = 3        # Min seconds between ANY economy command per user
GIVE_COOLDOWN = 5 * 60         # 5 min cooldown on $give
GIVE_DAILY_MAX = 500_000       # Max coins transferable per day via $give
GIVE_DAILY_WINDOW = 86400      # Rolling window for daily give cap (24h)

# Bounty settings
BOUNTY_MIN = 10
BOUNTY_MAX = 100_000  # sanity cap per bounty placement (pool can exceed)

# -----------------------
# Item shop configuration
# -----------------------
ITEMS = {
    "mask": {
        "name": "Heist Mask",
        "price": 120,
        "desc": "Boosts your mug success chance.",
        "mug_success_bonus": 7,
    },
    "knucks": {
        "name": "Brass Knuckles",
        "price": 250,
        "desc": "Steal more on successful mugs.",
        "mug_steal_bonus_pct": 6,
    },
    "luckycoin": {
        "name": "Lucky Coin",
        "price": 180,
        "desc": "Extra coins from $coins + better $bet odds.",
        "coins_bonus_flat": 3,
        "bet_win_bonus": 7,
    },
    "vest": {
        "name": "Kevlar Vest",
        "price": 220,
        "desc": "Reduces how much others can steal from you.",
        "steal_reduction_pct": 20,
    },
    "cloak": {
        "name": "Shadow Cloak",
        "price": 500,
        "desc": "Chance to dodge a successful mug entirely.",
        "mug_immune_chance": 15,
    },
    "banana": {
        "name": "Banana Peel",
        "price": 50,
        "desc": "Traps muggers: chance they slip into disaster.",
        "banana_slip_chance": BANANA_SLIP_PER_ITEM_PCT,  # per item
    },
    "bail": {
        "name": "Bail Bondsman",
        "price": 5000,
        "desc": "Pays your bail once to spring you out of jail. Use $use bail (PM). 🪓🎉",
    },
}

# ============================================================
# ===================== FUNNY MESSAGE POOLS ==================
# ============================================================

# ONLY for people who try to mug themselves (PM admin tools won't use these)
SELF_MUG_MESSAGES = [
    "😢 You can’t mug yourself. Therapy is cheaper. (Probably.)",
    "🪞 You stare into the mirror and threaten it. The mirror wins.",
    "🤡 You try to rob yourself and immediately call the cops on yourself.",
    "🧠 Galaxy brain move: attempted self-mug. Zero coins gained.",
    "🫠 You aggressively pat your own pockets. Nothing happens.",
    "📉 You almost stole from yourself. Almost.",
    "🚨 Crime rejected. Victim and attacker are the same idiot.",
    "🪙 You reach into your pocket… and feel shame.",
    "🎭 You put on a mask, scare yourself, and run away.",
    "🧘 Inner peace achieved. Crime aborted.",
]

COINS_COOLDOWN_MESSAGES = [
    "⏳ Whoa there, coin goblin! More coins in {time}.",
    "🪙 The coin gods are buffering… try again in {time}.",
    "🐌 Your greed is on cooldown. Return in {time}.",
    "💤 The coins are respawning… give them {time}.",
    "☕ Banker’s on break. Next appointment in {time}.",
    "👀 The IRS noticed you. Lay low for {time}.",
    "🧊 Your wallet needs to chill. Try again in {time}.",
    "🫠 You’re melting the economy. Pause {time}.",
    "🧃 Hydrate first. Coins in {time}.",
    "🧠 Your dopamine is rate-limited. Next hit in {time}.",
]

MUG_COOLDOWN_MESSAGES = [
    "🕵️ Lay low… the cops still remember your face. Try again in {time}.",
    "🚑 You’re still recovering. Mug again in {time}.",
    "👟 Your getaway shoes are untied. Fix them in {time}.",
    "📸 CCTV is still tracking you. Hide {time} longer.",
    "😵 Emotional damage cooldown: {time}.",
    "🎒 Crime backpack is reloading… {time} left.",
    "🧼 You’re still washing fingerprints off. {time}.",
    "🧯 Your criminal aura is cooling down. {time}.",
    "🫵 The neighborhood watch is staring at you. Wait {time}.",
    "🧠 Your evil plan needs another brain cell. {time}.",
]

BET_COOLDOWN_MESSAGES = [
    "🎲 The casino bouncer says “not yet.” Try again in {time}.",
    "🃏 Dealer’s shuffling… slowly. {time} left.",
    "💸 Your wallet is begging for mercy. Wait {time}.",
    "🥴 You’re still dizzy from the last bet. {time}.",
    "🎰 The slot machine overheated. Cooling for {time}.",
    "🤡 The clown dealer dropped all the chips. {time}.",
    "🍀 The four-leaf clover needs a nap — try again in {time}.",
    "🐇 Your rabbit's foot is on strike; luck resumes in {time}.",
    "🧿 The lucky charm's battery is low. Recharge: {time}.",
    "🎲 An RNG gremlin spilled the odds. Cleanup ETA: {time}.",
    "🥠 Fortune cookie production delayed. New cookie in {time}.",
    "🐢 The luck turtle is taking a scenic detour — back in {time}.",
    "🪙 Your lucky coin is under the sofa cushions; retrieval: {time}.",
    "🐈‍⬛ Karma cat is filing your fate; grooming finishes in {time}.",
    "🔮 The crystal ball is foggy. The seer will return in {time}.",
    "👻 Your luck ghost is debugging the haunt — try again in {time}.",
    "👻 Your luck ghost is recharging. {time}.",
    "📉 The economy needs a second after your antics. {time}.",
    "🧨 Your gambling fuse is lit. Wait {time} before you explode again.",
]

BROKE_MESSAGES = [
    "💸 You’re broke. Like, *emotionally* and financially.",
    "🪫 Wallet empty. Dreams empty. Try $coins.",
    "🧻 You can’t afford air right now. Earn coins first.",
    "🥲 Not enough coins. The streets are calling: $coins.",
    "🫠 Your bank account just said 'lol'.",
]

INVALID_MESSAGES = [
    "❓ That ain’t it, chief. Check $mughelp.",
    "🧠 Brain.exe stopped. Try using the command correctly.",
    "🧩 Missing pieces. See $mughelp for syntax.",
    "🤨 What are you *trying* to do? $mughelp explains it.",
    "🫵 Skill issue. Read $mughelp. (I’m kidding… mostly.)",
]

# Optional one-line “vibe prefix” (no names; keeps mug output ONE LINE)
MUG_VIBE_PREFIXES = ["👣", "🧲", "🧤", "🕶️", "🧃", "🌑", "🧿"]

MUG_FAIL_MESSAGES = [
    "💥 {att} slipped mid-mug and yeeted {loss} coins across the street!",
    "🤦 {att} tried to look intimidating but sneezed and dropped {loss} coins!",
    "🩴 {att} tripped over shoelaces and made it rain {loss} coins!",
    "🐈 {att} got ambushed by a random cat and scattered {loss} coins!",
    "🕺 {att} attempted a dramatic pose and breakdanced {loss} coins away!",
    "🐦 A pigeon dive-bombed {att}, causing panic-loss of {loss} coins!",
    "💨 {att} let one rip, panicked, and dropped {loss} coins in shame!",
    "🧼 {att} tried to wipe fingerprints but wiped out and lost {loss} coins!",
    "📦 {att} mugged a mailbox by accident and paid {loss} coins in fines!",
    "🧃 {att} spilled the crime juice and lost {loss} coins buying napkins!",
]

MUG_SUCCESS_MESSAGES = [
    "🦹 {att} jumps {vic} and snatches {steal} coins! 💼",
    "🧤 {att} runs {vic}'s pockets for {steal} coins! 🪙",
    "🏃 {att} hits-and-dips {vic} for {steal} coins! 💨",
    "🧛 {att} drains {vic} for {steal} coins! 🩸🪙",
    "🧲 {att} magnetizes {vic}'s wallet: {steal} coins fly out! 🧲",
    "🪤 {att} sets a trap for {vic} and walks away with {steal} coins! 🪙",
    "📉 {att} commits tax evasion on {vic}: +{steal} coins!",
    "🫳 {att} yoinks {steal} coins off {vic} like it’s casual. 😭",
]

MUG_MEGA_SUCCESS_MESSAGES = [
    "🌟 MEGA HEIST! {att} pulls a legendary swipe on {vic} for {steal} coins!! 🏆💰",
    "🔥 ULTRA MUG! {att} hits {vic} with main-character energy: {steal} coins! 🎬🪙",
    "⚡ GOD TIER YOINK! {att} extracts {steal} coins from {vic}'s soul!! 👻🪙",
]

MUG_CRIT_FAIL_MESSAGES = [
    "🚔 CRITICAL FAIL! {att} faceplants, drops {loss} coins, and gets tossed in jail for {jail}. 🔒",
    "🚨 {att} mugs the air, loses {loss} coins, and the police applauded… then arrested them. Jail: {jail}.",
    "🐕 {att} got tackled by a tiny dog, lost {loss} coins, and got booked. Jail: {jail}.",
    "📛 {att} left fingerprints on EVERYTHING, lost {loss} coins, and got detained. Jail: {jail}.",
    "🧯 {att} set off a silent alarm somehow, dropped {loss} coins, and got hauled away. Jail: {jail}.",
    "🧱 {att} ran into a wall they forgot existed, lost {loss} coins, and went to jail: {jail}.",
]

BOUNTY_CONFIRM_MESSAGES = [
    "🎯 Bounty placed on {vic} for {amt} coins. Somebody is getting *touched*. 😈",
    "🪙 You put {amt} coins on {vic}'s head. That’s… oddly motivational. 🗡️",
    "📛 {vic} is now WANTED for {amt} coins. Channel justice incoming. 🚨",
    "🧨 Bounty armed: {vic} ({amt} coins). Chaos will find them soon. 💥",
]

BOUNTY_CLAIM_MESSAGES = [
    "🎯 BOUNTY CLAIMED! {att} collects {bounty} bonus coins for mugging {vic}! 🏆",
    "💰 Payday! {att} cashes in a {bounty}-coin bounty on {vic}. 🪙🪙🪙",
    "🚨 WANTED DEAD OR POOR: {att} claims {bounty} coins from {vic}'s bounty! 😈",
]

# ============================================================
# ===================== INTERNAL (DON'T EDIT) =================
# ============================================================

# Per-channel runtime enable/disable toggles (persisted in bot.db)
# {channel_lower: bool}  — missing key = enabled (default)
_channel_toggles: dict[str, bool] | None = None  # None = not loaded yet

# Re-entrant lock: safe if helpers call helpers while locked
_data_lock = threading.RLock()
_data = None

# Conservative payload limit so we don't hit IRC's ~512 byte ceiling after prefixes.
SAFE_SAY_MAX_BYTES = 350


@contextmanager
def locked_data(bot):
    """Lock around any read-modify-write updates to avoid lost updates."""
    with _data_lock:
        data = _load_data(bot)
        yield data
        _save_data(bot)


def _rand(lst):
    return random.choice(lst)


def _utf8_len(s: str) -> int:
    return len(s.encode("utf-8"))


# Normalize nicknames/keys to avoid duplicate records from invisible
# characters, IRC formatting codes, or Unicode variants.
_MIRC_COLOR_RE = re.compile(r'\x03(?:\d{1,2}(?:,\d{1,2})?)?')
_IRC_CTRL_RE = re.compile(r'[\x00-\x1f\x7f-\x9f\u200b\u200c\u200d\uFEFF]')
_IRC_FORMAT_RE = re.compile(r'[\x02\x0f\x16\x1d\x1f]')


def normalize_nick(nick: str) -> str:
    if not nick:
        return ""
    s = str(nick)
    # Unicode compatibility decomposition to fold variant glyphs
    s = unicodedata.normalize('NFKC', s)
    # Remove mIRC color codes and other common IRC formatting/control chars
    s = _MIRC_COLOR_RE.sub('', s)
    s = _IRC_FORMAT_RE.sub('', s)
    s = _IRC_CTRL_RE.sub('', s)
    return s.strip()


def normalize_key(nick: str) -> str:
    return normalize_nick(nick).lower()


def _split_for_irc(text: str, max_bytes: int) -> list[str]:
    """
    Split a message into chunks that each fit within max_bytes (UTF-8).
    Prefer splitting on ' | ' (leaderboard/bounty style), then spaces, then hard-cut.
    """
    text = (text or "").strip()
    if not text:
        return []

    chunks: list[str] = []
    remaining = text

    while _utf8_len(remaining) > max_bytes:
        candidate = remaining
        while candidate and _utf8_len(candidate) > max_bytes:
            candidate = candidate[:-1]
        if not candidate:
            break

        cut = candidate.rfind(" | ")
        if cut == -1:
            cut = candidate.rfind(" ")
        if cut == -1 or cut < 10:
            part = candidate
            remaining = remaining[len(part):].lstrip()
        else:
            part = candidate[:cut].rstrip()
            remaining = remaining[cut:].lstrip()
            if remaining.startswith("|"):
                remaining = remaining[1:].lstrip()

        if part and part.strip():
            chunks.append(part)

    if remaining and remaining.strip():
        chunks.append(remaining)

    return chunks


def safe_say(bot, text: str, dest: str | None = None, max_bytes: int = SAFE_SAY_MAX_BYTES):
    """Say a message, splitting into multiple IRC-safe lines if needed."""
    parts = _split_for_irc(text, max_bytes=max_bytes)
    if not parts:
        return
    for part in parts:
        if dest is None:
            bot.say(part)
        else:
            bot.say(part, dest)


def _load_data(bot):
    """Load from bot.db, then merge old JSON balances if found."""
    global _data
    with _data_lock:
        if _data is not None:
            return _data

        data = bot.db.get_plugin_value(PLUGIN_NAME, 'data')
        if not isinstance(data, dict):
            data = {}

        data.setdefault("users", {})
        data.setdefault("bounties", {})         # {nick_lower: amount}
        data.setdefault("last_bounty", {})      # {nick_lower: timestamp}

        users = data["users"]

        # Merge old JSON if exists (only for users/money)
        try:
            json_path = os.path.join(bot.config.core.homedir, OLD_DATA_FILENAME)
            if os.path.isfile(json_path):
                with open(json_path, 'r') as f:
                    old = json.load(f)

                old_users = None
                if isinstance(old, dict):
                    if "users" in old and isinstance(old["users"], dict):
                        old_users = old["users"]
                    else:
                        old_users = old

                if isinstance(old_users, dict):
                    for key, rec in old_users.items():
                        if not isinstance(rec, dict):
                            continue
                        if key in users and isinstance(users[key], dict):
                            if rec.get("money", 0) > users[key].get("money", 0):
                                users[key]["money"] = rec.get("money", 0)
                        else:
                            users[key] = rec

                # Normalize inventory keys (lowercase) for all users after merge
                for u in users.values():
                    inv = u.get("inv")
                    if isinstance(inv, dict):
                        norm = {}
                        for k, v in inv.items():
                            if not k:
                                continue
                            lk = str(k).lower()
                            norm[lk] = norm.get(lk, 0) + int(v)
                        u["inv"] = norm

                bot.db.set_plugin_value(PLUGIN_NAME, 'data', data)
        except Exception:
            # Use Sopel's logger instead of print
            try:
                bot.logger.exception("mug_game: JSON merge failed")
            except Exception:
                pass

        _data = data
        return _data


def _save_data(bot):
    with _data_lock:
        if _data is None:
            return
        bot.db.set_plugin_value(PLUGIN_NAME, 'data', _data)


def get_user_record(bot, nick):
    data = _load_data(bot)
    users = data["users"]
    key = normalize_key(nick)
    if key not in users:
        users[key] = {
            "nick": nick,
            "money": 0,
            "last_coins": 0.0,
            "last_mug": 0.0,
            "jail_until": 0.0,
            "last_bet": 0.0,
            "last_give": 0.0,
            "inv": {},
        }
    else:
        u = users[key]
        u["nick"] = nick
        u.setdefault("money", 0)
        u.setdefault("last_coins", 0.0)
        u.setdefault("last_mug", 0.0)
        u.setdefault("jail_until", 0.0)
        u.setdefault("last_bet", 0.0)
        u.setdefault("last_give", 0.0)
        u.setdefault("inv", {})
    
    # Assign u for both new and existing users so normalization works
    u = users[key]
    
    # Normalize inventory keys to lowercase to avoid casing mismatches
    # Also cap each item at 3 max (migration for legacy users with 100+ stacks)
    try:
        inv = u.get("inv", {})
        if isinstance(inv, dict):
            norm = {}
            for k, v in inv.items():
                if not k:
                    continue
                lk = str(k).lower()
                count = norm.get(lk, 0) + int(v)
                norm[lk] = min(count, 3)  # cap each item at 3
            u["inv"] = norm
    except Exception:
        # Best-effort normalization; don't fail record access
        pass
    return users[key]


def fmt_time_remaining(seconds):
    seconds = int(max(0, seconds))
    if seconds < 60:
        return f"{seconds}s"
    minutes = seconds // 60
    sec = seconds % 60
    if minutes < 60:
        return f"{minutes}m {sec}s" if sec else f"{minutes}m"
    hours = minutes // 60
    minutes = minutes % 60
    return f"{hours}h {minutes}m" if minutes else f"{hours}h"


def fmt_coins(value) -> str:
    """Format coin amounts with commas for readability."""
    try:
        n = int(value)
    except (TypeError, ValueError):
        return str(value)
    return f"{n:,}"


def coins_cd_remaining(user, now):
    return max(0.0, (user.get("last_coins", 0.0) + COINS_COOLDOWN) - now)


def mug_cd_remaining(user, now):
    return max(0.0, (user.get("last_mug", 0.0) + MUG_BASE_COOLDOWN) - now)


def bet_cd_remaining(user, now):
    return max(0.0, (user.get("last_bet", 0.0) + BET_COOLDOWN) - now)


def give_cd_remaining(user, now):
    return max(0.0, (user.get("last_give", 0.0) + GIVE_COOLDOWN) - now)


def bounty_cd_remaining(data, nick, now):
    last = data["last_bounty"].get(normalize_key(nick), 0.0)
    return max(0.0, (last + BOUNTY_COOLDOWN) - now)


def get_item(key):
    return ITEMS.get(key.lower())


def get_item_count(user, item_key):
    return int(user.get("inv", {}).get(item_key, 0))


def get_item_bonus(user, attr):
    inv = user.get("inv", {})
    total = 0
    for k, count in inv.items():
        item = ITEMS.get(k)
        if not item:
            continue
        total += int(item.get(attr, 0)) * int(count)
    return total


# ---- Anti-cheat: global per-user command throttle ----
_last_cmd: dict[str, float] = {}
# ---- Anti-cheat: rolling $give tracker {nick_lower: [(timestamp, amount), ...]} ----
_give_history: dict[str, list[tuple[float, int]]] = {}
_give_history_loaded = False  # flag to avoid repeated DB loads

# Items that have active $use behavior; everything else is passive
USABLE_ITEMS = {'bail'}

# God mode: set of normalized nicks with near-guaranteed luck (toggled via $godmode)
_godmode: set[str] = set()


def _has_godmode(nick: str) -> bool:
    """Return True if the nick currently has god mode enabled."""
    return normalize_key(nick) in _godmode


def _check_identified(bot, trigger) -> bool:
    """Return True if user is identified with NickServ (or check is disabled).

    Tries multiple sources because trigger.account requires IRCv3 account-tag
    capability which many servers don't support:
      1. trigger.account  (IRCv3 account-tag on each PRIVMSG)
      2. bot.users[nick].account  (Sopel's WHO/WHOIS-based tracking)
      3. bot.channels[chan].privileges  (has channel privs → must be identified)
    If none of the above yield data, allow the command (no false lockouts).
    """
    if not REQUIRE_IDENTIFIED:
        return True

    nick = trigger.nick

    # 1) IRCv3 account-tag (best case)
    account = getattr(trigger, 'account', None)
    if account and account != '*':
        return True

    # 2) Sopel's internal user tracking (populated via WHO on join)
    try:
        user = bot.users.get(nick)
        if user:
            acct = getattr(user, 'account', None)
            if acct and acct != '*':
                return True
            # If Sopel explicitly recorded None/'*', user is NOT identified
            if acct is not None:
                return False
    except Exception:
        pass

    # 3) Channel privilege heuristic: if the user has any channel privileges
    #    (op, voice, etc.) they are almost certainly identified on most networks.
    try:
        chan_name = str(trigger.sender)
        if chan_name.startswith('#'):
            chan = bot.channels.get(chan_name)
            if chan:
                privs = getattr(chan, 'privileges', None)
                if isinstance(privs, dict):
                    v = privs.get(nick) or privs.get(nick.lower())
                    if v is None:
                        for k in privs:
                            if k.lower() == nick.lower():
                                v = privs[k]
                                break
                    if v and (isinstance(v, int) and v > 0):
                        return True
    except Exception:
        pass

    # 4) If we have no account data at all (server doesn't support account
    #    tracking), allow the command rather than locking out everyone.
    #    This means the check is effectively a no-op on networks without
    #    IRCv3 account-tag or WHOX, but that's better than a full lockout.
    if account is None:
        try:
            user = bot.users.get(nick)
            if user and getattr(user, 'account', 'UNSET') == 'UNSET':
                # Sopel has no account info at all → server doesn't track accounts
                return True
        except Exception:
            pass
        # Final fallback: no data available, allow through
        return True

    # account is explicitly '*' (not identified) and no privileges
    return False


def _check_global_cooldown(nick: str) -> bool:
    """Return True if the user is allowed to run a command (not flooding)."""
    now = time.time()
    key = normalize_key(nick)
    last = _last_cmd.get(key, 0.0)
    if now - last < GLOBAL_CMD_COOLDOWN:
        return False
    _last_cmd[key] = now
    return True


def _load_give_history(bot):
    """Load give history from bot.db on first use (persists across restarts)."""
    global _give_history_loaded
    if _give_history_loaded:
        return
    try:
        saved = bot.db.get_plugin_value(PLUGIN_NAME, 'give_history')
        if isinstance(saved, dict):
            now = time.time()
            cutoff = now - GIVE_DAILY_WINDOW
            for k, entries in saved.items():
                if isinstance(entries, list):
                    _give_history[k] = [(t, a) for t, a in entries if t > cutoff]
    except Exception:
        pass
    _give_history_loaded = True


def _save_give_history(bot):
    """Persist give history to bot.db."""
    try:
        bot.db.set_plugin_value(PLUGIN_NAME, 'give_history', _give_history)
    except Exception:
        pass


def _check_give_daily(bot, nick: str, amount: int, now: float) -> tuple[bool, int]:
    """Check if a $give would exceed the daily cap. Returns (allowed, remaining)."""
    _load_give_history(bot)
    key = normalize_key(nick)
    history = _give_history.get(key, [])
    # Prune entries outside the rolling window
    cutoff = now - GIVE_DAILY_WINDOW
    history = [(t, a) for t, a in history if t > cutoff]
    _give_history[key] = history
    total_given = sum(a for _, a in history)
    remaining = max(0, GIVE_DAILY_MAX - total_given)
    return (total_given + amount <= GIVE_DAILY_MAX), remaining


def _record_give(bot, nick: str, amount: int, now: float):
    """Record a successful $give for daily cap tracking (persisted to DB)."""
    _load_give_history(bot)
    key = normalize_key(nick)
    _give_history.setdefault(key, []).append((now, amount))
    _save_give_history(bot)


def _anticheat_gate(bot, trigger, cmd_name: str = "") -> bool:
    """Combined anti-cheat check. Returns True if command should STOP (blocked)."""
    if REQUIRE_IDENTIFIED and not _check_identified(bot, trigger):
        bot.reply("🔐 You must be identified with NickServ to play. Type: /msg NickServ IDENTIFY <password>")
        return True
    if not _check_global_cooldown(trigger.nick):
        return True  # silently drop rapid-fire commands
    return False


def _load_channel_toggles(bot) -> dict[str, bool]:
    """Lazily load per-channel toggles from bot.db."""
    global _channel_toggles
    if _channel_toggles is None:
        val = bot.db.get_plugin_value(PLUGIN_NAME, 'channel_toggles')
        _channel_toggles = val if isinstance(val, dict) else {}
    return _channel_toggles


def _plugin_enabled(bot, channel: str | None = None) -> bool:
    """Return True if the mug game is enabled for *channel*.

    If *channel* is None (PM context), only the config master switch is checked.
    """
    # Config master switch (hard kill)
    try:
        if not bot.config.mug_game.enabled:
            return False
    except Exception:
        pass  # no config section → default enabled

    # PM-only commands have no channel context → always allowed
    if channel is None:
        return True

    toggles = _load_channel_toggles(bot)
    # Missing key = enabled by default
    return toggles.get(channel.lower(), True)


def _set_channel_enabled(bot, channel: str, enabled: bool):
    """Set the per-channel enable/disable toggle (persisted across restarts)."""
    toggles = _load_channel_toggles(bot)
    toggles[channel.lower()] = enabled
    bot.db.set_plugin_value(PLUGIN_NAME, 'channel_toggles', toggles)


def _disabled_msg(bot, trigger):
    """Send a short 'plugin is disabled' notice."""
    chan = str(trigger.sender) if str(trigger.sender).startswith('#') else ''
    if chan:
        bot.reply(f"🔒 The mug game is disabled in {chan}.")
    else:
        bot.reply("🔒 The mug game is currently disabled.")


def _pm(bot, nick, text):
    bot.say(text, nick)


def _pm_only(trigger) -> bool:
    return not str(trigger.sender).startswith('#')


def _is_admin(bot, nick: str) -> bool:
    n = normalize_key(nick)

    # plugin allowlist
    if n in {normalize_key(x) for x in ADMIN_NICKS}:
        return True

    # Sopel core owner/admins (best-effort)
    core = getattr(getattr(bot, "config", None), "core", None)
    if core:
        owner = getattr(core, "owner", None)
        if isinstance(owner, str) and n == normalize_key(owner):
            return True
        admins = getattr(core, "admins", None)
        if isinstance(admins, (list, tuple, set)) and any(n == normalize_key(str(a)) for a in admins):
            return True

    return False


def get_title_for_money(money: int) -> str | None:
    """Return a title string (with emoji) for money >= 50, else None."""
    title = None
    for threshold, name in TITLE_THRESHOLDS:
        if money >= threshold:
            title = name
        else:
            break
    return title


def tag(nick: str, money: int) -> str:
    """
    Narrative-friendly name:
      - <50: "End3r"
      - >=50: "End3r, the 🐀 Street Rat"
    """
    title = get_title_for_money(money)
    if title:
        return f"{nick}, the {title}"
    return nick


def _is_in_channel(bot, channel_name: str, nick: str) -> bool:
    channel = bot.channels.get(channel_name)
    if channel is None:
        return True  # fail open if unknown (keeps original behavior)
    return nick in channel.users


# ============================================================
# ======================= CORE COMMANDS ======================
# ============================================================

@module.commands('coins')
def coins(bot, trigger):
    """$coins - get coins (scaled by wealth)"""
    if not _plugin_enabled(bot, trigger.sender):
        _disabled_msg(bot, trigger)
        return
    if not trigger.sender.startswith('#'):
        bot.reply("🏠 Use this in a channel.")
        return
    if _anticheat_gate(bot, trigger, 'coins'):
        return

    nick = trigger.nick
    now = time.time()

    with locked_data(bot):
        user = get_user_record(bot, nick)

        rem = coins_cd_remaining(user, now)
        if rem > 0:
            bot.reply(_rand(COINS_COOLDOWN_MESSAGES).format(time=fmt_time_remaining(rem)))
            return

        current = max(0, int(user.get("money", 0)))
        base_gain = random.randint(COINS_MIN_GAIN, COINS_MAX_GAIN)

        if current > 0:
            pct = random.randint(COINS_SCALE_MIN_PCT, COINS_SCALE_MAX_PCT) / 100.0
            scaling_gain = min(int(current * pct), COINS_SCALE_MAX_EXTRA)
        else:
            scaling_gain = 0

        bonus = min(50, get_item_bonus(user, "coins_bonus_flat"))  # cap at +50 coins
        gain = max(1, base_gain + scaling_gain + bonus)

        user["money"] = current + gain
        user["last_coins"] = now

        # Keep the response concise: only report total gain and new balance
        # Avoid titles and mentioning the command; just state what happened.
        bot.say(
            f"💰 {nick} found {fmt_coins(gain)} coins! New balance: {fmt_coins(user['money'])} coins. ✨"
        )


@module.commands('balance', 'bal')
def balance(bot, trigger):
    """$balance [nick] - check coins"""
    if not _plugin_enabled(bot, trigger.sender):
        _disabled_msg(bot, trigger)
        return
    if not trigger.sender.startswith('#'):
        bot.reply("🏠 Use this in a channel.")
        return
    if not _check_global_cooldown(trigger.nick):
        return  # silently drop spam

    arg = (trigger.group(2) or "").strip()
    target = arg.split()[0] if arg else trigger.nick
    u = get_user_record(bot, target)
    bot.say(f"🧾 {tag(u['nick'], u.get('money', 0))} has {fmt_coins(u.get('money', 0))} coins. 🪙")


@module.commands('give')
def give(bot, trigger):
    """$give <nick> <amount>"""
    if not _plugin_enabled(bot, trigger.sender):
        _disabled_msg(bot, trigger)
        return
    if not trigger.sender.startswith('#'):
        bot.reply("🏠 Use this in a channel.")
        return
    if _anticheat_gate(bot, trigger, 'give'):
        return

    args = (trigger.group(2) or "").split()
    if len(args) < 2:
        bot.reply("📦 Usage: $give <nick> <amount>")
        return

    target_nick = args[0]
    try:
        amt = int(args[1])
    except ValueError:
        bot.reply("🔢 Amount must be a whole number.")
        return

    if amt < 1:
        bot.reply("🪙 Amount must be at least 1.")
        return

    if normalize_key(target_nick) == normalize_key(trigger.nick):
        bot.reply("🙃 You can’t give yourself coins. That’s called *having coins*.")
        return

    if not _is_in_channel(bot, trigger.sender, target_nick):
        bot.reply("🕵️ That user doesn’t seem to be in this channel.")
        return

    with locked_data(bot):
        giver = get_user_record(bot, trigger.nick)

        # $give cooldown
        now = time.time()
        rem = give_cd_remaining(giver, now)
        if rem > 0:
            bot.reply(f"⏳ You can give again in {fmt_time_remaining(rem)}.")
            return

        if giver.get("money", 0) < amt:
            bot.reply(_rand(BROKE_MESSAGES))
            return

        recv = get_user_record(bot, target_nick)
        giver["money"] -= amt
        recv["money"] += amt
        giver["last_give"] = now

        bot.say(
            f"🤝 {tag(trigger.nick, giver['money'])} gave {fmt_coins(amt)} coins to {tag(recv['nick'], recv['money'])}! 🎉"
        )


# ============================================================
# ======================= JAIL STATUS ========================
# ============================================================

@module.commands('jail')
def jail(bot, trigger):
    """$jail - check your jail status"""
    if not _plugin_enabled(bot, trigger.sender):
        _disabled_msg(bot, trigger)
        return
    if not trigger.sender.startswith('#'):
        bot.reply("🏠 Use this in a channel.")
        return
    if not _check_global_cooldown(trigger.nick):
        return  # silently drop spam

    u = get_user_record(bot, trigger.nick)
    now = time.time()
    until = u.get("jail_until", 0.0)
    if until > now:
        rem = fmt_time_remaining(until - now)
        bot.say(f"🚔 {tag(trigger.nick, u.get('money', 0))} is doing time! Free in {rem}. 🔒")
    else:
        bot.say(f"✅ {tag(trigger.nick, u.get('money', 0))} is a free criminal once again. 😈")


# ============================================================
# ========================== BOUNTIES =========================
# ============================================================

@module.commands('bounty')
def bounty(bot, trigger):
    """$bounty <nick> <amount> - place a bounty (costs you coins)"""
    if not _plugin_enabled(bot, trigger.sender):
        _disabled_msg(bot, trigger)
        return
    if not trigger.sender.startswith('#'):
        bot.reply("🏠 Use this in a channel.")
        return
    if _anticheat_gate(bot, trigger, 'bounty'):
        return

    now = time.time()

    with locked_data(bot) as data:
        rem = bounty_cd_remaining(data, trigger.nick, now)
        if rem > 0:
            bot.reply(f"⏳ Slow down, bounty goblin. Try again in {fmt_time_remaining(rem)}.")
            return

        args = (trigger.group(2) or "").split()
        if len(args) < 2:
            bot.reply("🎯 Usage: $bounty <nick> <amount>")
            return

        target_nick = args[0]
        if normalize_key(target_nick) == normalize_key(trigger.nick):
            bot.reply("🫠 You can’t bounty yourself. That’s just therapy with extra steps.")
            return

        if not _is_in_channel(bot, trigger.sender, target_nick):
            bot.reply("🕵️ That user doesn’t seem to be in this channel.")
            return

        # Require target to be a known mug player (prevents dead-end bounties)
        if normalize_key(target_nick) not in data.get('users', {}):
            bot.reply("🗃️ That user isn't a known mug player yet. No point bounty-ing a ghost.")
            return

        try:
            amt = int(args[1])
        except ValueError:
            bot.reply("🔢 Amount must be a whole number.")
            return

        if amt < BOUNTY_MIN:
            bot.reply(f"🪙 Minimum bounty is {fmt_coins(BOUNTY_MIN)}. Stop being cheap.")
            return

        if amt > BOUNTY_MAX:
            bot.reply(f"🧯 Max bounty per placement is {fmt_coins(BOUNTY_MAX)}. Relax, Batman.")
            return

        placer = get_user_record(bot, trigger.nick)
        if placer.get("money", 0) < amt:
            bot.reply(_rand(BROKE_MESSAGES))
            return

        placer["money"] -= amt
        key = normalize_key(target_nick)
        data["bounties"][key] = int(data["bounties"].get(key, 0)) + amt
        data["last_bounty"][normalize_key(trigger.nick)] = now

        bot.say(
            _rand(BOUNTY_CONFIRM_MESSAGES).format(vic=target_nick, amt=fmt_coins(amt))
            + f" ({tag(trigger.nick, placer['money'])})"
        )


@module.commands('bounties')
def bounties(bot, trigger):
    """$bounties - one-line list of top bounties"""
    if not _plugin_enabled(bot, trigger.sender):
        _disabled_msg(bot, trigger)
        return
    if not trigger.sender.startswith('#'):
        bot.reply("🏠 Use this in a channel.")
        return

    data = _load_data(bot)
    b = data.get("bounties", {})
    if not b:
        bot.say("🎯 No active bounties. Everyone is (unfortunately) safe. 😇")
        return

    items = sorted(b.items(), key=lambda kv: -int(kv[1]))[:10]
    parts = [f"🎯 {nick}({fmt_coins(amt)})" for nick, amt in items]
    safe_say(bot, "🔥 Top bounties: " + " | ".join(parts))


# ============================================================
# ======================= MUGGING / ROB ======================
# ============================================================

def _check_mug_allowed(attacker):
    now = time.time()

    jail_until = attacker.get("jail_until", 0.0)
    if jail_until > now:
        return False, f"🚔 You’re still in jail for {fmt_time_remaining(jail_until - now)}. No crimes for now!"

    rem = mug_cd_remaining(attacker, now)
    if rem > 0:
        return False, _rand(MUG_COOLDOWN_MESSAGES).format(time=fmt_time_remaining(rem))

    if attacker.get("money", 0) < MUG_FEE:
        return False, _rand(BROKE_MESSAGES)

    return True, None


def _banana_slip_chance(victim):
    count = get_item_count(victim, "banana")
    if count <= 0:
        return 0
    chance = count * BANANA_SLIP_PER_ITEM_PCT
    return min(chance, BANANA_SLIP_MAX_PCT)


@module.commands('mug', 'rob')
def mug(bot, trigger):
    """$mug/$rob <nick> (ONE LINE OUTPUT ONLY)"""
    if not _plugin_enabled(bot, trigger.sender):
        _disabled_msg(bot, trigger)
        return
    if not trigger.sender.startswith('#'):
        bot.reply("🏠 Use this in a channel, not PM.")
        return
    if _anticheat_gate(bot, trigger, 'mug'):
        return

    arg = (trigger.group(2) or "").strip()
    if not arg:
        bot.reply("🗡️ Usage: $mug <nick>")
        return

    target_nick = arg.split()[0]
    attacker_nick = trigger.nick

    # Self-mug: random smartass roast; bot.say avoids "nick:" reply prefix
    if normalize_key(target_nick) == normalize_key(attacker_nick):
        bot.say(_rand(SELF_MUG_MESSAGES))
        return

    now = time.time()
    prefix = _rand(MUG_VIBE_PREFIXES)

    with locked_data(bot) as data:
        attacker = get_user_record(bot, attacker_nick)

        # Allow mugging from anywhere, but only if the target already exists in the mug DB.
        # This prevents creating new victim records just because someone typed a nick.
        users = data.get("users", {})
        victim_key = normalize_key(target_nick)
        if victim_key not in users:
            bot.reply("🗃️ That user isn’t a known mug player yet.")
            return

        victim = get_user_record(bot, target_nick)

        # Snapshot display names ONCE so titles don't change mid-message.
        att_display = tag(attacker_nick, int(attacker.get("money", 0)))
        vic_display = tag(target_nick, int(victim.get("money", 0)))

        allowed, msg = _check_mug_allowed(attacker)
        if not allowed:
            bot.reply(msg)
            return

        def do_crit_fail(reason_text=None):
            att_money = int(attacker.get("money", 0))
            pct = random.randint(CRIT_LOSS_MIN, CRIT_LOSS_MAX) / 100.0
            loss = max(1, int(att_money * pct)) if att_money > 0 else 0
            # Cap critical-fail loss to avoid catastrophic single-roll losses
            loss = min(loss, att_money, MAX_CRIT_LOSS)

            attacker["money"] = att_money - loss
            victim["money"] = int(victim.get("money", 0)) + loss
            attacker["jail_until"] = now + JAIL_TIME
            attacker["last_mug"] = now

            # Check if attacker has bail in inventory - auto-release if they do
            inv = attacker.get("inv", {})
            bail_count = int(inv.get("bail", 0))
            if bail_count > 0:
                inv["bail"] = bail_count - 1
                attacker["jail_until"] = 0.0
                # NOTE: mug cooldown intentionally NOT cleared — bail frees from
                # jail but does not grant an instant re-mug (anti-exploit).
                extra = f" {reason_text}" if reason_text else ""
                msg2 = _rand(MUG_CRIT_FAIL_MESSAGES).format(
                    att=att_display,
                    loss=fmt_coins(loss),
                    jail=fmt_time_remaining(JAIL_TIME),
                )
                bot.say(
                    f"{prefix} {msg2}{extra} 🪓 BUT {attacker_nick} had a Bail Bondsman and got out of jail! (Mug cooldown still active.) | {attacker_nick}: {fmt_coins(attacker['money'])} | {target_nick}: {fmt_coins(victim['money'])} 💼"
                )
                return

            extra = f" {reason_text}" if reason_text else ""
            msg2 = _rand(MUG_CRIT_FAIL_MESSAGES).format(
                att=att_display,
                loss=fmt_coins(loss),
                jail=fmt_time_remaining(JAIL_TIME),
            )
            bot.say(
                f"{prefix} {msg2}{extra} | {attacker_nick}: {fmt_coins(attacker['money'])} | {target_nick}: {fmt_coins(victim['money'])} 🪙"
            )
            return

        # Ultra-rare oops-jail (happens even before the roll) — godmode users are immune
        if not _has_godmode(attacker_nick) and random.randint(1, 100) <= MUG_OOPS_JAIL_CHANCE_PCT:
            do_crit_fail(reason_text="🤡 ULTRA-RARE OOPS: you looked suspicious and got arrested instantly.")
            return

        # Charge the mug fee now (don't charge if instant-oops jailed)
        attacker["money"] = max(0, int(attacker.get("money", 0)) - MUG_FEE)

        # Mug chance mods
        bonus_success = get_item_bonus(attacker, "mug_success_bonus")
        # Admins with god mode get near-guaranteed success
        if _has_godmode(attacker_nick):
            success_chance = 99
        else:
            success_chance = min(95, SUCCESS_CHANCE + bonus_success)

        roll = random.randint(1, 100)
        success_max = success_chance
        normal_fail_max = success_max + NORMAL_FAIL_CHANCE

        # SUCCESS PATH
        if roll <= success_max:
            # Banana trap check (victim)
            slip = _banana_slip_chance(victim)
            if not _has_godmode(attacker_nick) and slip > 0 and random.randint(1, 100) <= slip and BANANA_SLIP_TRIGGERS_CRIT_FAIL:
                do_crit_fail(reason_text=f"🍌 Banana trap! {target_nick} had {slip}% slip chance.")
                return

            # Cloak dodge check (victim) - cap at 50% max to prevent 100% immunity
            immune = min(50, get_item_bonus(victim, "mug_immune_chance"))
            if immune > 0 and random.randint(1, 100) <= immune:
                attacker["last_mug"] = now
                bot.say(
                    f"{prefix} 🕶️ {att_display} tries to mug {vic_display}, but {target_nick} vanishes into the shadows. No coins stolen! 👻 | {attacker_nick}: {fmt_coins(attacker['money'])} | {target_nick}: {fmt_coins(victim.get('money', 0))}"
                )
                return

            vic_money = int(victim.get("money", 0))
            if vic_money <= 0:
                attacker["last_mug"] = now
                bot.say(
                    f"{prefix} 🪫 {att_display} tried to mug {vic_display}, but {target_nick} is broke as a joke. Nothing to steal! 🤷 | {attacker_nick}: {fmt_coins(attacker['money'])} | {target_nick}: {fmt_coins(victim.get('money', 0))}"
                )
                return

            pct_base = random.randint(SUCCESS_STEAL_MIN, SUCCESS_STEAL_MAX)
            pct_bonus = min(30, get_item_bonus(attacker, "mug_steal_bonus_pct"))  # cap bonus at +30%

            mega = (random.randint(1, 100) <= MUG_MEGA_STEAL_CHANCE_PCT)
            if mega:
                steal_pct = max(0, pct_base + pct_bonus + MUG_MEGA_STEAL_BONUS_PCT)
            else:
                steal_pct = max(0, pct_base + pct_bonus)

            # Whale protection: cap max steal
            rich_cap = None
            if vic_money > RICH_VICTIM_THRESHOLD:
                rich_cap = max(1, int(vic_money * (RICH_VICTIM_MAX_STEAL_PCT / 100.0)))

            steal_raw = max(1, int(vic_money * (steal_pct / 100.0)))
            if rich_cap is not None:
                steal_raw = min(steal_raw, rich_cap)

            # Vest reduces stolen amount - cap at 60% max reduction
            reduction = min(60, get_item_bonus(victim, "steal_reduction_pct"))
            if reduction > 0:
                steal = max(1, int(steal_raw * max(0, (100 - reduction)) / 100.0))
            else:
                steal = steal_raw

            victim["money"] = max(0, vic_money - steal)
            attacker["money"] = int(attacker.get("money", 0)) + steal

            # Bounty claim (explicit mutation)
            bounty_key = normalize_key(target_nick)
            bounty_pool = int(data["bounties"].get(bounty_key, 0))
            bounty_claim = 0
            if bounty_pool > 0:
                bounty_claim = bounty_pool
                data["bounties"].pop(bounty_key, None)
                attacker["money"] += bounty_claim

            attacker["last_mug"] = now

            if mega:
                msg = _rand(MUG_MEGA_SUCCESS_MESSAGES).format(att=att_display, vic=vic_display, steal=fmt_coins(steal))
            else:
                msg = _rand(MUG_SUCCESS_MESSAGES).format(att=att_display, vic=vic_display, steal=fmt_coins(steal))

            whale_note = " 🐋(whale-protected)" if rich_cap is not None else ""
            bounty_note = ""
            if bounty_claim:
                bounty_note = " " + _rand(BOUNTY_CLAIM_MESSAGES).format(
                    att=attacker_nick, vic=target_nick, bounty=fmt_coins(bounty_claim)
                )

            bot.say(
                f"{prefix} {msg}{whale_note}{bounty_note} | {attacker_nick}: {fmt_coins(attacker['money'])} | {target_nick}: {fmt_coins(victim['money'])} 💼"
            )
            return

        # NORMAL FAIL PATH
        if roll <= normal_fail_max:
            att_money = int(attacker.get("money", 0))
            pct = random.randint(FAIL_LOSS_MIN, FAIL_LOSS_MAX) / 100.0
            loss = max(1, int(att_money * pct))
            # Cap normal fail loss so a single failed mug can't bankrupt someone for millions
            loss = min(loss, att_money, MAX_FAIL_LOSS)

            attacker["money"] = max(0, att_money - loss)
            victim["money"] = int(victim.get("money", 0)) + loss
            # store last_mug in the future to extend cooldown (base + extra)
            attacker["last_mug"] = now + MUG_EXTRA_FAIL_CD

            msg = _rand(MUG_FAIL_MESSAGES).format(att=att_display, loss=fmt_coins(loss))
            bot.say(
                f"{prefix} {msg} | {attacker_nick}: {fmt_coins(attacker['money'])} | {target_nick}: {fmt_coins(victim['money'])} 🤡"
            )
            return

        # CRITICAL FAIL PATH
        do_crit_fail()


# ============================================================
# ========================== BETTING =========================
# ============================================================

# Keep betting as a percentage chance, but use a larger roll range to reduce
# the "small sample" feel and allow finer granularity if needed later.
BET_ROLL_MAX = 10000

@module.commands('bet')
def bet(bot, trigger):
    """$bet <amount>"""
    if not _plugin_enabled(bot, trigger.sender):
        _disabled_msg(bot, trigger)
        return
    if not trigger.sender.startswith('#'):
        bot.reply("🏠 Use this in a channel.")
        return
    if _anticheat_gate(bot, trigger, 'bet'):
        return

    arg = (trigger.group(2) or "").strip()
    if not arg:
        bot.reply("🎲 Usage: $bet <amount>")
        return

    try:
        amount = int(arg.split()[0])
    except ValueError:
        bot.reply("🔢 Bet amount must be a whole number.")
        return

    if amount < 1 or amount < GAMBLE_MIN_BET:
        bot.reply(f"💁 Minimum bet is {fmt_coins(GAMBLE_MIN_BET)}.")
        return



    now = time.time()

    with locked_data(bot):
        user = get_user_record(bot, trigger.nick)

        rem = bet_cd_remaining(user, now)
        if rem > 0:
            bot.reply(_rand(BET_COOLDOWN_MESSAGES).format(time=fmt_time_remaining(rem)))
            return

        if int(user.get("money", 0)) < amount:
            bot.reply(_rand(BROKE_MESSAGES))
            return

        user["money"] = int(user.get("money", 0)) - amount

        bonus = get_item_bonus(user, "bet_win_bonus")
        # God mode gets near-guaranteed wins
        if _has_godmode(trigger.nick):
            win_chance = 99
        else:
            win_chance = min(95, BET_BASE_WIN_CHANCE + bonus)

        # Roll 1..10000; win_chance is still a percent.
        roll = random.randint(1, BET_ROLL_MAX)
        win = roll <= (win_chance * (BET_ROLL_MAX // 100))
        user["last_bet"] = now

        if win:
            payout = amount * 2
            user["money"] += payout
            bot.say(
                f"🎲 {tag(trigger.nick, user['money'])} bets {fmt_coins(amount)} and WINS! Payout: {fmt_coins(payout)}. New balance: {fmt_coins(user['money'])} 🤑✨"
            )
        else:
            bot.say(
                f"💀 {tag(trigger.nick, user['money'])} bets {fmt_coins(amount)} and loses it all! New balance: {fmt_coins(user['money'])} 😭🎰"
            )


# ============================================================
# ======================= SHOP & INVENTORY ===================
# ============================================================

@module.commands('shop')
def shop(bot, trigger):
    """$shop (PM or channel)"""
    chan = str(trigger.sender) if str(trigger.sender).startswith('#') else None
    if not _plugin_enabled(bot, chan):
        _disabled_msg(bot, trigger)
        return
    nick = trigger.nick
    if trigger.sender.startswith('#'):
        bot.say(f"🛒 {nick}: check your PM for the shop list.")

    _pm(bot, nick, "🛒 Welcome to the Crime Shop! Use $buy <itemkey> to purchase. 😈")
    for key, item in ITEMS.items():
        _pm(bot, nick, f"{key} → {item['name']} ({fmt_coins(item['price'])} coins) – {item.get('desc', '')}")


@module.commands('buy')
def buy(bot, trigger):
    """$buy <itemkey> (PM)"""
    chan = str(trigger.sender) if str(trigger.sender).startswith('#') else None
    if not _plugin_enabled(bot, chan):
        _disabled_msg(bot, trigger)
        return
    if _anticheat_gate(bot, trigger, 'buy'):
        return

    nick = trigger.nick
    args = (trigger.group(2) or "").strip()
    if not args:
        _pm(bot, nick, "🛒 Usage: $buy <itemkey>  |  Use $shop to see keys.")
        return

    key = args.split()[0].lower()
    item = get_item(key)
    if not item:
        _pm(bot, nick, f"❓ No such item '{key}'. Use $shop to see valid keys.")
        return

    price = int(item.get("price", 0))

    with locked_data(bot):
        user = get_user_record(bot, nick)
        if int(user.get("money", 0)) < price:
            _pm(bot, nick, f"💸 Not enough coins for {item['name']} (costs {fmt_coins(price)}). Go farm $coins like a goblin. 🪙")
            return

        inv = user.setdefault("inv", {})
        current_count = int(inv.get(key, 0))
        
        if current_count >= 3:
            _pm(bot, nick, f"🚫 You already have 3 {item['name']}s. That's the max allowed! (Stack limit: 3)")
            return

        user["money"] = int(user.get("money", 0)) - price
        inv[key] = current_count + 1

        # Auto-consume bail if bought while jailed
        if key == 'bail' and int(user.get('jail_until', 0)) > time.time():
            # consume one bail immediately but keep mug cooldown (anti-exploit)
            inv[key] = max(0, int(inv.get(key, 0)) - 1)
            user['jail_until'] = 0.0
            _pm(bot, nick, f"🪓🎉 {nick} bought {item['name']} for {fmt_coins(price)} coins and got bailed out! Welcome back to freedom. (Mug cooldown still active.) New balance: {fmt_coins(user['money'])} 🧾✨")
        else:
            _pm(bot, nick, f"✅ Bought {item['name']} for {fmt_coins(price)} coins! New balance: {fmt_coins(user['money'])} 🧾✨")


@module.commands('inv', 'inventory')
def inventory(bot, trigger):
    """$inv (PM)"""
    chan = str(trigger.sender) if str(trigger.sender).startswith('#') else None
    if not _plugin_enabled(bot, chan):
        _disabled_msg(bot, trigger)
        return
    nick = trigger.nick
    user = get_user_record(bot, nick)
    inv = user.get("inv", {})

    if not inv:
        _pm(bot, nick, "🎒 Your inventory is empty. You’re basically a civilian. Use $shop. 😭")
        return

    _pm(bot, nick, "🎒 Your inventory:")
    for k, c in inv.items():
        item = get_item(k)
        if not item:
            continue
        _pm(bot, nick, f"- {item['name']} x{c} ({k}) – {item.get('desc', '')}")


# PM: use an item (e.g., bail)
@module.commands('use')
def use_item(bot, trigger):
    """PM: $use <itemkey> — consume an item (e.g., `bail`)"""
    if not _plugin_enabled(bot, None):
        _disabled_msg(bot, trigger)
        return
    if not _pm_only(trigger):
        bot.reply("📩 Use this in PM, not in-channel.")
        return
    if _anticheat_gate(bot, trigger, 'use'):
        return

    nick = trigger.nick
    args = (trigger.group(2) or "").strip()
    if not args:
        _pm(bot, nick, "🧰 Usage: $use <itemkey> — use an item from your inventory.")
        return

    key = args.split()[0].lower()
    item = get_item(key)
    if not item:
        _pm(bot, nick, f"❓ No such item '{key}'. Use $inv to see your items.")
        return

    with locked_data(bot):
        user = get_user_record(bot, nick)
        inv = user.get('inv', {})
        count = int(inv.get(key, 0))
        if count <= 0:
            _pm(bot, nick, f"🪫 You don't have any {item['name']} ({key}). Use $shop to buy one.")
            return

        # Bail behavior: release from jail
        if key == 'bail':
            if int(user.get('jail_until', 0)) > time.time():
                # Consume one and clear jail, but keep mug cooldown (anti-exploit)
                inv[key] = count - 1
                user['jail_until'] = 0.0
                _pm(bot, nick, f"🪓🎉 {nick} used {item['name']} and was BAIL-IFIED! You're free! (Mug cooldown still active.) Stay out of trouble. 💸")
            else:
                _pm(bot, nick, f"🤷 {nick}, you're not in jail. Your {item['name']} stays in your pocket for when you need it. Use it wisely! 🪙")
            return

        # Passive items cannot be consumed — they provide bonuses just by existing in inventory
        if key not in USABLE_ITEMS:
            _pm(bot, nick, f"🛡️ {item['name']} is a passive item — it works automatically from your inventory. No need to use it!")
            return

        # Consume one for other usable items (future-proofed)
        inv[key] = count - 1
        _pm(bot, nick, f"✅ Used {item['name']} x1. ({item.get('desc','')})")


# ============================================================
# =================== LEADERBOARDS (ONE LINE) =================
# ============================================================

def _get_leaderboard(bot, limit):
    data = _load_data(bot)
    users = data.get("users", {})
    sorted_users = sorted(users.values(), key=lambda u: (-int(u.get("money", 0)), u.get("nick", "").lower()))
    return sorted_users[:limit]


def _format_lb(users, start_rank=1):
    """
    Format leaderboard entries with correct global rank.
    🥇🥈🥉 are reserved for ranks 1–3; the rest use #N.
    """
    parts = []
    for i, u in enumerate(users):
        rank = start_rank + i
        if rank == 1:
            prefix = "🥇"
        elif rank == 2:
            prefix = "🥈"
        elif rank == 3:
            prefix = "🥉"
        else:
            prefix = f"#{rank}"
        parts.append(f"{prefix} {u.get('nick','?')}({fmt_coins(u.get('money',0))})")
    return " | ".join(parts)


@module.commands('top5')
def top5(bot, trigger):
    if not _plugin_enabled(bot, trigger.sender):
        _disabled_msg(bot, trigger)
        return
    top = _get_leaderboard(bot, 5)
    if not top:
        bot.say("📉 No coin data yet. Go earn some with $coins! 💰")
        return
    safe_say(bot, f"🏆 Top 5 coin hoarders: {_format_lb(top, start_rank=1)}")


@module.commands('top10')
def top10(bot, trigger):
    if not _plugin_enabled(bot, trigger.sender):
        _disabled_msg(bot, trigger)
        return
    top = _get_leaderboard(bot, 10)
    if not top:
        bot.say("📉 No coin data yet. Go earn some with $coins! 💰")
        return

    safe_say(bot, f"💰 Top 10 coin legends (1–5): {_format_lb(top[:5], start_rank=1)}")
    safe_say(bot, f"💰 Top 10 coin legends (6–10): {_format_lb(top[5:10], start_rank=6)}")


# ============================================================
# ===================== PM ADMIN COMMANDS ====================
# ============================================================

@module.commands('mugadd')
def mugadd(bot, trigger):
    """PM: $mugadd <nick> <amount>  (admin only)"""
    if not _pm_only(trigger):
        bot.reply("📩 Use this in PM, not in-channel.")
        return
    if not _is_admin(bot, trigger.nick):
        _pm(bot, trigger.nick, "🚫 Nice try. You’re not an admin.")
        return

    args = (trigger.group(2) or "").split()
    if len(args) < 2:
        _pm(bot, trigger.nick, "Usage: $mugadd <nick> <amount>")
        return

    target = args[0]
    try:
        amt = int(args[1])
    except ValueError:
        _pm(bot, trigger.nick, "🔢 Amount must be a whole number.")
        return
    if amt <= 0:
        _pm(bot, trigger.nick, "🧠 Amount must be > 0.")
        return

    with locked_data(bot):
        u = get_user_record(bot, target)
        u["money"] = int(u.get("money", 0)) + amt

    _pm(bot, trigger.nick, f"✅ Added {fmt_coins(amt)} coins to {u['nick']}. New balance: {fmt_coins(u['money'])} 🪙")


@module.commands('mugset')
def mugset(bot, trigger):
    """PM: $mugset <nick> <amount>  (admin only)"""
    if not _pm_only(trigger):
        bot.reply("📩 Use this in PM, not in-channel.")
        return
    if not _is_admin(bot, trigger.nick):
        _pm(bot, trigger.nick, "🚫 Nice try. You’re not an admin.")
        return

    args = (trigger.group(2) or "").split()
    if len(args) < 2:
        _pm(bot, trigger.nick, "Usage: $mugset <nick> <amount>")
        return

    target = args[0]
    try:
        amt = int(args[1])
    except ValueError:
        _pm(bot, trigger.nick, "🔢 Amount must be a whole number.")
        return
    if amt < 0:
        _pm(bot, trigger.nick, "🧠 Amount must be >= 0.")
        return

    with locked_data(bot):
        u = get_user_record(bot, target)
        u["money"] = amt

    _pm(bot, trigger.nick, f"✅ Set {u['nick']}'s balance to {fmt_coins(u['money'])} 🪙")


@module.commands('mugtake')
def mugtake(bot, trigger):
    """PM: $mugtake <nick> <amount>  (admin only)"""
    if not _pm_only(trigger):
        bot.reply("📩 Use this in PM, not in-channel.")
        return
    if not _is_admin(bot, trigger.nick):
        _pm(bot, trigger.nick, "🚫 Nice try. You’re not an admin.")
        return

    args = (trigger.group(2) or "").split()
    if len(args) < 2:
        _pm(bot, trigger.nick, "Usage: $mugtake <nick> <amount>")
        return

    target = args[0]
    try:
        amt = int(args[1])
    except ValueError:
        _pm(bot, trigger.nick, "🔢 Amount must be a whole number.")
        return
    if amt <= 0:
        _pm(bot, trigger.nick, "🧠 Amount must be > 0.")
        return

    with locked_data(bot):
        u = get_user_record(bot, target)
        cur = int(u.get("money", 0))
        u["money"] = max(0, cur - amt)

    _pm(bot, trigger.nick, f"✅ Took {fmt_coins(amt)} coins from {u['nick']}. New balance: {fmt_coins(u['money'])} 🪙")


@module.commands('mugreset')
def mugreset(bot, trigger):
    """PM: $mugreset  (admin only) - resets ALL users + bounties + cooldowns + inventories"""
    if not _pm_only(trigger):
        bot.reply("📩 Use this in PM, not in-channel.")
        return
    if not _is_admin(bot, trigger.nick):
        _pm(bot, trigger.nick, "🚫 Nice try. You’re not an admin.")
        return

    with locked_data(bot) as data:
        # reset all user records
        for u in data.get("users", {}).values():
            if not isinstance(u, dict):
                continue
            u["money"] = 0
            u["last_coins"] = 0.0
            u["last_mug"] = 0.0
            u["jail_until"] = 0.0
            u["last_bet"] = 0.0
            u["last_give"] = 0.0
            u["inv"] = {}

        # reset bounties
        data["bounties"] = {}
        data["last_bounty"] = {}

    _pm(bot, trigger.nick, "🧨 FULL RESET DONE. Everyone is broke again. Society restored. ✅")


@module.commands('mugcleardb')
def mugcleardb(bot, trigger):
    """PM: $mugcleardb confirm  (admin only) - deletes ALL user records + bounties from storage"""
    if not _pm_only(trigger):
        bot.reply("📩 Use this in PM, not in-channel.")
        return
    if not _is_admin(bot, trigger.nick):
        _pm(bot, trigger.nick, "🚫 Nice try. You’re not an admin.")
        return

    args = (trigger.group(2) or "").strip().lower()
    if args != "confirm":
        _pm(bot, trigger.nick, "⚠️ This permanently wipes ALL mug player records (money/cooldowns/inv) and bounties.")
        _pm(bot, trigger.nick, "Usage: $mugcleardb confirm")
        return

    global _data
    with _data_lock:
        # Replace in-memory cache with a clean default structure.
        _data = {"users": {}, "bounties": {}, "last_bounty": {}}

        # Persist wipe to Sopel's DB. Prefer deletion if supported; otherwise overwrite.
        try:
            db = getattr(bot, "db", None)
            if db is not None and hasattr(db, "delete_plugin_value"):
                db.delete_plugin_value(PLUGIN_NAME, 'data')
            else:
                bot.db.set_plugin_value(PLUGIN_NAME, 'data', _data)
        except Exception:
            # Best-effort fallback: overwrite.
            try:
                bot.db.set_plugin_value(PLUGIN_NAME, 'data', _data)
            except Exception:
                pass

    _pm(bot, trigger.nick, "🧹 DB WIPE DONE. All mug player records have been deleted. ✅")


@module.commands('mugmerge')
def mugmerge(bot, trigger):
    """PM: $mugmerge <nick> — merge duplicate records for normalized nick (admin only)"""
    if not _pm_only(trigger):
        bot.reply("📩 Use this in PM, not in-channel.")
        return
    if not _is_admin(bot, trigger.nick):
        _pm(bot, trigger.nick, "🚫 Nice try. You’re not an admin.")
        return
    raw = (trigger.group(2) or "").strip()
    args = raw.split()
    if not args:
        _pm(bot, trigger.nick, "Usage: $mugmerge <nick> [--dry]")
        return

    # allow flags: --dry or trailing 'dry'
    dry = False
    flags = [a for a in args if a.startswith('--') or a.lower() == 'dry']
    if '--dry' in args or 'dry' in [a.lower() for a in args]:
        dry = True

    # pick first non-flag token as target
    target = next((a for a in args if not a.startswith('--') and a.lower() != 'dry'), None)
    if not target:
        _pm(bot, trigger.nick, "Usage: $mugmerge <nick> [--dry]")
        return

    nk = normalize_key(target)

    with locked_data(bot) as data:
        users = data.get('users', {})
        # Find keys whose stored display name normalizes to the same key
        matches = [k for k, u in users.items() if normalize_key(u.get('nick', '')) == nk]
        if not matches:
            _pm(bot, trigger.nick, f"No records found matching '{target}'.")
            return

        if dry:
            total_money = sum(int(users.get(k, {}).get('money', 0)) for k in matches)
            combined_inv = {}
            for k in matches:
                for itemk, cnt in users.get(k, {}).get('inv', {}).items():
                    combined_inv[itemk] = combined_inv.get(itemk, 0) + int(cnt)

            _pm(bot, trigger.nick, f"Dry-run: would merge {len(matches)} records for normalized key '{nk}':")
            for k in matches:
                u = users.get(k, {})
                _pm(bot, trigger.nick, f" - stored key: {k} | nick: {u.get('nick','?')} | money: {fmt_coins(u.get('money',0))}")

            _pm(bot, trigger.nick, f"Combined total money: {fmt_coins(total_money)}")
            if combined_inv:
                _pm(bot, trigger.nick, "Combined inventory summary:")
                for itemk, cnt in combined_inv.items():
                    _pm(bot, trigger.nick, f" - {itemk}: x{cnt}")
            _pm(bot, trigger.nick, "Run '$mugmerge <nick>' (without --dry) to perform the merge.")
            return

        # Non-dry merge: perform the merge into canonical nk
        # Ensure canonical record exists
        canonical_existed = nk in users
        if not canonical_existed:
            users[nk] = {
                'nick': target,
                'money': 0,
                'last_coins': 0.0,
                'last_mug': 0.0,
                'jail_until': 0.0,
                'last_bet': 0.0,
                'inv': {},
            }

        canonical = users[nk]
        merged_money = 0
        merged_inv = {}
        merged_count = 0

        for k in list(matches):
            if k == nk:
                continue
            u = users.get(k)
            if not isinstance(u, dict):
                continue
            merged_money += int(u.get('money', 0))
            for itemk, cnt in u.get('inv', {}).items():
                merged_inv[itemk] = merged_inv.get(itemk, 0) + int(cnt)

            canonical['last_coins'] = max(canonical.get('last_coins', 0.0), u.get('last_coins', 0.0))
            canonical['last_mug'] = max(canonical.get('last_mug', 0.0), u.get('last_mug', 0.0))
            canonical['last_bet'] = max(canonical.get('last_bet', 0.0), u.get('last_bet', 0.0))
            canonical['jail_until'] = max(canonical.get('jail_until', 0.0), u.get('jail_until', 0.0))

            users.pop(k, None)
            merged_count += 1

        canonical['money'] = int(canonical.get('money', 0)) + merged_money
        inv = canonical.setdefault('inv', {})
        for itemk, cnt in merged_inv.items():
            inv[itemk] = min(3, int(inv.get(itemk, 0)) + int(cnt))  # cap at 3

        _pm(bot, trigger.nick, f"✅ Merged {merged_count} record(s) into {canonical.get('nick', target)}. New balance: {fmt_coins(canonical['money'])} coins.")


@module.commands('mugdup')
def mugdup(bot, trigger):
    """PM: $mugdup <nick> — list stored records that normalize to the same nick (admin only)"""
    if not _pm_only(trigger):
        bot.reply("📩 Use this in PM, not in-channel.")
        return
    if not _is_admin(bot, trigger.nick):
        _pm(bot, trigger.nick, "🚫 Nice try. You’re not an admin.")
        return

    args = (trigger.group(2) or "").split()
    if not args:
        _pm(bot, trigger.nick, "Usage: $mugdup <nick>")
        return

    target = args[0]
    nk = normalize_key(target)

    data = _load_data(bot)
    users = data.get('users', {})
    matches = [k for k, u in users.items() if normalize_key(u.get('nick', '')) == nk]
    if not matches:
        _pm(bot, trigger.nick, f"No stored records normalize to '{target}' (normalized: '{nk}').")
        return

    _pm(bot, trigger.nick, f"Records normalizing to '{nk}':")
    for k in matches:
        u = users.get(k, {})
        _pm(bot, trigger.nick, f" - stored key: {k} | nick: {u.get('nick','?')} | money: {fmt_coins(u.get('money',0))}")
    _pm(bot, trigger.nick, "Use $mugmerge <nick> --dry to preview a merge, or $mugmerge <nick> to merge.")


@module.commands('mugclearbounty')
def mugclearbounty(bot, trigger):
    """PM: $mugclearbounty <nick> — remove a bounty (admin only)"""
    if not _pm_only(trigger):
        bot.reply("📩 Use this in PM, not in-channel.")
        return
    if not _is_admin(bot, trigger.nick):
        _pm(bot, trigger.nick, "🚫 Nice try. You're not an admin.")
        return

    args = (trigger.group(2) or "").split()
    if not args:
        _pm(bot, trigger.nick, "Usage: $mugclearbounty <nick>")
        return

    target = args[0]
    nk = normalize_key(target)

    with locked_data(bot) as data:
        bounties = data.get('bounties', {})
        amt = int(bounties.pop(nk, 0))
        if amt > 0:
            _pm(bot, trigger.nick, f"✅ Removed {fmt_coins(amt)} coin bounty on {target}. Coins returned to the void. 🕳️")
        else:
            _pm(bot, trigger.nick, f"🤷 No active bounty on {target}.")


# ============================================================
# ======================== GOD MODE ==========================
# ============================================================

@module.commands('mugstats')
def mugstats(bot, trigger):
    """$mugstats  (admin only) - economy overview via PM"""
    nick = trigger.nick
    if not _is_admin(bot, trigger.nick):
        if _pm_only(trigger):
            _pm(bot, nick, "🚫 Nice try. You're not an admin.")
        else:
            bot.reply("🚫 Nice try. You're not an admin.")
        return

    if not _pm_only(trigger):
        bot.say(f"📊 {nick}: sending economy stats to your PM.")

    data = _load_data(bot)
    users = data.get('users', {})
    total_users = len(users)
    total_coins = sum(int(u.get('money', 0)) for u in users.values())

    top = sorted(users.values(), key=lambda u: -int(u.get('money', 0)))[:5]

    _pm(bot, nick, "📊 Mug Economy Stats:")
    _pm(bot, nick, f"👥 Users in DB: {total_users}")
    _pm(bot, nick, f"💰 Total coins in economy: {fmt_coins(total_coins)}")
    if top:
        _pm(bot, nick, f"🏆 Top 5: {_format_lb(top, start_rank=1)}")
    else:
        _pm(bot, nick, "🏆 No users yet.")


@module.commands('godmode')
def godmode(bot, trigger):
    """PM: $godmode [on|off] [nick] — toggle god luck (admin only)"""
    if not _pm_only(trigger):
        bot.reply("📩 Use this in PM, not in-channel.")
        return
    if not _is_admin(bot, trigger.nick):
        _pm(bot, trigger.nick, "🚫 Nice try. You're not an admin.")
        return

    args = (trigger.group(2) or "").strip().split()

    # No args: show current status for self
    if not args:
        nk = normalize_key(trigger.nick)
        status = "✅ ON" if nk in _godmode else "❌ OFF"
        _pm(bot, trigger.nick, f"🔱 God mode for {trigger.nick}: {status}")
        # Also list anyone else with god mode
        others = [n for n in _godmode if n != nk]
        if others:
            _pm(bot, trigger.nick, f"👑 Others with god mode: {', '.join(others)}")
        return

    action = args[0].lower()
    target = args[1] if len(args) > 1 else str(trigger.nick)
    nk = normalize_key(target)

    if action in ('on', 'enable', 'true', '1'):
        _godmode.add(nk)
        _pm(bot, trigger.nick, f"🔱 God mode ENABLED for {target}. 99% luck on everything. 😈")
    elif action in ('off', 'disable', 'false', '0'):
        _godmode.discard(nk)
        _pm(bot, trigger.nick, f"🔱 God mode DISABLED for {target}. Back to mortal luck. 💨")
    else:
        _pm(bot, trigger.nick, "Usage: $godmode [on|off] [nick] — no args = check status")


# ============================================================
# ===================== ENABLE/DISABLE =======================
# ============================================================

@module.commands('mugtoggle')
def mugtoggle(bot, trigger):
    """$mugtoggle [on|off] or $mugtoggle #channel [on|off]  (admin only)"""
    if not _is_admin(bot, trigger.nick):
        bot.reply("🚫 Nice try. You're not an admin.")
        return

    args = (trigger.group(2) or "").strip().split()
    in_channel = str(trigger.sender).startswith('#')

    # Determine target channel and action
    channel = None
    action = None

    if in_channel:
        # Used in-channel: channel is implicit
        channel = str(trigger.sender)
        if args:
            action = args[0].lower()
    else:
        # Used in PM: first arg may be a #channel
        if args and args[0].startswith('#'):
            channel = args[0]
            if len(args) > 1:
                action = args[1].lower()
        elif args:
            action = args[0].lower()

    if not channel:
        _pm(bot, trigger.nick, "🎮 Usage (PM): $mugtoggle #channel [on|off]")
        _pm(bot, trigger.nick, "In-channel: just use $mugtoggle [on|off]")
        # Show all channel statuses
        toggles = _load_channel_toggles(bot)
        if toggles:
            for ch, enabled in sorted(toggles.items()):
                st = "ENABLED ✅" if enabled else "DISABLED 🔒"
                _pm(bot, trigger.nick, f"  {ch}: {st}")
        else:
            _pm(bot, trigger.nick, "  All channels: ENABLED ✅ (default)")
        return

    def _reply(text):
        if in_channel:
            bot.say(text)
        else:
            _pm(bot, trigger.nick, text)

    if action in ('on', 'enable', 'true', '1'):
        _set_channel_enabled(bot, channel, True)
        _reply(f"✅ Mug game ENABLED in {channel}. Let the chaos resume. 😈")
    elif action in ('off', 'disable', 'false', '0'):
        _set_channel_enabled(bot, channel, False)
        _reply(f"🔒 Mug game DISABLED in {channel}. All gameplay commands paused.")
    else:
        status = "ENABLED ✅" if _plugin_enabled(bot, channel) else "DISABLED 🔒"
        _reply(f"🎮 Mug game in {channel}: {status}")
        _reply("Usage: $mugtoggle [on|off]")


# ============================================================
# ========================= HELP (PM) =========================
# ============================================================

@module.commands('mughelp')
def mughelp(bot, trigger):
    nick = trigger.nick
    if trigger.sender.startswith('#'):
        bot.say(f"📨 {nick}: check your PM for mug game help.")

    lines = [
        "📖 Mug Game Help (FULL EMOJI CHAOS) 😈🪙",
        "",
        "💰 Economy:",
        "  • $coins — Get coins (cooldown). Richer players gain more (5–15% bonus, capped).",
        "  • $bal / $balance [nick] — Check coin balance.",
        "  • $give <nick> <amount> — Give coins to someone in the channel.",
        "",
        "🎯 Bounties:",
        "  • $bounty <nick> <amount> — Put coins on someone’s head (costs you coins).",
        "  • $bounties — Show top bounty targets (auto-splits if too long).",
        "    - If someone mugs a bounty target, they instantly claim the entire bounty pool.",
        "",
        "🗡️ Mugging (ONE LINE):",
        "  • $mug <nick> / $rob <nick> — Attempt to mug someone (costs a small fee).",
        "    - Always outputs ONE line per mug attempt (no setup line).",
        "    - You can mug from anywhere, but the target must already be a known mug player.",
        "    - Success: steal a % of their coins (items can boost).",
        "    - Fail: you drop coins and they pick them up + extra cooldown.",
        "    - Critical fail: you lose a lot + go to jail (no mugging until free).",
        "    - Ultra-rare: mega heists + instant oops-jail events.",
        "    - Whale protection: if victim > 10,000 coins, max steal is 25% per mug.",
        "  • $jail — Check if you’re jailed and for how long.",
        "",
        "🎲 Gambling:",
        "  • $bet <amount> — gamble it all (Lucky Coin improves odds). No max bet!",
        "    - Win: payout is 2x your bet. Lose: you lose the bet.",
        "",
        "🛒 Shop (PM-only):",
        "  • $shop — View shop items (PM).",
        "  • $buy <itemkey> — Buy an item (PM).",
        "  • $inv / $inventory — View inventory (PM).",
        "  • $use <itemkey> — Use a consumable item, e.g. bail (PM). Passive items can't be consumed.",
        "",
        "🧰 Items:",
        "  • mask — +mug success chance",
        "  • knucks — +steal % on success",
        "  • luckycoin — +$coins bonus, +$bet win chance",
        "  • vest — reduces stolen amount",
        "  • cloak — chance to dodge a successful mug",
        "  • banana — trap: chance mugger slips into disaster 🍌",
        "  • bail — Bail Bondsman: costs 5000, instantly frees you when used or bought while jailed. 🪓🎉",
        "",
        "🏆 Leaderboards:",
        "  • $top5 — Top 5 richest (auto-splits if needed)",
        "  • $top10 — Top 10 richest (split into two lines + auto-split safety)",
        "",
        "🛠️ PM Admin (if you're allowed):",
        "  • $mugadd <nick> <amount> — add coins",
        "  • $mugset <nick> <amount> — set balance",
        "  • $mugtake <nick> <amount> — remove coins",
        "  • $mugreset — FULL RESET (everyone to 0, clears inv/cooldowns/bounties)",
        "  • $mugcleardb confirm — WIPE DB (deletes all player records)",
        "  • $mugmerge <nick> [--dry] — merge duplicate records for normalized nick (admin only; use --dry to preview)",
        "  • $mugdup <nick> — list stored records that normalize to the same nick (admin only)",
        "  • $mugclearbounty <nick> — remove a bounty and refund to pool (admin only)",
        "  • $mugstats — PM economy overview: user count, top 5, total coins (admin only)",
        "  • $godmode [on|off] [nick] — toggle 99% luck for yourself or a player (PM, admin only)",
        "  • $mugtoggle [on|off] — enable/disable the mug game in the current channel (in-channel or PM)",
        "    - In PM: $mugtoggle #channel [on|off] — target a specific channel",
        "    - With no args in PM: shows status of all channels",
        "",
        "😎 Pro tip: If the channel is bullying you, buy bananas and place bounties like a supervillain.",
    ]

    for line in lines:
        _pm(bot, nick, line)

