/*
 * ms_mugserv.cpp — MugServ standalone economy service for Anope IRC Services 2.0
 *
 * Ported from mug_game.py (Sopel plugin). Full feature parity:
 *   coin collection, mugging, gambling, bounties, item shop, jail, leaderboards.
 *
 * Players are automatically enrolled on first use — a NickServ account (identified)
 * is required; no manual registration step needed.
 * Data is stored in <services datadir>/mugserv.db (flat text, human-readable).
 *
 * Channel mode: the bot joins configured channels and responds to !command triggers.
 * Use /msg MugServ ENABLE #channel and DISABLE #channel to add/remove channels at
 * runtime. Users may also still /msg MugServ directly.
 *
 * ─── services.conf ───────────────────────────────────────────────────────────
 *  module { name = "ms_mugserv"; }
 *
 *  service
 *  {
 *      nick  = "MugServ"
 *      user  = "mugserv"
 *      host  = "services.example.net"
 *      gecos = "Mugging Service"
 *  }
 *
 *  ms_mugserv
 *  {
 *      client      = "MugServ"
 *      channels    = "#general #gaming"  # channels where the bot listens (optional)
 *      cmd_prefix  = "!"                 # trigger prefix in channels (default: !)
 *      # admin_nicks = "Nick1 Nick2"     # extra MugServ admins beyond IRCops
 *  }
 * ─────────────────────────────────────────────────────────────────────────────
 *
 * Build: place in anope/modules/third/ms_mugserv.cpp, then:
 *   cd anope-build && make ms_mugserv
 *   anoperc modload ms_mugserv
 */

#include "module.h"

#include <algorithm>
#include <cstdint>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <map>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <vector>

// ===========================================================================
// Tuneable game constants
// ===========================================================================

static const int      CD_COINS          = 600;    // cooldown seconds
static const int      CD_MUG            = 300;
static const int      CD_MUG_FAIL_XTRA  = 120;    // extra CD on normal fail
static const int      CD_JAIL           = 600;
static const int      CD_BET            = 60;
static const int      CD_GIVE           = 300;
static const int      CD_BOUNTY         = 60;

static const int      COINS_MIN         = 15;
static const int      COINS_MAX         = 75;
static const int      COINS_SCALE_MIN   = 5;      // pct of current balance
static const int      COINS_SCALE_MAX   = 15;
static const int64_t  COINS_SCALE_CAP   = 1500;

static const int64_t  MUG_FEE           = 2;
static const int      STEAL_MIN         = 10;     // pct
static const int      STEAL_MAX         = 30;
static const int      FAIL_LOSS_MIN     = 5;
static const int      FAIL_LOSS_MAX     = 15;
static const int      CRIT_LOSS_MIN     = 20;
static const int      CRIT_LOSS_MAX     = 40;
static const int64_t  MAX_FAIL_LOSS     = 100000LL;
static const int64_t  MAX_CRIT_LOSS     = 250000LL;

static const int      SUCCESS_CHANCE    = 60;     // 1-100 roll
static const int      FAIL_CHANCE       = 25;     // remaining % = crit fail

static const int      BET_WIN_BASE      = 40;     // pct
static const int      BET_MIN           = 1;

static const int64_t  RICH_THRESHOLD    = 10000LL;
static const int      RICH_MAX_STEAL    = 25;     // pct cap vs rich targets

static const int      BANANA_SLIP_PCT   = 5;      // per banana item
static const int      BANANA_SLIP_MAX   = 25;

static const int      MEGA_STEAL_CHANCE = 1;      // pct
static const int      MEGA_STEAL_BONUS  = 25;     // extra steal pct
static const int      OOPS_JAIL_CHANCE  = 1;      // pct instant jail

static const int      BOUNTY_MIN_AMT    = 10;
static const int64_t  BOUNTY_MAX_AMT    = 100000LL;
static const int64_t  GIVE_DAILY_LIMIT  = 500000LL;
static const int      GIVE_DAY_SECS     = 86400;

// ===========================================================================
// Item table  (index 0-6 maps to ITEM_KEYS[])
// ===========================================================================

struct ItemDef
{
    const char *key, *name;
    int64_t     price;
    const char *desc;
    int mug_success_bonus;
    int steal_bonus;
    int coins_flat;
    int bet_bonus;
    int steal_reduction;
    int immune_chance;
    int banana_slip;
    bool is_bail;
};

static const ItemDef ITEMS[7] =
{
    { "mask",      "Heist Mask",     120,  "Boosts mug success chance.",                7, 0, 0, 0,  0,  0, 0,                 false },
    { "knucks",    "Brass Knuckles", 250,  "Steal more on successful mugs.",            0, 6, 0, 0,  0,  0, 0,                 false },
    { "luckycoin", "Lucky Coin",     180,  "Extra COINS + better BET odds.",            0, 0, 3, 7,  0,  0, 0,                 false },
    { "vest",      "Kevlar Vest",    220,  "Reduces how much others steal from you.",   0, 0, 0, 0, 20,  0, 0,                 false },
    { "cloak",     "Shadow Cloak",   500,  "Chance to dodge a successful mug.",         0, 0, 0, 0,  0, 15, 0,                 false },
    { "banana",    "Banana Peel",     50,  "Muggers may slip into disaster.",           0, 0, 0, 0,  0,  0, BANANA_SLIP_PCT,   false },
    { "bail",      "Bail Bondsman", 5000,  "Frees you from jail once.",                 0, 0, 0, 0,  0,  0, 0,                 true  },
};

static const int NUM_ITEMS = 7;

static int find_item(const Anope::string &key)
{
    Anope::string lk = key.lower();
    for (int i = 0; i < NUM_ITEMS; ++i)
        if (lk == ITEMS[i].key)
            return i;
    return -1;
}

// ===========================================================================
// RNG
// ===========================================================================

static std::mt19937 s_rng(static_cast<uint32_t>(std::time(nullptr)));

static int ri(int lo, int hi)   // inclusive[lo,hi]
{
    std::uniform_int_distribution<int> d(lo, hi);
    return d(s_rng);
}

template<typename T>
static const T& rand_pick(const std::vector<T> &v)
{
    return v[static_cast<size_t>(ri(0, static_cast<int>(v.size()) - 1))];
}

// ===========================================================================
// Formatting helpers
// ===========================================================================

static Anope::string fmt_coins(int64_t n)
{
    bool neg = n < 0;
    uint64_t val = static_cast<uint64_t>(neg ? -n : n);
    std::string s = std::to_string(val);
    for (int p = static_cast<int>(s.size()) - 3; p > 0; p -= 3)
        s.insert(static_cast<size_t>(p), ",");
    if (neg) s.insert(0, "-");
    return Anope::string(s);
}

static Anope::string fmt_dur(int sec)
{
    if (sec <= 0) return Anope::string("0s");
    if (sec < 60)  return stringify(sec) + "s";
    int m = sec / 60, s = sec % 60;
    if (m < 60) return stringify(m) + "m" + (s ? " " + stringify(s) + "s" : "");
    int h = m / 60; m %= 60;
    return stringify(h) + "h" + (m ? " " + stringify(m) + "m" : "");
}

// ===========================================================================
// Player record
// ===========================================================================

struct MugUser
{
    Anope::string account;          // NickServ account name (lowercased; map key)
    Anope::string nick;             // current IRC display nick (updated on each command)
    int64_t coins       = 0;
    time_t  last_coins  = 0;
    time_t  last_mug    = 0;
    time_t  jail_until  = 0;
    time_t  last_bet    = 0;
    time_t  last_give   = 0;
    time_t  last_bounty = 0;
    int64_t daily_given = 0;
    time_t  daily_reset = 0;
    int     inv[7]      = {};       // indexed by ITEMS[]
};

// Sum an int field across all item stacks the user holds.
// Uses pointer-to-member to avoid a switch/cascade.
static int inv_sum(const MugUser &u, int ItemDef::*fld)
{
    int total = 0;
    for (int i = 0; i < NUM_ITEMS; ++i)
        total += (ITEMS[i].*fld) * u.inv[i];
    return total;
}

// ===========================================================================
// Module-level globals  (managed by ModuleMugServ::OnReload + DB functions)
// ===========================================================================

static BotInfo                               *s_bot            = nullptr;
static std::set<Anope::string>                s_channels;       // lowercased channel names
static Anope::string                          s_cmd_prefix      = "!";
static std::vector<Anope::string>             s_admin_nicks;
// Set by OnMessage before dispatching — announce() uses this to reply to the
// originating channel. Cleared after dispatch. Empty = PM-triggered.
static Anope::string                          s_current_chan;

// nick.lower() → MugUser
static std::map<Anope::string, MugUser>       s_users;
// nick.lower() → bounty pool
static std::map<Anope::string, int64_t>       s_bounties;

// ===========================================================================
// Persistence helpers  (flat-file DB)
// ===========================================================================

static Anope::string db_path()
{
    return Anope::DataDir + "/mugserv.db";
}

static void save_db()
{
    std::ofstream f(db_path().c_str());
    if (!f.is_open())
        return;

    f << "# MugServ database v1 — do not edit while service is running\n";

    for (const auto &ch : s_channels)
        f << "CHANNEL " << ch.c_str() << "\n";

    for (const auto &kv : s_users)
    {
        const MugUser &u = kv.second;
        f << "USER"
          << " " << u.account.c_str()
          << " " << u.nick.c_str()
          << " " << u.coins
          << " " << static_cast<long long>(u.last_coins)
          << " " << static_cast<long long>(u.last_mug)
          << " " << static_cast<long long>(u.jail_until)
          << " " << static_cast<long long>(u.last_bet)
          << " " << static_cast<long long>(u.last_give)
          << " " << static_cast<long long>(u.last_bounty)
          << " " << u.daily_given
          << " " << static_cast<long long>(u.daily_reset);
        for (int i = 0; i < NUM_ITEMS; ++i)
            f << " " << u.inv[i];
        f << "\n";
    }

    for (const auto &kv : s_bounties)
        f << "BOUNTY " << kv.first.c_str() << " " << kv.second << "\n";
}

static void load_db()
{
    std::ifstream f(db_path().c_str());
    if (!f.is_open())
        return;

    std::string line;
    while (std::getline(f, line))
    {
        if (line.empty() || line[0] == '#')
            continue;

        std::istringstream ss(line);
        std::string tag;
        ss >> tag;

        if (tag == "CHANNEL")
        {
            std::string ch;
            ss >> ch;
            if (!ch.empty())
                s_channels.insert(Anope::string(ch).lower());
        }
        else if (tag == "USER")
        {
            MugUser u;
            std::string acct, nick;
            long long lc, lm, ju, lb, lg, lbn, dr;
            ss >> acct >> nick >> u.coins >> lc >> lm >> ju >> lb >> lg >> lbn
               >> u.daily_given >> dr;
            u.account      = Anope::string(acct);
            u.nick         = Anope::string(nick);
            u.last_coins   = static_cast<time_t>(lc);
            u.last_mug     = static_cast<time_t>(lm);
            u.jail_until   = static_cast<time_t>(ju);
            u.last_bet     = static_cast<time_t>(lb);
            u.last_give    = static_cast<time_t>(lg);
            u.last_bounty  = static_cast<time_t>(lbn);
            u.daily_reset  = static_cast<time_t>(dr);
            for (int i = 0; i < NUM_ITEMS; ++i)
            {
                int v = 0;
                ss >> v;
                u.inv[i] = std::min(v, 3);  // cap at 3
            }
            if (!u.account.empty())
                s_users[u.account] = u;
        }
        else if (tag == "BOUNTY")
        {
            std::string nick;
            int64_t amt = 0;
            ss >> nick >> amt;
            s_bounties[Anope::string(nick).lower()] = amt;
        }
    }
}

// ===========================================================================
// Misc helpers
// ===========================================================================

static MugUser* get_user(const Anope::string &nick)
{
    auto it = s_users.find(nick.lower());
    return (it != s_users.end()) ? &it->second : nullptr;
}

static bool is_admin(CommandSource &src)
{
    if (!src.GetUser())
        return false;
    if (src.GetUser()->IsOper())
        return true;
    Anope::string lnick = src.GetNick().lower();
    for (const Anope::string &an : s_admin_nicks)
        if (lnick == an.lower())
            return true;
    return false;
}

// Send a public message: if s_current_chan is set (channel-triggered command),
// reply there; otherwise broadcast to all active channels (PM-triggered command).
// Falls back to user PM if no channels are configured.
static void announce(CommandSource &src, const Anope::string &msg,
                     const Anope::string &chan = "")
{
    // Prefer explicit channel arg, then s_current_chan, then broadcast/PM.
    const Anope::string &dest = !chan.empty() ? chan : s_current_chan;
    if (!s_bot) { src.Reply("%s", msg.c_str()); return; }
    if (!dest.empty())
    {
        s_bot->Say(dest, msg);
    }
    else if (!s_channels.empty())
    {
        // PM-triggered with no current channel: broadcast to all active channels.
        for (const auto &ch : s_channels)
            s_bot->Say(ch, msg);
    }
    else
    {
        src.Reply("%s", msg.c_str());
    }
}

// Always reply to the user (PM from MugServ).
static void pm(CommandSource &src, const Anope::string &msg)
{
    src.Reply("%s", msg.c_str());
}

// Resolve a target nick to a MugUser via NickServ account (then nick fallback).
static MugUser* get_user_by_target(const Anope::string &target)
{
    NickAlias *na = NickAlias::Find(target);
    if (na && na->nc)
    {
        auto it = s_users.find(na->nc->display.lower());
        if (it != s_users.end())
            return &it->second;
    }
    // Fallback: match stored display nick (covers players who are offline).
    Anope::string tl = target.lower();
    for (auto &kv : s_users)
        if (kv.second.nick.lower() == tl)
            return &kv.second;
    return nullptr;
}

// Get the s_users map key for a target nick (NickServ account name, or nick fallback).
static Anope::string get_target_account_key(const Anope::string &target)
{
    NickAlias *na = NickAlias::Find(target);
    if (na && na->nc)
        return na->nc->display.lower();
    return target.lower();
}

// Gate: require NickServ identification; auto-enroll on first use.
// Returns true if the command should be blocked.
static bool check_gate(CommandSource &src)
{
    if (!src.nc)
    {
        pm(src, "\002MugServ\002: You must be identified with NickServ to play. "
                "Use: /msg NickServ IDENTIFY <password>");
        return true;
    }
    Anope::string key = src.nc->display.lower();
    if (!s_users.count(key))
    {
        // First use — auto-enroll silently.
        MugUser u;
        u.account = key;
        u.nick    = src.GetNick();
        s_users[key] = u;
        pm(src, "\002Welcome to MugServ, " + src.GetNick() + "!\002 "
                "You've been automatically enrolled (NickServ account: " + src.nc->display + "). "
                "Start with COINS to collect your first coins, or HELP for all commands.");
    }
    else
    {
        // Refresh display nick on every command.
        s_users[key].nick = src.GetNick();
    }
    return false;
}

// Cooldown remaining in seconds (0 = ready).
static int cd_rem(time_t last, int duration)
{
    time_t now = Anope::CurTime;
    time_t expire = last + static_cast<time_t>(duration);
    return (now < expire) ? static_cast<int>(expire - now) : 0;
}

// Leaderboard formatting.
static Anope::string format_lb(const std::vector<MugUser*> &list, int start_rank)
{
    Anope::string out;
    for (size_t i = 0; i < list.size(); ++i)
    {
        int rank = static_cast<int>(i) + start_rank;
        Anope::string prefix;
        if (rank == 1)      prefix = "\00307\002#1\002\003";
        else if (rank == 2) prefix = "\00315#2\003";
        else if (rank == 3) prefix = "\00305#3\003";
        else                prefix = "#" + stringify(rank);

        Anope::string entry = prefix + " " + list[i]->nick
                              + "(" + fmt_coins(list[i]->coins) + ")";
        if (!out.empty()) out += " | ";
        out += entry;
    }
    return out;
}

// ===========================================================================
// Flavour text pools
// ===========================================================================

static const std::vector<Anope::string> MSG_BROKE = {
    "You're broke. Like, emotionally and financially.",
    "Wallet empty. Dreams empty. Use COINS.",
    "Not enough coins. The streets are calling: COINS.",
    "Your bank account just said 'lol'.",
};
static const std::vector<Anope::string> MSG_COINS_CD = {
    "The coin gods are buffering... try again in {t}.",
    "Your greed is on cooldown. Return in {t}.",
    "Banker's on break. Next appointment in {t}.",
    "Your dopamine is rate-limited. Next hit in {t}.",
};
static const std::vector<Anope::string> MSG_MUG_CD = {
    "Lay low... the cops still remember your face. Try again in {t}.",
    "Your getaway shoes are untied. Fix them in {t}.",
    "CCTV is still tracking you. Hide {t} longer.",
    "Your criminal aura is cooling down. {t}.",
};
static const std::vector<Anope::string> MSG_BET_CD = {
    "The casino bouncer says 'not yet.' Try again in {t}.",
    "Your wallet is begging for mercy. Wait {t}.",
    "The slot machine overheated. Cooling for {t}.",
    "The crystal ball is foggy. Returns in {t}.",
};
static const std::vector<Anope::string> MSG_SELF_MUG = {
    "You can't mug yourself. Therapy is cheaper. (Probably.)",
    "You stare into the mirror and threaten it. The mirror wins.",
    "Galaxy brain move: attempted self-mug. Zero coins gained.",
    "Crime rejected. Victim and attacker are the same idiot.",
};
static const std::vector<Anope::string> MSG_MUG_SUCCESS = {
    "{att} jumps {vic} and snatches {steal} coins!",
    "{att} runs {vic}'s pockets for {steal} coins!",
    "{att} hits-and-dips {vic} for {steal} coins!",
    "{att} drains {vic} for {steal} coins!",
    "{att} yoinks {steal} coins off {vic} like it's casual.",
};
static const std::vector<Anope::string> MSG_MUG_MEGA = {
    "MEGA HEIST! {att} pulls a legendary swipe on {vic} for {steal} coins!!",
    "ULTRA MUG! {att} hits {vic} with main-character energy: {steal} coins!",
    "GOD TIER YOINK! {att} extracts {steal} coins from {vic}'s soul!!",
};
static const std::vector<Anope::string> MSG_MUG_FAIL = {
    "{att} slipped mid-mug and dropped {loss} coins across the street!",
    "{att} tried to look intimidating but sneezed and dropped {loss} coins!",
    "{att} tripped over shoelaces and made it rain {loss} coins!",
    "{att} got ambushed by a random cat and scattered {loss} coins!",
};
static const std::vector<Anope::string> MSG_MUG_CRIT = {
    "CRITICAL FAIL! {att} faceplants, drops {loss} coins, and gets tossed in jail for {jail}.",
    "{att} mugs the air, loses {loss} coins, and the police applauded... then arrested them. Jail: {jail}.",
    "{att} left fingerprints on EVERYTHING, lost {loss} coins, and got detained. Jail: {jail}.",
};
static const std::vector<Anope::string> MSG_BOUNTY_PLACE = {
    "Bounty placed on {vic} for {amt} coins. Somebody is getting touched.",
    "You put {amt} coins on {vic}'s head. That's... oddly motivational.",
    "{vic} is now WANTED for {amt} coins.",
};
static const std::vector<Anope::string> MSG_BOUNTY_CLAIM = {
    "BOUNTY CLAIMED! {att} collects {bounty} bonus coins for mugging {vic}!",
    "Payday! {att} cashes in a {bounty}-coin bounty on {vic}.",
};

// Replace {key} tokens in a template string.
static Anope::string tpl(const Anope::string &tmpl,
                          const std::vector<std::pair<Anope::string, Anope::string>> &vars)
{
    Anope::string out = tmpl;
    for (const auto &kv : vars)
    {
        Anope::string tok = "{" + kv.first + "}";
        size_t pos = 0;
        while ((pos = out.find(tok, pos)) != Anope::string::npos)
        {
            out = out.substr(0, pos) + kv.second + out.substr(pos + tok.length());
            pos += kv.second.length();
        }
    }
    return out;
}

// ===========================================================================
// Forward declaration (timer needs module ptr)
// ===========================================================================
class ModuleMugServ;
static ModuleMugServ *s_module = nullptr;

// ===========================================================================
// Auto-save timer
// ===========================================================================
class MugSaveTimer : public Timer
{
public:
    MugSaveTimer() : Timer(300, true) {}  // every 5 minutes
    void Tick(time_t) override { save_db(); }
};

// ===========================================================================
// Commands
// ===========================================================================

// ─── COINS ──────────────────────────────────────────────────────────────────
struct CommandMugCoins : Command
{
    CommandMugCoins(Module *c) : Command(c, "mugserv/COINS", 0, 0)
    {
        SetDesc("Collect your coins (10-min cooldown)");
    }

    void Execute(CommandSource &src, const std::vector<Anope::string> &) override
    {
        if (check_gate(src)) return;
        MugUser &u = s_users[src.nc->display.lower()];

        int rem = cd_rem(u.last_coins, CD_COINS);
        if (rem > 0)
        {
            pm(src, tpl(rand_pick(MSG_COINS_CD), {{"t", fmt_dur(rem)}}));
            return;
        }

        int64_t cur  = std::max(int64_t(0), u.coins);
        int base = ri(COINS_MIN, COINS_MAX);
        int64_t scale = 0;
        if (cur > 0)
        {
            int pct = ri(COINS_SCALE_MIN, COINS_SCALE_MAX);
            scale = std::min(static_cast<int64_t>(cur * pct / 100), COINS_SCALE_CAP);
        }
        int64_t flat = std::min(50, inv_sum(u, &ItemDef::coins_flat));
        int64_t gain = std::max(int64_t(1), static_cast<int64_t>(base) + scale + flat);

        u.coins      += gain;
        u.last_coins  = Anope::CurTime;

        Anope::string msg = "💰 " + u.nick + " found \002" + fmt_coins(gain)
                          + "\002 coins! Balance: \002" + fmt_coins(u.coins) + "\002 coins.";
        announce(src, msg);
    }
};

// ─── BALANCE ────────────────────────────────────────────────────────────────
struct CommandMugBalance : Command
{
    CommandMugBalance(Module *c) : Command(c, "mugserv/BALANCE", 0, 1)
    {
        SetDesc("Check your (or another player's) coin balance");
        SetSyntax("[nick]");
    }

    void Execute(CommandSource &src, const std::vector<Anope::string> &params) override
    {
        if (!src.nc)
        {
            pm(src, "You must be identified with NickServ to use MugServ.");
            return;
        }

        MugUser *u;
        if (params.empty())
        {
            u = get_user(src.nc->display.lower());
            if (!u)
            {
                pm(src, "You don't have a MugServ account yet. Use any command to get started!");
                return;
            }
        }
        else
        {
            u = get_user_by_target(params[0]);
            if (!u)
            {
                pm(src, params[0] + " has no MugServ account.");
                return;
            }
        }
        pm(src, "🧾 \002" + u->nick + "\002 has \002" + fmt_coins(u->coins) + "\002 coins.");
    }
};

// ─── GIVE ───────────────────────────────────────────────────────────────────
struct CommandMugGive : Command
{
    CommandMugGive(Module *c) : Command(c, "mugserv/GIVE", 2, 2)
    {
        SetDesc("Give coins to another registered player");
        SetSyntax("<nick> <amount>");
    }

    void Execute(CommandSource &src, const std::vector<Anope::string> &params) override
    {
        if (check_gate(src)) return;
        MugUser &giver = s_users[src.nc->display.lower()];

        Anope::string target = params[0];
        if (get_target_account_key(target) == src.nc->display.lower())
        {
            pm(src, "You can't give yourself coins. That's called having coins.");
            return;
        }

        int64_t amt = 0;
        try { amt = std::stoll(params[1].c_str()); } catch (...) {}
        if (amt < 1)
        {
            pm(src, "Amount must be a positive whole number.");
            return;
        }

        MugUser *recv = get_user_by_target(target);
        if (!recv)
        {
            pm(src, target + " has no MugServ account.");
            return;
        }

        int rem = cd_rem(giver.last_give, CD_GIVE);
        if (rem > 0)
        {
            pm(src, "You can give again in " + fmt_dur(rem) + ".");
            return;
        }

        // Rolling daily cap
        time_t now = Anope::CurTime;
        if (now - giver.daily_reset > GIVE_DAY_SECS)
        {
            giver.daily_given = 0;
            giver.daily_reset = now;
        }
        if (giver.daily_given + amt > GIVE_DAILY_LIMIT)
        {
            int64_t remaining = std::max(int64_t(0), GIVE_DAILY_LIMIT - giver.daily_given);
            pm(src, "Daily give limit reached. You may give " + fmt_coins(remaining)
                    + " more coins today.");
            return;
        }

        if (giver.coins < amt)
        {
            pm(src, rand_pick(MSG_BROKE));
            return;
        }

        giver.coins      -= amt;
        recv->coins      += amt;
        giver.daily_given += amt;
        giver.last_give   = now;

        Anope::string msg = "🤝 \002" + giver.nick + "\002 gave \002" + fmt_coins(amt)
                          + "\002 coins to \002" + recv->nick + "\002!";
        announce(src, msg);
    }
};

// ─── JAIL ───────────────────────────────────────────────────────────────────
struct CommandMugJail : Command
{
    CommandMugJail(Module *c) : Command(c, "mugserv/JAIL", 0, 0)
    {
        SetDesc("Check your jail status");
    }

    void Execute(CommandSource &src, const std::vector<Anope::string> &) override
    {
        if (check_gate(src)) return;
        MugUser &u = s_users[src.nc->display.lower()];
        time_t now = Anope::CurTime;

        if (u.jail_until > now)
        {
            int rem = static_cast<int>(u.jail_until - now);
            pm(src, "🚔 \002" + u.nick + "\002 is doing time! Free in "
                    + fmt_dur(rem) + ".");
        }
        else
        {
            pm(src, "✅ \002" + u.nick + "\002 is a free criminal once again. 😈");
        }
    }
};

// ─── MUG / ROB ──────────────────────────────────────────────────────────────
struct CommandMugMug : Command
{
    CommandMugMug(Module *c) : Command(c, "mugserv/MUG", 1, 1)
    {
        SetDesc("Attempt to mug a registered player");
        SetSyntax("<nick>");
    }

    void Execute(CommandSource &src, const std::vector<Anope::string> &params) override
    {
        if (check_gate(src)) return;

        Anope::string att_nick = src.GetNick();
        Anope::string vic_nick = params[0];

        if (get_target_account_key(vic_nick) == src.nc->display.lower())
        {
            pm(src, rand_pick(MSG_SELF_MUG));
            return;
        }

        MugUser *vic = get_user_by_target(vic_nick);
        if (!vic)
        {
            pm(src, vic_nick + " has no MugServ account.");
            return;
        }

        MugUser &att = s_users[src.nc->display.lower()];
        time_t now = Anope::CurTime;

        // Jail check
        if (att.jail_until > now)
        {
            int rem = static_cast<int>(att.jail_until - now);
            pm(src, "🚔 You're still in jail for " + fmt_dur(rem) + ". No crimes for now!");
            return;
        }

        // Mug cooldown
        int mug_rem = cd_rem(att.last_mug, CD_MUG);
        if (mug_rem > 0)
        {
            pm(src, tpl(rand_pick(MSG_MUG_CD), {{"t", fmt_dur(mug_rem)}}));
            return;
        }

        // Fee check
        if (att.coins < MUG_FEE)
        {
            pm(src, rand_pick(MSG_BROKE));
            return;
        }

        // ── Ultra-rare instant jail (before anything else) ─────────────────
        if (ri(1, 100) <= OOPS_JAIL_CHANCE)
        {
            int64_t loss = std::min(att.coins, MAX_CRIT_LOSS);
            att.coins      = std::max(int64_t(0), att.coins - loss);
            vic->coins    += loss;
            att.jail_until = now + CD_JAIL;
            att.last_mug   = now;

            Anope::string msg = tpl(rand_pick(MSG_MUG_CRIT),
                {{"att", att.nick}, {"loss", fmt_coins(loss)},
                 {"jail", fmt_dur(CD_JAIL)}})
                + " [OOPS-JAIL — you looked suspicious!] | "
                + att.nick + ": " + fmt_coins(att.coins)
                + " | " + vic->nick + ": " + fmt_coins(vic->coins);
            announce(src, msg);
            return;
        }

        // Deduct mug fee
        att.coins -= MUG_FEE;

        // ── Roll ────────────────────────────────────────────────────────────
        int success_bonus = std::min(35, inv_sum(att, &ItemDef::mug_success_bonus));
        int eff_success   = std::min(95, SUCCESS_CHANCE + success_bonus);
        int roll          = ri(1, 100);

        // ── Lambda helpers (mug outcome functions) ──────────────────────────

        // CRIT FAIL
        auto do_crit_fail = [&](const Anope::string &extra = "")
        {
            int64_t cur = att.coins;
            int pct = ri(CRIT_LOSS_MIN, CRIT_LOSS_MAX);
            int64_t loss = std::min({static_cast<int64_t>(cur * pct / 100), cur, MAX_CRIT_LOSS});
            loss = std::max(loss, int64_t(0));

            att.coins    -= loss;
            vic->coins   += loss;
            att.last_mug  = now;

            // Auto-bail
            if (att.inv[6] > 0)  // [6] = bail
            {
                att.inv[6]--;
                att.jail_until = 0;
                Anope::string msg = tpl(rand_pick(MSG_MUG_CRIT),
                    {{"att", att.nick}, {"loss", fmt_coins(loss)},
                     {"jail", fmt_dur(CD_JAIL)}}) + extra
                    + " BUT \002" + att.nick + "\002 had a Bail Bondsman and got out instantly!"
                    + " (Mug cooldown still active.)"
                    + " | " + att.nick + ": " + fmt_coins(att.coins)
                    + " | " + vic->nick + ": " + fmt_coins(vic->coins);
                announce(src, msg);
            }
            else
            {
                att.jail_until = now + CD_JAIL;
                Anope::string msg = tpl(rand_pick(MSG_MUG_CRIT),
                    {{"att", att.nick}, {"loss", fmt_coins(loss)},
                     {"jail", fmt_dur(CD_JAIL)}}) + extra
                    + " | " + att.nick + ": " + fmt_coins(att.coins)
                    + " | " + vic->nick + ": " + fmt_coins(vic->coins);
                announce(src, msg);
            }
        };

        // SUCCESS
        if (roll <= eff_success)
        {
            // Banana trap
            int banana_count = vic->inv[5];  // [5] = banana
            if (banana_count > 0)
            {
                int slip = std::min(banana_count * BANANA_SLIP_PCT, BANANA_SLIP_MAX);
                if (ri(1, 100) <= slip)
                {
                    do_crit_fail(" [🍌 BANANA TRAP fired!]");
                    return;
                }
            }

            // Cloak dodge
            int immune = std::min(50, inv_sum(*vic, &ItemDef::immune_chance));
            if (immune > 0 && ri(1, 100) <= immune)
            {
                att.last_mug = now;
                Anope::string msg = "🕶️ \002" + att.nick + "\002 tries to mug \002"
                    + vic->nick + "\002 but they vanish into the shadows. Nothing stolen! 👻"
                    + " | " + att.nick + ": " + fmt_coins(att.coins)
                    + " | " + vic->nick + ": " + fmt_coins(vic->coins);
                announce(src, msg);
                return;
            }

            if (vic->coins <= 0)
            {
                att.last_mug = now;
                Anope::string msg = "\002" + att.nick + "\002 tried to mug \002"
                    + vic->nick + "\002 but they're flat broke. Nothing to steal! 🤷"
                    + " | " + att.nick + ": " + fmt_coins(att.coins)
                    + " | " + vic->nick + ": " + fmt_coins(vic->coins);
                announce(src, msg);
                return;
            }

            int steal_pct = ri(STEAL_MIN, STEAL_MAX)
                          + std::min(30, inv_sum(att, &ItemDef::steal_bonus));
            bool mega = (ri(1, 100) <= MEGA_STEAL_CHANCE);
            if (mega) steal_pct += MEGA_STEAL_BONUS;

            int64_t steal_raw = std::max(int64_t(1),
                                         static_cast<int64_t>(vic->coins * steal_pct / 100));

            bool whale_capped = false;
            if (vic->coins > RICH_THRESHOLD)
            {
                int64_t rich_cap = std::max(int64_t(1),
                                            vic->coins * RICH_MAX_STEAL / 100);
                if (steal_raw > rich_cap)
                {
                    steal_raw   = rich_cap;
                    whale_capped = true;
                }
            }

            int reduction = std::min(60, inv_sum(*vic, &ItemDef::steal_reduction));
            int64_t steal = (reduction > 0)
                ? std::max(int64_t(1), steal_raw * (100 - reduction) / 100)
                : steal_raw;

            vic->coins  -= steal;
            att.coins   += steal;
            att.last_mug = now;

            // Bounty claim
            int64_t bounty_claim = 0;
            auto bit = s_bounties.find(get_target_account_key(vic_nick));
            if (bit != s_bounties.end())
            {
                bounty_claim = bit->second;
                att.coins   += bounty_claim;
                s_bounties.erase(bit);
            }

            Anope::string base_msg = mega
                ? tpl(rand_pick(MSG_MUG_MEGA),
                       {{"att", att.nick}, {"vic", vic->nick}, {"steal", fmt_coins(steal)}})
                : tpl(rand_pick(MSG_MUG_SUCCESS),
                       {{"att", att.nick}, {"vic", vic->nick}, {"steal", fmt_coins(steal)}});

            Anope::string whale_tag = whale_capped ? " [whale-capped]" : "";
            Anope::string bounty_tag = "";
            if (bounty_claim > 0)
                bounty_tag = " " + tpl(rand_pick(MSG_BOUNTY_CLAIM),
                             {{"att", att.nick}, {"vic", vic->nick},
                              {"bounty", fmt_coins(bounty_claim)}});

            Anope::string msg = base_msg + whale_tag + bounty_tag
                + " | " + att.nick + ": " + fmt_coins(att.coins)
                + " | " + vic->nick + ": " + fmt_coins(vic->coins);
            announce(src, msg);
            return;
        }

        // NORMAL FAIL
        if (roll <= eff_success + FAIL_CHANCE)
        {
            int pct = ri(FAIL_LOSS_MIN, FAIL_LOSS_MAX);
            int64_t loss = std::min({static_cast<int64_t>(att.coins * pct / 100),
                                     att.coins, MAX_FAIL_LOSS});
            loss = std::max(loss, int64_t(0));
            att.coins   -= loss;
            vic->coins  += loss;
            att.last_mug = now + CD_MUG_FAIL_XTRA; // extra penalty

            Anope::string msg = tpl(rand_pick(MSG_MUG_FAIL),
                {{"att", att.nick}, {"loss", fmt_coins(loss)}})
                + " | " + att.nick + ": " + fmt_coins(att.coins)
                + " | " + vic->nick + ": " + fmt_coins(vic->coins);
            announce(src, msg);
            return;
        }

        // CRIT FAIL
        do_crit_fail();
    }
};

// ─── BET ────────────────────────────────────────────────────────────────────
struct CommandMugBet : Command
{
    CommandMugBet(Module *c) : Command(c, "mugserv/BET", 1, 1)
    {
        SetDesc("Gamble coins");
        SetSyntax("<amount>");
    }

    void Execute(CommandSource &src, const std::vector<Anope::string> &params) override
    {
        if (check_gate(src)) return;
        MugUser &u = s_users[src.nc->display.lower()];

        int rem = cd_rem(u.last_bet, CD_BET);
        if (rem > 0)
        {
            pm(src, tpl(rand_pick(MSG_BET_CD), {{"t", fmt_dur(rem)}}));
            return;
        }

        int64_t amt = 0;
        try { amt = std::stoll(params[0].c_str()); } catch (...) {}
        if (amt < BET_MIN)
        {
            pm(src, "Minimum bet is " + fmt_coins(BET_MIN) + " coin.");
            return;
        }
        if (u.coins < amt)
        {
            pm(src, rand_pick(MSG_BROKE));
            return;
        }

        u.coins  -= amt;
        u.last_bet = Anope::CurTime;

        int win_chance = std::min(95,
            BET_WIN_BASE + std::min(35, inv_sum(u, &ItemDef::bet_bonus)));
        bool win = (ri(1, 100) <= win_chance);

        Anope::string msg;
        if (win)
        {
            int64_t payout = amt * 2;
            u.coins += payout;
            msg = "🎲 \002" + u.nick + "\002 bets " + fmt_coins(amt)
                + " and \002WINS\002! Payout: " + fmt_coins(payout)
                + ". Balance: \002" + fmt_coins(u.coins) + "\002 🤑";
        }
        else
        {
            msg = "💀 \002" + u.nick + "\002 bets " + fmt_coins(amt)
                + " and loses it all! Balance: \002" + fmt_coins(u.coins) + "\002 😭🎰";
        }

        announce(src, msg);
    }
};

// ─── BOUNTY ─────────────────────────────────────────────────────────────────
struct CommandMugBounty : Command
{
    CommandMugBounty(Module *c) : Command(c, "mugserv/BOUNTY", 2, 2)
    {
        SetDesc("Place a coin bounty on another registered player");
        SetSyntax("<nick> <amount>");
    }

    void Execute(CommandSource &src, const std::vector<Anope::string> &params) override
    {
        if (check_gate(src)) return;
        MugUser &placer = s_users[src.nc->display.lower()];

        Anope::string target = params[0];
        if (get_target_account_key(target) == src.nc->display.lower())
        {
            pm(src, "You can't bounty yourself. That's just therapy with extra steps.");
            return;
        }

        MugUser *vic = get_user_by_target(target);
        if (!vic)
        {
            pm(src, target + " has no MugServ account.");
            return;
        }

        int rem = cd_rem(placer.last_bounty, CD_BOUNTY);
        if (rem > 0)
        {
            pm(src, "Slow down, bounty goblin. Try again in " + fmt_dur(rem) + ".");
            return;
        }

        int64_t amt = 0;
        try { amt = std::stoll(params[1].c_str()); } catch (...) {}
        if (amt < BOUNTY_MIN_AMT)
        {
            pm(src, "Minimum bounty is " + fmt_coins(BOUNTY_MIN_AMT) + ".");
            return;
        }
        if (amt > BOUNTY_MAX_AMT)
        {
            pm(src, "Max bounty per placement is " + fmt_coins(BOUNTY_MAX_AMT) + ".");
            return;
        }
        if (placer.coins < amt)
        {
            pm(src, rand_pick(MSG_BROKE));
            return;
        }

        placer.coins -= amt;
        placer.last_bounty = Anope::CurTime;
        Anope::string vic_key = get_target_account_key(target);
        s_bounties[vic_key] = s_bounties.count(vic_key)
            ? s_bounties[vic_key] + amt
            : amt;

        Anope::string msg = tpl(rand_pick(MSG_BOUNTY_PLACE),
            {{"vic", vic->nick}, {"amt", fmt_coins(amt)}})
            + " (" + placer.nick + " now has " + fmt_coins(placer.coins) + " coins)";
        announce(src, msg);
    }
};

// ─── BOUNTIES ────────────────────────────────────────────────────────────────
struct CommandMugBounties : Command
{
    CommandMugBounties(Module *c) : Command(c, "mugserv/BOUNTIES", 0, 0)
    {
        SetDesc("List the top active bounties");
    }

    void Execute(CommandSource &src, const std::vector<Anope::string> &) override
    {
        if (s_bounties.empty())
        {
            pm(src, "🎯 No active bounties. Everyone is (unfortunately) safe.");
            return;
        }

        std::vector<std::pair<int64_t, Anope::string>> list;
        for (const auto &kv : s_bounties)
            list.push_back({kv.second, kv.first});
        std::sort(list.begin(), list.end(),
                  [](const auto &a, const auto &b){ return a.first > b.first; });

        Anope::string out = "🔥 Top bounties:";
        int shown = 0;
        for (const auto &kv : list)
        {
            if (shown >= 10) break;
            // Resolve display nick from users map
            MugUser *u = get_user(kv.second);
            Anope::string dn = u ? u->nick : kv.second;
            out += " | 🎯 " + dn + "(" + fmt_coins(kv.first) + ")";
            ++shown;
        }
        pm(src, out);
    }
};

// ─── SHOP ────────────────────────────────────────────────────────────────────
struct CommandMugShop : Command
{
    CommandMugShop(Module *c) : Command(c, "mugserv/SHOP", 0, 0)
    {
        SetDesc("View available items in the crime shop");
    }

    void Execute(CommandSource &src, const std::vector<Anope::string> &) override
    {
        pm(src, "🛒 \002MugServ Crime Shop\002 — use \002BUY <key>\002 to purchase:");
        pm(src, "─────────────────────────────────────────────────────");
        for (int i = 0; i < NUM_ITEMS; ++i)
        {
            pm(src, "  \002" + Anope::string(ITEMS[i].key) + "\002 → "
                + ITEMS[i].name
                + " (" + fmt_coins(ITEMS[i].price) + " coins)"
                + " — " + ITEMS[i].desc
                + " [max 3 per player]");
        }
        pm(src, "─────────────────────────────────────────────────────");
        pm(src, "Passive items (mask/knucks/luckycoin/vest/cloak/banana) work automatically.");
        pm(src, "Bail is a consumable: freed from jail instantly when you buy/use it.");
    }
};

// ─── BUY ─────────────────────────────────────────────────────────────────────
struct CommandMugBuy : Command
{
    CommandMugBuy(Module *c) : Command(c, "mugserv/BUY", 1, 1)
    {
        SetDesc("Purchase an item from the shop");
        SetSyntax("<item-key>");
    }

    void Execute(CommandSource &src, const std::vector<Anope::string> &params) override
    {
        if (check_gate(src)) return;
        MugUser &u = s_users[src.nc->display.lower()];

        int idx = find_item(params[0]);
        if (idx < 0)
        {
            pm(src, "Unknown item '\002" + params[0] + "\002'. Send SHOP to see valid keys.");
            return;
        }

        const ItemDef &item = ITEMS[idx];

        if (u.inv[idx] >= 3)
        {
            pm(src, "You already have 3 \002" + Anope::string(item.name) + "\002. "
                    "That's the max! (Stack limit: 3)");
            return;
        }
        if (u.coins < item.price)
        {
            pm(src, "Not enough coins for \002" + Anope::string(item.name) + "\002 "
                    "(costs " + fmt_coins(item.price) + "). Farm more with COINS.");
            return;
        }

        u.coins -= item.price;
        u.inv[idx]++;

        // Auto-use bail if bought while jailed
        if (item.is_bail && u.jail_until > Anope::CurTime)
        {
            u.inv[idx]--;
            u.jail_until = 0;
            pm(src, "🪓🎉 \002" + u.nick + "\002 bought \002" + Anope::string(item.name)
                    + "\002 for " + fmt_coins(item.price)
                    + " coins and got bailed out! Welcome back to freedom. "
                    "(Mug cooldown still active.) Balance: " + fmt_coins(u.coins));
        }
        else
        {
            pm(src, "✅ Bought \002" + Anope::string(item.name) + "\002 for "
                    + fmt_coins(item.price) + " coins. Balance: " + fmt_coins(u.coins));
        }
    }
};

// ─── INV ─────────────────────────────────────────────────────────────────────
struct CommandMugInv : Command
{
    CommandMugInv(Module *c) : Command(c, "mugserv/INV", 0, 0)
    {
        SetDesc("View your item inventory");
    }

    void Execute(CommandSource &src, const std::vector<Anope::string> &) override
    {
        if (check_gate(src)) return;
        MugUser &u = s_users[src.nc->display.lower()];

        bool has_any = false;
        for (int i = 0; i < NUM_ITEMS; ++i)
            if (u.inv[i] > 0) { has_any = true; break; }

        if (!has_any)
        {
            pm(src, "🎒 Your inventory is empty. Send SHOP to browse items.");
            return;
        }

        pm(src, "🎒 \002" + u.nick + "'s inventory\002:");
        for (int i = 0; i < NUM_ITEMS; ++i)
        {
            if (u.inv[i] <= 0) continue;
            pm(src, "  \002" + Anope::string(ITEMS[i].key) + "\002 x"
                    + stringify(u.inv[i])
                    + " — " + ITEMS[i].name
                    + " (" + ITEMS[i].desc + ")");
        }
    }
};

// ─── USE ─────────────────────────────────────────────────────────────────────
struct CommandMugUse : Command
{
    CommandMugUse(Module *c) : Command(c, "mugserv/USE", 1, 1)
    {
        SetDesc("Use a consumable item (e.g., bail)");
        SetSyntax("<item-key>");
    }

    void Execute(CommandSource &src, const std::vector<Anope::string> &params) override
    {
        if (check_gate(src)) return;
        MugUser &u = s_users[src.nc->display.lower()];

        int idx = find_item(params[0]);
        if (idx < 0)
        {
            pm(src, "Unknown item '\002" + params[0] + "\002'. Send INV to see your items.");
            return;
        }
        if (u.inv[idx] <= 0)
        {
            pm(src, "You don't have any \002" + Anope::string(ITEMS[idx].name)
                    + "\002. Send SHOP to buy one.");
            return;
        }
        if (!ITEMS[idx].is_bail)
        {
            pm(src, "\002" + Anope::string(ITEMS[idx].name)
                    + "\002 is a passive item — it works automatically from your inventory. "
                    "No need to use it manually.");
            return;
        }

        // Bail
        if (u.jail_until > Anope::CurTime)
        {
            u.inv[idx]--;
            u.jail_until = 0;
            pm(src, "🪓🎉 \002" + u.nick + "\002 used \002Bail Bondsman\002 and is FREE! "
                    "(Mug cooldown still active.) Stay out of trouble.");
        }
        else
        {
            pm(src, "You're not in jail. Your \002Bail Bondsman\002 stays in your pocket "
                    "for when you actually need it.");
        }
    }
};

// ─── TOP5 ─────────────────────────────────────────────────────────────────────
struct CommandMugTop5 : Command
{
    CommandMugTop5(Module *c) : Command(c, "mugserv/TOP5", 0, 0)
    {
        SetDesc("Show the top 5 richest players");
    }

    void Execute(CommandSource &src, const std::vector<Anope::string> &) override
    {
        std::vector<MugUser*> sorted;
        for (auto &kv : s_users) sorted.push_back(&kv.second);
        std::sort(sorted.begin(), sorted.end(),
                  [](MugUser *a, MugUser *b){ return a->coins > b->coins; });
        if (sorted.size() > 5) sorted.resize(5);

        if (sorted.empty())
        {
            pm(src, "No coin data yet. Register and use COINS to get started!");
            return;
        }
        pm(src, "🏆 Top 5: " + format_lb(sorted, 1));
    }
};

// ─── TOP10 ────────────────────────────────────────────────────────────────────
struct CommandMugTop10 : Command
{
    CommandMugTop10(Module *c) : Command(c, "mugserv/TOP10", 0, 0)
    {
        SetDesc("Show the top 10 richest players");
    }

    void Execute(CommandSource &src, const std::vector<Anope::string> &) override
    {
        std::vector<MugUser*> sorted;
        for (auto &kv : s_users) sorted.push_back(&kv.second);
        std::sort(sorted.begin(), sorted.end(),
                  [](MugUser *a, MugUser *b){ return a->coins > b->coins; });
        if (sorted.size() > 10) sorted.resize(10);

        if (sorted.empty())
        {
            pm(src, "No coin data yet. Register and use COINS to get started!");
            return;
        }

        // Split into two lines to avoid IRC line-length issues
        size_t half = sorted.size() > 5 ? 5 : sorted.size();
        std::vector<MugUser*> first(sorted.begin(), sorted.begin() + static_cast<ptrdiff_t>(half));
        std::vector<MugUser*> second(sorted.begin() + static_cast<ptrdiff_t>(half), sorted.end());

        pm(src, "💰 Top 10 (1-5):   " + format_lb(first, 1));
        if (!second.empty())
            pm(src, "💰 Top 10 (6-10):  " + format_lb(second, 6));
    }
};

// ─── Admin: MUGADD ───────────────────────────────────────────────────────────
struct CommandMugAdd : Command
{
    CommandMugAdd(Module *c) : Command(c, "mugserv/MUGADD", 2, 2)
    {
        SetDesc("[Admin] Add coins to a player");
        SetSyntax("<nick> <amount>");
    }

    void Execute(CommandSource &src, const std::vector<Anope::string> &params) override
    {
        if (!is_admin(src))
        {
            pm(src, "Access denied.");
            return;
        }
        MugUser *u = get_user(params[0]);
        if (!u)
        {
            pm(src, params[0] + " is not registered with MugServ.");
            return;
        }
        int64_t amt = 0;
        try { amt = std::stoll(params[1].c_str()); } catch (...) {}
        if (amt <= 0) { pm(src, "Amount must be > 0."); return; }
        u->coins += amt;
        pm(src, "✅ Added " + fmt_coins(amt) + " coins to \002" + u->nick
                + "\002. New balance: " + fmt_coins(u->coins));
    }
};

// ─── Admin: MUGSET ───────────────────────────────────────────────────────────
struct CommandMugSet : Command
{
    CommandMugSet(Module *c) : Command(c, "mugserv/MUGSET", 2, 2)
    {
        SetDesc("[Admin] Set a player's coin balance");
        SetSyntax("<nick> <amount>");
    }

    void Execute(CommandSource &src, const std::vector<Anope::string> &params) override
    {
        if (!is_admin(src)) { pm(src, "Access denied."); return; }
        MugUser *u = get_user(params[0]);
        if (!u) { pm(src, params[0] + " is not registered."); return; }
        int64_t amt = 0;
        try { amt = std::stoll(params[1].c_str()); } catch (...) {}
        if (amt < 0) { pm(src, "Amount must be >= 0."); return; }
        u->coins = amt;
        pm(src, "✅ Set \002" + u->nick + "\002's balance to " + fmt_coins(amt));
    }
};

// ─── Admin: MUGTAKE ──────────────────────────────────────────────────────────
struct CommandMugTake : Command
{
    CommandMugTake(Module *c) : Command(c, "mugserv/MUGTAKE", 2, 2)
    {
        SetDesc("[Admin] Remove coins from a player");
        SetSyntax("<nick> <amount>");
    }

    void Execute(CommandSource &src, const std::vector<Anope::string> &params) override
    {
        if (!is_admin(src)) { pm(src, "Access denied."); return; }
        MugUser *u = get_user(params[0]);
        if (!u) { pm(src, params[0] + " is not registered."); return; }
        int64_t amt = 0;
        try { amt = std::stoll(params[1].c_str()); } catch (...) {}
        if (amt <= 0) { pm(src, "Amount must be > 0."); return; }
        u->coins = std::max(int64_t(0), u->coins - amt);
        pm(src, "✅ Took " + fmt_coins(amt) + " coins from \002" + u->nick
                + "\002. New balance: " + fmt_coins(u->coins));
    }
};

// ─── Admin: MUGRESET ─────────────────────────────────────────────────────────
struct CommandMugReset : Command
{
    CommandMugReset(Module *c) : Command(c, "mugserv/MUGRESET", 0, 1)
    {
        SetDesc("[Admin] Reset all player data to zero (DESTRUCTIVE)");
        SetSyntax("[confirm]");
    }

    void Execute(CommandSource &src, const std::vector<Anope::string> &params) override
    {
        if (!is_admin(src)) { pm(src, "Access denied."); return; }

        if (params.empty() || params[0].lower() != "confirm")
        {
            pm(src, "⚠️  This resets ALL player balances, cooldowns, inventories, and bounties.");
            pm(src, "Send \002MUGRESET confirm\002 to proceed.");
            return;
        }

        for (auto &kv : s_users)
        {
            MugUser &u = kv.second;
            u.coins = u.last_coins = u.last_mug = u.jail_until = 0;
            u.last_bet = u.last_give = u.last_bounty = 0;
            u.daily_given = u.daily_reset = 0;
            for (int i = 0; i < NUM_ITEMS; ++i) u.inv[i] = 0;
        }
        s_bounties.clear();
        save_db();
        pm(src, "🧨 FULL RESET DONE. Everyone is broke again. Society restored. ✅");
    }
};

// ─── Admin: MUGSTATS ─────────────────────────────────────────────────────────
struct CommandMugStats : Command
{
    CommandMugStats(Module *c) : Command(c, "mugserv/MUGSTATS", 0, 0)
    {
        SetDesc("[Admin] Economy overview statistics");
    }

    void Execute(CommandSource &src, const std::vector<Anope::string> &) override
    {
        if (!is_admin(src)) { pm(src, "Access denied."); return; }

        int64_t total = 0;
        for (const auto &kv : s_users) total += kv.second.coins;

        std::vector<MugUser*> sorted;
        for (auto &kv : s_users) sorted.push_back(&kv.second);
        std::sort(sorted.begin(), sorted.end(),
                  [](MugUser *a, MugUser *b){ return a->coins > b->coins; });
        if (sorted.size() > 5) sorted.resize(5);

        pm(src, "📊 \002MugServ Economy Stats\002");
        pm(src, "  Players in DB: \002" + stringify(static_cast<int>(s_users.size())) + "\002");
        pm(src, "  Active bounties: \002" + stringify(static_cast<int>(s_bounties.size())) + "\002");
        pm(src, "  Total coins in economy: \002" + fmt_coins(total) + "\002");
        if (!sorted.empty())
            pm(src, "  Top 5: " + format_lb(sorted, 1));

        if (!s_channels.empty())
        {
            Anope::string chlist;
            for (const auto &ch : s_channels) { if (!chlist.empty()) chlist += " "; chlist += ch; }
            pm(src, "  Active channels (" + stringify(static_cast<int>(s_channels.size())) + "): " + chlist);
            pm(src, "  Command prefix: " + s_cmd_prefix);
        }
        else
        {
            pm(src, "  Active channels: none (PM only)");
        }
        pm(src, "  NickServ identification: always required");
        pm(src, "  DB path: " + db_path());
    }
};

// ─── HELP ─────────────────────────────────────────────────────────────────────
struct CommandMugHelp : Command
{
    CommandMugHelp(Module *c) : Command(c, "mugserv/HELP", 0, 1)
    {
        SetDesc("Show MugServ command help");
        SetSyntax("[command]");
    }

    void Execute(CommandSource &src, const std::vector<Anope::string> &) override
    {
        pm(src, "📖 \002MugServ Help\002 — /msg MugServ <COMMAND>  or  " + s_cmd_prefix + "command in an active channel");
        pm(src, "");
        pm(src, "\002Access\002");
        pm(src, "  A NickServ account is required. Identify first: /msg NickServ IDENTIFY <pass>");
        pm(src, "  You are automatically enrolled the first time you send any command.");
        pm(src, "  In channels, prefix commands with \002" + s_cmd_prefix + "\002 (e.g. \002" + s_cmd_prefix + "coins\002).");
        pm(src, "");
        pm(src, "\002Economy\002");
        pm(src, "  COINS                 — Collect coins (10-min cooldown, scales with wealth)");
        pm(src, "  BALANCE [nick]        — Check your (or another player's) balance");
        pm(src, "  GIVE <nick> <amount>  — Give coins to another player (5-min CD, daily cap)");
        pm(src, "");
        pm(src, "\002Bounties\002");
        pm(src, "  BOUNTY <nick> <amt>   — Place a coin bounty on someone (costs you coins)");
        pm(src, "  BOUNTIES              — List top active bounties");
        pm(src, "                          Mugger who hits a bounty target claims the pool.");
        pm(src, "");
        pm(src, "\002Mugging\002");
        pm(src, "  MUG <nick>            — Attempt to mug a registered player");
        pm(src, "  ROB <nick>            — Alias for MUG");
        pm(src, "    60% base success: steal 10-30% of victim's coins");
        pm(src, "    25% normal fail:  you drop coins (+ extra cooldown)");
        pm(src, "    15% crit fail:    you lose big + jail (no mugs until free)");
        pm(src, "    Rare: mega heists, oops-jail, whale protection at >10k coins");
        pm(src, "  JAIL                  — Check your current jail status");
        pm(src, "");
        pm(src, "\002Gambling\002");
        pm(src, "  BET <amount>          — 40% chance to double your bet (Lucky Coin improves odds)");
        pm(src, "");
        pm(src, "\002Shop & Inventory\002");
        pm(src, "  SHOP                  — Browse available items");
        pm(src, "  BUY <key>             — Buy an item (max 3 of each)");
        pm(src, "  INV                   — View your inventory");
        pm(src, "  USE <key>             — Use a consumable item (e.g., bail)");
        pm(src, "");
        pm(src, "\002Items\002  (passive bonuses stack up to 3x)");
        pm(src, "  mask      120c  — +7% mug success per stack");
        pm(src, "  knucks    250c  — +6% steal per stack on success");
        pm(src, "  luckycoin 180c  — +3 flat COINS bonus; +7% BET win chance per stack");
        pm(src, "  vest      220c  — -20% stolen per stack (victim)");
        pm(src, "  cloak     500c  — 15% dodge chance per stack (victim)");
        pm(src, "  banana     50c  — 5% mugger-slip chance per stack (victim, triggers crit fail)");
        pm(src, "  bail     5000c  — consumable: instantly frees you from jail once");
        pm(src, "");
        pm(src, "\002Leaderboards\002");
        pm(src, "  TOP5                  — Top 5 richest players");
        pm(src, "  TOP10                 — Top 10 richest players");
        pm(src, "");
        pm(src, "\002Admin (IRCops and configured admin_nicks only)\002");
        pm(src, "  MUGADD <nick> <amt>   — Add coins");
        pm(src, "  MUGSET <nick> <amt>   — Set balance");
        pm(src, "  MUGTAKE <nick> <amt>  — Remove coins");
        pm(src, "  MUGRESET [confirm]    — Reset ALL data");
        pm(src, "  MUGSTATS              — Economy overview");
        pm(src, "  ENABLE <#channel>     — Add a channel (bot joins + listens)");
        pm(src, "  DISABLE <#channel>    — Remove a channel");
        pm(src, "");
        pm(src, "\002Channel Usage\002");
        pm(src, "  In any active channel, prefix commands with \002" + s_cmd_prefix + "\002:");
        pm(src, "  " + s_cmd_prefix + "coins   " + s_cmd_prefix + "mug Nick   "
                + s_cmd_prefix + "bet 100   " + s_cmd_prefix + "top5");
        pm(src, "  SHOP, BUY, INV, USE and admin commands always reply via PM.");
        pm(src, "  You can also always /msg MugServ directly.");
    }
};

// ─── Admin: ENABLE ──────────────────────────────────────────────────────────
struct CommandMugEnable : Command
{
    CommandMugEnable(Module *c) : Command(c, "mugserv/ENABLE", 1, 1)
    {
        SetDesc("[Admin] Enable MugServ in a channel");
        SetSyntax("<#channel>");
    }

    void Execute(CommandSource &src, const std::vector<Anope::string> &params) override
    {
        if (!is_admin(src)) { pm(src, "Access denied."); return; }
        Anope::string chan = params[0].lower();
        if (chan.empty() || chan[0] != '#')
        {
            pm(src, "Provide a channel name starting with #.");
            return;
        }
        if (s_channels.count(chan))
        {
            pm(src, chan + " is already active.");
            return;
        }
        s_channels.insert(chan);
        // Have the bot join the channel if not already there.
        if (s_bot)
        {
            Channel *c = Channel::Find(chan);
            if (!c || !c->FindUser(s_bot))
                s_bot->Join(chan);
        }
        save_db();
        pm(src, "✅ MugServ is now active in " + chan + ".");
    }
};

// ─── Admin: DISABLE ─────────────────────────────────────────────────────────
struct CommandMugDisable : Command
{
    CommandMugDisable(Module *c) : Command(c, "mugserv/DISABLE", 1, 1)
    {
        SetDesc("[Admin] Disable MugServ in a channel");
        SetSyntax("<#channel>");
    }

    void Execute(CommandSource &src, const std::vector<Anope::string> &params) override
    {
        if (!is_admin(src)) { pm(src, "Access denied."); return; }
        Anope::string chan = params[0].lower();
        if (!s_channels.count(chan))
        {
            pm(src, chan + " is not in the active channel list.");
            return;
        }
        s_channels.erase(chan);
        // Part the channel if the bot is there and no other reason to stay.
        if (s_bot)
        {
            Channel *c = Channel::Find(chan);
            if (c && c->FindUser(s_bot))
                s_bot->Part(c);
        }
        save_db();
        pm(src, "✅ MugServ disabled in " + chan + ".");
    }
};

// ===========================================================================
// Module class
// ===========================================================================

class ModuleMugServ : public Module
{
    // All command instances — order matches OnReload binding list
    CommandMugCoins     cmd_coins;
    CommandMugBalance   cmd_balance;
    CommandMugGive      cmd_give;
    CommandMugMug       cmd_mug;
    CommandMugBet       cmd_bet;
    CommandMugBounty    cmd_bounty;
    CommandMugBounties  cmd_bounties;
    CommandMugJail      cmd_jail;
    CommandMugShop      cmd_shop;
    CommandMugBuy       cmd_buy;
    CommandMugInv       cmd_inv;
    CommandMugUse       cmd_use;
    CommandMugTop5      cmd_top5;
    CommandMugTop10     cmd_top10;
    CommandMugAdd       cmd_mugadd;
    CommandMugSet       cmd_mugset;
    CommandMugTake      cmd_mugtake;
    CommandMugReset     cmd_mugreset;
    CommandMugStats     cmd_mugstats;
    CommandMugHelp      cmd_help;
    CommandMugEnable    cmd_enable;
    CommandMugDisable   cmd_disable;

    MugSaveTimer        save_timer;

public:
    ModuleMugServ(const Anope::string &modname, const Anope::string &creator)
        : Module(modname, creator, VENDOR)
        , cmd_coins(this),    cmd_balance(this)
        , cmd_give(this),      cmd_mug(this),       cmd_bet(this)
        , cmd_bounty(this),    cmd_bounties(this),  cmd_jail(this)
        , cmd_shop(this),      cmd_buy(this),       cmd_inv(this)
        , cmd_use(this),       cmd_top5(this),      cmd_top10(this)
        , cmd_mugadd(this),    cmd_mugset(this),    cmd_mugtake(this)
        , cmd_mugreset(this),  cmd_mugstats(this),  cmd_help(this)
        , cmd_enable(this),    cmd_disable(this)
        , save_timer()
    {
        s_module = this;
        load_db();
    }

    ~ModuleMugServ() override
    {
        save_db();
        s_module = nullptr;
    }

    void OnReload(Configuration::Conf *conf) override
    {
        Configuration::Block *block = conf->GetModule(this);

        const Anope::string cname = block->Get<Anope::string>("client", "MugServ");
        s_bot = BotInfo::Find(cname, true);
        if (!s_bot)
            throw ConfigException(this->name + ": no service bot named \"" + cname + "\". "
                "Add a 'service { nick = \"" + cname + "\"; ... }' block to services.conf.");

        // Parse channels from config (space-separated); DB-persisted channels are
        // already in s_channels from load_db(), so we only add config ones here.
        {
            Anope::string ch_str = block->Get<Anope::string>("channels", "");
            if (!ch_str.empty())
            {
                std::istringstream ss(ch_str.c_str());
                std::string tok;
                while (ss >> tok)
                    s_channels.insert(Anope::string(tok).lower());
            }
        }

        s_cmd_prefix = block->Get<Anope::string>("cmd_prefix", "!");
        if (s_cmd_prefix.empty()) s_cmd_prefix = "!";

        // Parse admin_nicks (space-separated)
        s_admin_nicks.clear();
        Anope::string an_str = block->Get<Anope::string>("admin_nicks", "");
        if (!an_str.empty())
        {
            std::istringstream ss(an_str.c_str());
            std::string tok;
            while (ss >> tok)
                s_admin_nicks.push_back(Anope::string(tok).lower());
        }

        // Bind commands to the service bot.
        // Format: bot->SetCommand("VERB", "mugserv/VERB")
        s_bot->SetCommand("COINS",     "mugserv/COINS");
        s_bot->SetCommand("BALANCE",   "mugserv/BALANCE");
        s_bot->SetCommand("BAL",       "mugserv/BALANCE");
        s_bot->SetCommand("GIVE",      "mugserv/GIVE");
        s_bot->SetCommand("MUG",       "mugserv/MUG");
        s_bot->SetCommand("ROB",       "mugserv/MUG");
        s_bot->SetCommand("BET",       "mugserv/BET");
        s_bot->SetCommand("BOUNTY",    "mugserv/BOUNTY");
        s_bot->SetCommand("BOUNTIES",  "mugserv/BOUNTIES");
        s_bot->SetCommand("JAIL",      "mugserv/JAIL");
        s_bot->SetCommand("SHOP",      "mugserv/SHOP");
        s_bot->SetCommand("BUY",       "mugserv/BUY");
        s_bot->SetCommand("INV",       "mugserv/INV");
        s_bot->SetCommand("INVENTORY", "mugserv/INV");
        s_bot->SetCommand("USE",       "mugserv/USE");
        s_bot->SetCommand("TOP5",      "mugserv/TOP5");
        s_bot->SetCommand("TOP10",     "mugserv/TOP10");
        s_bot->SetCommand("MUGADD",    "mugserv/MUGADD");
        s_bot->SetCommand("MUGSET",    "mugserv/MUGSET");
        s_bot->SetCommand("MUGTAKE",   "mugserv/MUGTAKE");
        s_bot->SetCommand("MUGRESET",  "mugserv/MUGRESET");
        s_bot->SetCommand("MUGSTATS",  "mugserv/MUGSTATS");
        s_bot->SetCommand("HELP",      "mugserv/HELP");
        s_bot->SetCommand("COMMANDS",  "mugserv/HELP");
        s_bot->SetCommand("ENABLE",    "mugserv/ENABLE");
        s_bot->SetCommand("DISABLE",   "mugserv/DISABLE");

        // Join all configured channels.
        for (const auto &chan : s_channels)
        {
            Channel *c = Channel::Find(chan);
            if (!c || !c->FindUser(s_bot))
                s_bot->Join(chan);
        }
    }

    // Intercept channel messages for !command triggers.
    void OnMessage(MessageSource &source, Anope::string &target, Anope::string &msg) override
    {
        // Only channel messages to active channels.
        if (target.empty() || target[0] != '#') return;
        if (!s_channels.count(target.lower())) return;
        if (!s_bot) return;
        if (msg.length() <= s_cmd_prefix.length()) return;
        if (msg.substr(0, s_cmd_prefix.length()) != s_cmd_prefix) return;

        User *u = source.GetUser();
        if (!u) return;

        // Strip prefix; split verb and args.
        Anope::string rest = msg.substr(s_cmd_prefix.length());
        size_t sp = rest.find(' ');
        Anope::string verb  = (sp == Anope::string::npos) ? rest : rest.substr(0, sp);
        Anope::string argstr = (sp == Anope::string::npos) ? "" : rest.substr(sp + 1);
        if (verb.empty()) return;

        // Commands that must be used in PM only.
        static const std::vector<Anope::string> pm_only = {
            "shop", "buy", "inv", "inventory", "use",
            "mugadd", "mugset", "mugtake", "mugreset", "mugstats",
            "enable", "disable"
        };
        Anope::string lverb = verb.lower();
        for (const auto &v : pm_only)
        {
            if (lverb == v)
            {
                s_bot->Say(target, u->GetNick() + ": Please \002/msg "
                           + s_bot->GetNick() + " " + verb.upper()
                           + (argstr.empty() ? "" : " " + argstr)
                           + "\002 for that command.");
                return;
            }
        }

        // NickServ gate.
        NickAlias *na = NickAlias::Find(u->GetNick());
        if (!na || !na->nc)
        {
            s_bot->Say(target, u->GetNick()
                + ": You must be identified with NickServ to play MugServ.");
            return;
        }

        NickCore *nc = na->nc;
        Anope::string acct_key = nc->display.lower();

        // Auto-enroll.
        if (!s_users.count(acct_key))
        {
            MugUser nu;
            nu.account = acct_key;
            nu.nick    = u->GetNick();
            s_users[acct_key] = nu;
            s_bot->Say(target, u->GetNick()
                + ": Welcome to MugServ! You've been enrolled. Type "
                + s_cmd_prefix + "help for commands.");
        }
        else
        {
            s_users[acct_key].nick = u->GetNick();
        }

        // Parse params.
        std::vector<Anope::string> params;
        if (!argstr.empty())
        {
            std::istringstream ss(argstr.c_str());
            std::string tok;
            while (ss >> tok)
                params.push_back(Anope::string(tok));
        }

        // Resolve canonical verb name.
        Anope::string svcmd = verb.upper();
        if (svcmd == "ROB")       svcmd = "MUG";
        if (svcmd == "BAL")       svcmd = "BALANCE";
        if (svcmd == "INVENTORY") svcmd = "INV";
        if (svcmd == "COMMANDS")  svcmd = "HELP";

        BotInfo::command_map::iterator ci = s_bot->commands.find(svcmd);
        if (ci == s_bot->commands.end()) return;

        Command *cmd = Service::Find<Command>("Command", ci->second.name);
        if (!cmd) return;

        // Min params check.
        if (static_cast<int>(params.size()) < cmd->min_params)
        {
            s_bot->Say(target, u->GetNick() + ": Usage: " + s_cmd_prefix
                       + svcmd.lower() + " " + cmd->GetSyntax());
            return;
        }

        // Set channel context so announce() routes to this channel.
        s_current_chan = target.lower();
        CommandSource fake_src(u->GetNick(), u, nc, nullptr, s_bot);
        cmd->Execute(fake_src, params);
        s_current_chan = "";
    }

    // Hook into Anope's periodic database save cycle.
    EventReturn OnSaveDatabase() override
    {
        save_db();
        return EVENT_CONTINUE;
    }
};

MODULE_INIT(ModuleMugServ)
