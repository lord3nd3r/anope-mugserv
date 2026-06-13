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
#include <cstddef>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <map>
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
static const long long COINS_SCALE_CAP  = 1500;

static const long long MUG_FEE          = 2;
static const int      STEAL_MIN         = 10;     // pct
static const int      STEAL_MAX         = 30;
static const int      FAIL_LOSS_MIN     = 5;
static const int      FAIL_LOSS_MAX     = 15;
static const int      CRIT_LOSS_MIN     = 20;
static const int      CRIT_LOSS_MAX     = 40;
static const long long MAX_FAIL_LOSS    = 100000LL;
static const long long MAX_CRIT_LOSS    = 250000LL;

static const int      SUCCESS_CHANCE    = 60;     // 1-100 roll
static const int      FAIL_CHANCE       = 25;     // remaining % = crit fail

static const int      BET_WIN_BASE      = 40;     // pct
static const int      BET_MIN           = 1;

static const long long RICH_THRESHOLD   = 10000LL;
static const int      RICH_MAX_STEAL    = 25;     // pct cap vs rich targets

static const int      BANANA_SLIP_PCT   = 5;      // per banana item
static const int      BANANA_SLIP_MAX   = 25;

static const int      MEGA_STEAL_CHANCE = 1;      // pct
static const int      MEGA_STEAL_BONUS  = 25;     // extra steal pct
static const int      OOPS_JAIL_CHANCE  = 1;      // pct instant jail

static const int      BOUNTY_MIN_AMT    = 10;
static const long long BOUNTY_MAX_AMT   = 100000LL;
static const long long GIVE_DAILY_LIMIT = 500000LL;
static const int      GIVE_DAY_SECS     = 86400;
static const long long MAX_COINS        = 999999999999LL; // sanity cap
static const int      GLOBAL_CMD_CD     = 3;  // seconds between any command per user

// Dynamic mug fee: max(MUG_FEE, MUG_FEE_PCT% of attacker balance)
static const double   MUG_FEE_PCT       = 0.1; // 0.1% of attacker balance
// Attacker must have >= MUG_MIN_BAL_PCT% of victim's coins (skip if victim <= FLOOR)
static const int      MUG_MIN_BAL_PCT   = 1;
static const int      MUG_MIN_BAL_FLOOR = 100;

// Slot machine cooldowns
static const int      PENNY_COOLDOWN    = 15;
static const int      PENNY_COST        = 1;
static const int      DOLLAR_COOLDOWN   = 30;
static const int      DOLLAR_COST       = 100;

// Per-user cooldown for top5/top10
static const int      TOP_CD            = 600;

// Flood / spam gate
static const int      FLOOD_WINDOW      = 60;    // rolling window in seconds
static const int      FLOOD_MAX_CMDS    = 15;    // max commands before lockout
static const int      FLOOD_LOCKOUT_DUR = 1800;  // 30-minute lockout

// ===========================================================================
// Item table  (index 0-6 maps to ITEM_KEYS[])
// ===========================================================================

struct ItemDef
{
    const char *key, *name;
    long long   price;
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

static const int NUM_ITEMS = 7;

static const ItemDef ITEMS[7] =
{
    { "mask",      "Heist Mask",     120,  "Boosts mug success chance.",                7, 0, 0, 0,  0,  0, 0,                       false },
    { "knucks",    "Brass Knuckles", 250,  "Steal more on successful mugs.",            0, 6, 0, 0,  0,  0, 0,                       false },
    { "luckycoin", "Lucky Coin",     180,  "Extra COINS + better BET odds.",            0, 0, 3, 7,  0,  0, 0,                       false },
    { "vest",      "Kevlar Vest",    220,  "Reduces how much others steal from you.",   0, 0, 0, 0, 20,  0, 0,                       false },
    { "cloak",     "Shadow Cloak",   500,  "Chance to dodge a successful mug.",         0, 0, 0, 0,  0, 15, 0,                       false },
    { "banana",    "Banana Peel",     50,  "Muggers may slip into disaster.",           0, 0, 0, 0,  0,  0, 5 /*BANANA_SLIP_PCT*/,   false },
    { "bail",      "Bail Bondsman", 5000,  "Frees you from jail once.",                 0, 0, 0, 0,  0,  0, 0,                       true  },
};

// Named indices into ITEMS[] — keep in sync with the array above.
static const int ITEM_MASK    = 0;
static const int ITEM_KNUCKS  = 1;
static const int ITEM_LUCKY   = 2;
static const int ITEM_VEST    = 3;
static const int ITEM_CLOAK   = 4;
static const int ITEM_BANANA  = 5;
static const int ITEM_BAIL    = 6;

static int find_item(const Anope::string &key)
{
    Anope::string lk = key.lower();
    for (int i = 0; i < NUM_ITEMS; ++i)
        if (lk == ITEMS[i].key)
            return i;
    return -1;
}

// ===========================================================================
// RNG (C++03-compatible using srand/rand)
// ===========================================================================

static bool s_rng_seeded = false;

static int ri(int lo, int hi)   // inclusive[lo,hi]
{
    if (!s_rng_seeded)
    {
        std::srand(static_cast<unsigned>(std::time(NULL)));
        s_rng_seeded = true;
    }
    if (lo > hi) return lo;
    return lo + (std::rand() % (hi - lo + 1));
}

static Anope::string rand_pick_str(const std::vector<Anope::string> &v)
{
    return v[static_cast<size_t>(ri(0, static_cast<int>(v.size()) - 1))];
}

// ===========================================================================
// Formatting helpers
// ===========================================================================

static Anope::string fmt_coins(long long n)
{
    bool neg = n < 0;
    unsigned long long val;
    if (neg)
        val = static_cast<unsigned long long>(-(n + 1)) + 1u;
    else
        val = static_cast<unsigned long long>(n);
    std::ostringstream oss;
    oss << val;
    std::string s = oss.str();
    for (int p = static_cast<int>(s.size()) - 3; p > 0; p -= 3)
        s.insert(static_cast<size_t>(p), ",");
    if (neg) s.insert(0, "-");
    return Anope::string(s);
}

static Anope::string fmt_dur(int sec)
{
    if (sec <= 0) return Anope::string("0s");
    if (sec < 60)  return stringify(sec) + "s";
    int m = sec / 60, s2 = sec % 60;
    if (m < 60) return stringify(m) + "m" + (s2 ? " " + stringify(s2) + "s" : "");
    int h = m / 60; m %= 60;
    return stringify(h) + "h" + (m ? " " + stringify(m) + "m" : "");
}

static long long parse_ll(const Anope::string &s)
{
    std::istringstream iss(s.c_str());
    long long v = 0;
    iss >> v;
    return v;
}

// ===========================================================================
// Player record
// ===========================================================================

struct MugUser
{
    Anope::string account;          // NickServ account name (lowercased; map key)
    Anope::string nick;             // current IRC display nick (updated on each command)
    long long coins;
    time_t  last_coins;
    time_t  last_mug;
    time_t  jail_until;
    time_t  last_bet;
    time_t  last_give;
    time_t  last_bounty;
    long long daily_given;
    time_t  daily_reset;
    int     inv[7];

    MugUser()
        : coins(0), last_coins(0), last_mug(0), jail_until(0)
        , last_bet(0), last_give(0), last_bounty(0)
        , daily_given(0), daily_reset(0)
    {
        for (int i = 0; i < NUM_ITEMS; ++i)
            inv[i] = 0;
    }
};

// Sum an int field across all item stacks the user holds.
static int inv_sum(const MugUser &u, int ItemDef::*fld)
{
    int total = 0;
    for (int i = 0; i < NUM_ITEMS; ++i)
        total += (ITEMS[i].*fld) * u.inv[i];
    return total;
}

// ===========================================================================
// Module-level globals
// ===========================================================================

static BotInfo                               *s_bot            = NULL;
static std::set<Anope::string>                s_channels;       // lowercased channel names
static Anope::string                          s_cmd_prefix      = "!";
static std::vector<Anope::string>             s_admin_nicks;
static Anope::string                          s_current_chan;

// nick.lower() -> MugUser
static std::map<Anope::string, MugUser>       s_users;
// nick.lower() -> bounty pool
static std::map<Anope::string, long long>     s_bounties;
// Per-user global command throttle: account.lower() -> last command time
static std::map<Anope::string, time_t>        s_last_cmd;

// Per-channel enable/disable toggles: chan.lower() -> enabled bool
static std::map<Anope::string, bool>          s_channel_toggles;
// God mode set: account.lower() in set = near-guaranteed luck
static std::set<Anope::string>                s_godmode;
// Flood tracking: account.lower() -> list of command timestamps
static std::map<Anope::string, std::vector<time_t> > s_flood_history;
// Flood lockout expiry: account.lower() -> expiry time
static std::map<Anope::string, time_t>        s_flood_lockout;
// Per-user top5/top10 cooldown
static std::map<Anope::string, time_t>        s_top_cd;
// All-time highscore
static Anope::string                          s_highscore_nick;
static long long                              s_highscore_amount = 0;

// Blackjack per-player hand state
struct BJHand
{
    struct Card { Anope::string rank; Anope::string suit; };
    std::vector<Card> hand;
    std::vector<Card> dealer;
    std::vector<Card> deck;
    long long amount;
    Anope::string channel;
    bool doubled;
    BJHand() : amount(0), doubled(false) {}
};
static std::map<Anope::string, BJHand> s_bj_hands;

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
    {
        Log(LOG_NORMAL) << "MugServ: failed to open " << db_path() << " for writing";
        return;
    }

    f << "# MugServ database v1 -- do not edit while service is running\n";

    for (std::set<Anope::string>::const_iterator it = s_channels.begin(); it != s_channels.end(); ++it)
        f << "CHANNEL " << it->c_str() << "\n";

    for (std::map<Anope::string, bool>::const_iterator it = s_channel_toggles.begin(); it != s_channel_toggles.end(); ++it)
        f << "CHANTOGGLE " << it->first.c_str() << " " << (it->second ? "1" : "0") << "\n";

    if (!s_highscore_nick.empty() && s_highscore_amount > 0)
        f << "HIGHSCORE " << s_highscore_nick.c_str() << " " << s_highscore_amount << "\n";

    for (std::map<Anope::string, MugUser>::const_iterator it = s_users.begin(); it != s_users.end(); ++it)
    {
        const MugUser &u = it->second;
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

    for (std::map<Anope::string, long long>::const_iterator it = s_bounties.begin(); it != s_bounties.end(); ++it)
        f << "BOUNTY " << it->first.c_str() << " " << it->second << "\n";
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
                u.inv[i] = std::max(0, std::min(v, 3));
            }
            u.coins       = std::max(0LL, std::min(u.coins, MAX_COINS));
            u.daily_given = std::max(0LL, u.daily_given);
            if (u.last_coins < 0)  u.last_coins = 0;
            if (u.last_mug < 0)    u.last_mug = 0;
            if (u.jail_until < 0)  u.jail_until = 0;
            if (u.last_bet < 0)    u.last_bet = 0;
            if (u.last_give < 0)   u.last_give = 0;
            if (u.last_bounty < 0) u.last_bounty = 0;
            if (u.daily_reset < 0) u.daily_reset = 0;
            if (!u.account.empty())
                s_users[u.account] = u;
        }
        else if (tag == "CHANTOGGLE")
        {
            std::string ch;
            int en = 1;
            ss >> ch >> en;
            if (!ch.empty())
                s_channel_toggles[Anope::string(ch).lower()] = (en != 0);
        }
        else if (tag == "HIGHSCORE")
        {
            std::string hn;
            long long ha = 0;
            ss >> hn >> ha;
            if (!hn.empty() && ha > 0)
            {
                s_highscore_nick   = Anope::string(hn);
                s_highscore_amount = ha;
            }
        }
        else if (tag == "BOUNTY")
        {
            std::string nick;
            long long amt = 0;
            ss >> nick >> amt;
            if (amt > 0)
                s_bounties[Anope::string(nick).lower()] = std::min(amt, MAX_COINS);
        }
    }
}

// ===========================================================================
// Misc helpers
// ===========================================================================

static MugUser* get_user(const Anope::string &nick)
{
    std::map<Anope::string, MugUser>::iterator it = s_users.find(nick.lower());
    return (it != s_users.end()) ? &it->second : NULL;
}

static bool is_admin(CommandSource &src)
{
    if (!src.GetUser())
        return false;
    if (src.GetUser()->HasMode("OPER") || src.GetUser()->IsServicesOper())
        return true;
    if (!src.nc)
        return false;
    Anope::string lacct = src.nc->display.lower();
    for (size_t i = 0; i < s_admin_nicks.size(); ++i)
        if (lacct == s_admin_nicks[i].lower())
            return true;
    return false;
}

// IRC safe message limit (conservative; accounts for nick!user@host prefix).
static const size_t IRC_SAFE_MAX = 400;

static std::vector<Anope::string> split_irc(const Anope::string &text)
{
    std::vector<Anope::string> chunks;
    if (text.length() <= IRC_SAFE_MAX)
    {
        chunks.push_back(text);
        return chunks;
    }

    Anope::string remaining = text;
    while (remaining.length() > IRC_SAFE_MAX)
    {
        Anope::string candidate = remaining.substr(0, IRC_SAFE_MAX);
        size_t cut = candidate.rfind(" | ");
        if (cut == Anope::string::npos || cut < 20)
            cut = candidate.rfind(' ');
        if (cut == Anope::string::npos || cut < 20)
            cut = IRC_SAFE_MAX;

        chunks.push_back(remaining.substr(0, cut));
        remaining = remaining.substr(cut);
        if (remaining.length() >= 3 && remaining.substr(0, 3) == " | ")
            remaining = remaining.substr(3);
        else if (!remaining.empty() && remaining[0] == ' ')
            remaining = remaining.substr(1);
    }
    if (!remaining.empty())
        chunks.push_back(remaining);
    return chunks;
}

// Send a public message using IRCD->SendPrivmsg.
static void announce(CommandSource &src, const Anope::string &msg,
                     const Anope::string &chan = "")
{
    const Anope::string &dest = !chan.empty() ? chan : s_current_chan;
    if (!s_bot) { src.Reply("%s", msg.c_str()); return; }

    std::vector<Anope::string> parts = split_irc(msg);
    if (!dest.empty())
    {
        for (size_t i = 0; i < parts.size(); ++i)
            IRCD->SendPrivmsg(MessageSource(s_bot), dest, "%s", parts[i].c_str());
    }
    else if (!s_channels.empty())
    {
        for (std::set<Anope::string>::const_iterator ci = s_channels.begin(); ci != s_channels.end(); ++ci)
            for (size_t i = 0; i < parts.size(); ++i)
                IRCD->SendPrivmsg(MessageSource(s_bot), *ci, "%s", parts[i].c_str());
    }
    else
    {
        for (size_t i = 0; i < parts.size(); ++i)
            src.Reply("%s", parts[i].c_str());
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
        std::map<Anope::string, MugUser>::iterator it = s_users.find(na->nc->display.lower());
        if (it != s_users.end())
            return &it->second;
    }
    Anope::string tl = target.lower();
    for (std::map<Anope::string, MugUser>::iterator it = s_users.begin(); it != s_users.end(); ++it)
        if (it->second.nick.lower() == tl)
            return &it->second;
    return NULL;
}

static Anope::string get_target_account_key(const Anope::string &target)
{
    NickAlias *na = NickAlias::Find(target);
    if (na && na->nc)
        return na->nc->display.lower();
    return target.lower();
}

// Gate: require NickServ identification; auto-enroll on first use;
// enforce global per-user command throttle.
static bool check_gate(CommandSource &src)
{
    if (!src.nc)
    {
        pm(src, "\002MugServ\002: You must be identified with NickServ to play. "
                "Use: /msg NickServ IDENTIFY <password>");
        return true;
    }

    Anope::string key = src.nc->display.lower();
    time_t now = Anope::CurTime;
    time_t &last = s_last_cmd[key];
    if (now - last < GLOBAL_CMD_CD)
        return true;
    last = now;

    if (s_users.find(key) == s_users.end())
    {
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
        s_users[key].nick = src.GetNick();
    }
    return false;
}

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

// Comparator for sorting MugUser* by coins descending
static bool cmp_coins_desc(MugUser *a, MugUser *b)
{
    return a->coins > b->coins;
}

// Comparator for sorting bounty pairs by amount descending
static bool cmp_bounty_desc(const std::pair<long long, Anope::string> &a,
                            const std::pair<long long, Anope::string> &b)
{
    return a.first > b.first;
}

// ===========================================================================
// Flavour text pools
// ===========================================================================

static std::vector<Anope::string> s_msg_broke;
static std::vector<Anope::string> s_msg_coins_cd;
static std::vector<Anope::string> s_msg_mug_cd;
static std::vector<Anope::string> s_msg_bet_cd;
static std::vector<Anope::string> s_msg_self_mug;
static std::vector<Anope::string> s_msg_mug_success;
static std::vector<Anope::string> s_msg_mug_mega;
static std::vector<Anope::string> s_msg_mug_fail;
static std::vector<Anope::string> s_msg_mug_crit;
static std::vector<Anope::string> s_msg_bounty_place;
static std::vector<Anope::string> s_msg_bounty_claim;

static bool s_msgs_init = false;

static void init_msgs()
{
    if (s_msgs_init) return;
    s_msgs_init = true;

    s_msg_broke.push_back("You're broke. Like, emotionally and financially.");
    s_msg_broke.push_back("Wallet empty. Dreams empty. Use COINS.");
    s_msg_broke.push_back("Not enough coins. The streets are calling: COINS.");
    s_msg_broke.push_back("Your bank account just said 'lol'.");

    s_msg_coins_cd.push_back("The coin gods are buffering... try again in {t}.");
    s_msg_coins_cd.push_back("Your greed is on cooldown. Return in {t}.");
    s_msg_coins_cd.push_back("Banker's on break. Next appointment in {t}.");
    s_msg_coins_cd.push_back("Your dopamine is rate-limited. Next hit in {t}.");

    s_msg_mug_cd.push_back("Lay low... the cops still remember your face. Try again in {t}.");
    s_msg_mug_cd.push_back("Your getaway shoes are untied. Fix them in {t}.");
    s_msg_mug_cd.push_back("CCTV is still tracking you. Hide {t} longer.");
    s_msg_mug_cd.push_back("Your criminal aura is cooling down. {t}.");

    s_msg_bet_cd.push_back("The casino bouncer says 'not yet.' Try again in {t}.");
    s_msg_bet_cd.push_back("Your wallet is begging for mercy. Wait {t}.");
    s_msg_bet_cd.push_back("The slot machine overheated. Cooling for {t}.");
    s_msg_bet_cd.push_back("The crystal ball is foggy. Returns in {t}.");

    s_msg_self_mug.push_back("You can't mug yourself. Therapy is cheaper. (Probably.)");
    s_msg_self_mug.push_back("You stare into the mirror and threaten it. The mirror wins.");
    s_msg_self_mug.push_back("Galaxy brain move: attempted self-mug. Zero coins gained.");
    s_msg_self_mug.push_back("Crime rejected. Victim and attacker are the same idiot.");

    s_msg_mug_success.push_back("{att} jumps {vic} and snatches {steal} coins!");
    s_msg_mug_success.push_back("{att} runs {vic}'s pockets for {steal} coins!");
    s_msg_mug_success.push_back("{att} hits-and-dips {vic} for {steal} coins!");
    s_msg_mug_success.push_back("{att} drains {vic} for {steal} coins!");
    s_msg_mug_success.push_back("{att} yoinks {steal} coins off {vic} like it's casual.");

    s_msg_mug_mega.push_back("MEGA HEIST! {att} pulls a legendary swipe on {vic} for {steal} coins!!");
    s_msg_mug_mega.push_back("ULTRA MUG! {att} hits {vic} with main-character energy: {steal} coins!");
    s_msg_mug_mega.push_back("GOD TIER YOINK! {att} extracts {steal} coins from {vic}'s soul!!");

    s_msg_mug_fail.push_back("{att} slipped mid-mug and dropped {loss} coins across the street!");
    s_msg_mug_fail.push_back("{att} tried to look intimidating but sneezed and dropped {loss} coins!");
    s_msg_mug_fail.push_back("{att} tripped over shoelaces and made it rain {loss} coins!");
    s_msg_mug_fail.push_back("{att} got ambushed by a random cat and scattered {loss} coins!");

    s_msg_mug_crit.push_back("CRITICAL FAIL! {att} faceplants, drops {loss} coins, and gets tossed in jail for {jail}.");
    s_msg_mug_crit.push_back("{att} mugs the air, loses {loss} coins, and the police applauded... then arrested them. Jail: {jail}.");
    s_msg_mug_crit.push_back("{att} left fingerprints on EVERYTHING, lost {loss} coins, and got detained. Jail: {jail}.");

    s_msg_bounty_place.push_back("Bounty placed on {vic} for {amt} coins. Somebody is getting touched.");
    s_msg_bounty_place.push_back("You put {amt} coins on {vic}'s head. That's... oddly motivational.");
    s_msg_bounty_place.push_back("{vic} is now WANTED for {amt} coins.");

    s_msg_bounty_claim.push_back("BOUNTY CLAIMED! {att} collects {bounty} bonus coins for mugging {vic}!");
    s_msg_bounty_claim.push_back("Payday! {att} cashes in a {bounty}-coin bounty on {vic}.");
}

// Replace {key} tokens in a template string.
static Anope::string tpl(const Anope::string &tmpl,
                          const std::vector<std::pair<Anope::string, Anope::string> > &vars)
{
    Anope::string out = tmpl;
    for (size_t vi = 0; vi < vars.size(); ++vi)
    {
        Anope::string tok = "{" + vars[vi].first + "}";
        size_t pos = 0;
        while ((pos = out.find(tok, pos)) != Anope::string::npos)
        {
            out = out.substr(0, pos) + vars[vi].second + out.substr(pos + tok.length());
            pos += vars[vi].second.length();
        }
    }
    return out;
}

// Helper to build a vars vector (up to 3 pairs)
static std::vector<std::pair<Anope::string, Anope::string> > mkv1(
    const Anope::string &k1, const Anope::string &v1)
{
    std::vector<std::pair<Anope::string, Anope::string> > v;
    v.push_back(std::make_pair(k1, v1));
    return v;
}

static std::vector<std::pair<Anope::string, Anope::string> > mkv2(
    const Anope::string &k1, const Anope::string &v1,
    const Anope::string &k2, const Anope::string &v2)
{
    std::vector<std::pair<Anope::string, Anope::string> > v;
    v.push_back(std::make_pair(k1, v1));
    v.push_back(std::make_pair(k2, v2));
    return v;
}

static std::vector<std::pair<Anope::string, Anope::string> > mkv3(
    const Anope::string &k1, const Anope::string &v1,
    const Anope::string &k2, const Anope::string &v2,
    const Anope::string &k3, const Anope::string &v3)
{
    std::vector<std::pair<Anope::string, Anope::string> > v;
    v.push_back(std::make_pair(k1, v1));
    v.push_back(std::make_pair(k2, v2));
    v.push_back(std::make_pair(k3, v3));
    return v;
}

// min of 3 long-longs
static long long min3(long long a, long long b, long long c)
{
    long long m = a;
    if (b < m) m = b;
    if (c < m) m = c;
    return m;
}

// ===========================================================================
// Toggle / godmode / flood helpers
// ===========================================================================

// Is the game enabled for a channel? Missing key = enabled (legacy compat).
// Pass "" for PM-only context (always allowed).
static bool _plugin_enabled(const Anope::string &chan)
{
    if (chan.empty()) return true;
    std::map<Anope::string, bool>::const_iterator it = s_channel_toggles.find(chan.lower());
    if (it == s_channel_toggles.end()) return true; // default: enabled
    return it->second;
}

static void _set_channel_enabled(const Anope::string &chan, bool on)
{
    s_channel_toggles[chan.lower()] = on;
    save_db();
}

static bool _has_godmode(const Anope::string &acct)
{
    return s_godmode.count(acct.lower()) > 0;
}

// Returns true if blocked (flood lockout active or just triggered).
static bool _flood_check(CommandSource &src)
{
    if (!src.nc) return false;
    Anope::string key = src.nc->display.lower();
    if (is_admin(src)) return false;
    time_t now = Anope::CurTime;
    // Already locked out?
    std::map<Anope::string, time_t>::iterator li = s_flood_lockout.find(key);
    if (li != s_flood_lockout.end() && now < li->second)
    {
        int rem = static_cast<int>(li->second - now);
        pm(src, "You are on a 30-minute lockout for spamming. " + fmt_dur(rem) + " remaining.");
        return true;
    }
    // Record this command
    std::vector<time_t> &hist = s_flood_history[key];
    hist.push_back(now);
    // Prune old entries outside window
    time_t cutoff = now - static_cast<time_t>(FLOOD_WINDOW);
    std::vector<time_t> fresh;
    for (size_t i = 0; i < hist.size(); ++i)
        if (hist[i] > cutoff) fresh.push_back(hist[i]);
    hist = fresh;
    if (static_cast<int>(hist.size()) > FLOOD_MAX_CMDS)
    {
        s_flood_lockout[key] = now + static_cast<time_t>(FLOOD_LOCKOUT_DUR);
        hist.clear();
        pm(src, "Slow down! You have been locked out of all casino commands for 30 minutes.");
        return true;
    }
    return false;
}

static void _flood_clear(const Anope::string &acct)
{
    Anope::string key = acct.lower();
    s_flood_lockout.erase(key);
    s_flood_history.erase(key);
}

// Update all-time highscore after any balance change.
static void _update_highscore(const MugUser &u)
{
    if (u.coins > s_highscore_amount)
    {
        s_highscore_amount = u.coins;
        s_highscore_nick   = u.nick;
    }
}

// ===========================================================================
// Forward declaration (timer needs module ptr)
// ===========================================================================
class ModuleMugServ;
static ModuleMugServ *s_module = NULL;

// ===========================================================================
// Auto-save timer
// ===========================================================================
class MugSaveTimer : public Timer
{
public:
    MugSaveTimer(Module *mod) : Timer(mod, 300, Anope::CurTime, true) {}
    void Tick(time_t) anope_override { save_db(); }
};

// ===========================================================================
// Commands
// ===========================================================================

// ---- COINS ----
struct CommandMugCoins : Command
{
    CommandMugCoins(Module *c) : Command(c, "mugserv/COINS", 0, 0)
    {
        SetDesc("Collect your coins (10-min cooldown)");
    }

    void Execute(CommandSource &src, const std::vector<Anope::string> &) anope_override
    {
        if (check_gate(src)) return;
        MugUser &u = s_users[src.nc->display.lower()];

        int rem = cd_rem(u.last_coins, CD_COINS);
        if (rem > 0)
        {
            pm(src, tpl(rand_pick_str(s_msg_coins_cd), mkv1("t", fmt_dur(rem))));
            return;
        }

        long long cur = std::max(0LL, u.coins);
        int base = ri(COINS_MIN, COINS_MAX);
        long long scale = 0;
        if (cur > 0)
        {
            int pct = ri(COINS_SCALE_MIN, COINS_SCALE_MAX);
            scale = std::min(static_cast<long long>(cur * pct / 100), COINS_SCALE_CAP);
        }
        long long flat = std::min(50LL, static_cast<long long>(inv_sum(u, &ItemDef::coins_flat)));
        long long gain = std::max(1LL, static_cast<long long>(base) + scale + flat);

        u.coins      += gain;
        u.last_coins  = Anope::CurTime;

        Anope::string msg = "\002" + u.nick + "\002 found \002" + fmt_coins(gain)
                          + "\002 coins! Balance: \002" + fmt_coins(u.coins) + "\002 coins.";
        announce(src, msg);
    }
};

// ---- BALANCE ----
struct CommandMugBalance : Command
{
    CommandMugBalance(Module *c) : Command(c, "mugserv/BALANCE", 0, 1)
    {
        SetDesc("Check your (or another player's) coin balance");
        SetSyntax("[nick]");
    }

    void Execute(CommandSource &src, const std::vector<Anope::string> &params) anope_override
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
        pm(src, "\002" + u->nick + "\002 has \002" + fmt_coins(u->coins) + "\002 coins.");
    }
};

// ---- GIVE ----
struct CommandMugGive : Command
{
    CommandMugGive(Module *c) : Command(c, "mugserv/GIVE", 2, 2)
    {
        SetDesc("Give coins to another registered player");
        SetSyntax("<nick> <amount>");
    }

    void Execute(CommandSource &src, const std::vector<Anope::string> &params) anope_override
    {
        if (check_gate(src)) return;
        MugUser &giver = s_users[src.nc->display.lower()];

        Anope::string target = params[0];
        if (get_target_account_key(target) == src.nc->display.lower())
        {
            pm(src, "You can't give yourself coins. That's called having coins.");
            return;
        }

        long long amt = parse_ll(params[1]);
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

        time_t now = Anope::CurTime;
        if (now - giver.daily_reset > GIVE_DAY_SECS)
        {
            giver.daily_given = 0;
            giver.daily_reset = now;
        }
        if (giver.daily_given + amt > GIVE_DAILY_LIMIT)
        {
            long long remaining = std::max(0LL, GIVE_DAILY_LIMIT - giver.daily_given);
            pm(src, "Daily give limit reached. You may give " + fmt_coins(remaining)
                    + " more coins today.");
            return;
        }

        if (giver.coins < amt)
        {
            pm(src, rand_pick_str(s_msg_broke));
            return;
        }

        giver.coins      -= amt;
        recv->coins      += amt;
        giver.daily_given += amt;
        giver.last_give   = now;

        Anope::string msg = "\002" + giver.nick + "\002 gave \002" + fmt_coins(amt)
                          + "\002 coins to \002" + recv->nick + "\002!";
        announce(src, msg);
    }
};

// ---- JAIL ----
struct CommandMugJail : Command
{
    CommandMugJail(Module *c) : Command(c, "mugserv/JAIL", 0, 0)
    {
        SetDesc("Check your jail status");
    }

    void Execute(CommandSource &src, const std::vector<Anope::string> &) anope_override
    {
        if (check_gate(src)) return;
        MugUser &u = s_users[src.nc->display.lower()];
        time_t now = Anope::CurTime;

        if (u.jail_until > now)
        {
            int rem = static_cast<int>(u.jail_until - now);
            pm(src, "\002" + u.nick + "\002 is doing time! Free in " + fmt_dur(rem) + ".");
        }
        else
        {
            pm(src, "\002" + u.nick + "\002 is a free criminal once again.");
        }
    }
};

// ---- MUG / ROB ----
struct CommandMugMug : Command
{
    CommandMugMug(Module *c) : Command(c, "mugserv/MUG", 1, 1)
    {
        SetDesc("Attempt to mug a registered player");
        SetSyntax("<nick>");
    }

    void Execute(CommandSource &src, const std::vector<Anope::string> &params) anope_override
    {
        if (check_gate(src)) return;

        Anope::string att_nick = src.GetNick();
        Anope::string vic_nick = params[0];

        if (get_target_account_key(vic_nick) == src.nc->display.lower())
        {
            pm(src, rand_pick_str(s_msg_self_mug));
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

        if (att.jail_until > now)
        {
            int rem = static_cast<int>(att.jail_until - now);
            pm(src, "You're still in jail for " + fmt_dur(rem) + ". No crimes for now!");
            return;
        }

        int mug_rem = cd_rem(att.last_mug, CD_MUG);
        if (mug_rem > 0)
        {
            pm(src, tpl(rand_pick_str(s_msg_mug_cd), mkv1("t", fmt_dur(mug_rem))));
            return;
        }

        if (att.coins < MUG_FEE)
        {
            pm(src, rand_pick_str(s_msg_broke));
            return;
        }

        att.coins -= MUG_FEE;

        // Ultra-rare instant jail
        if (ri(1, 100) <= OOPS_JAIL_CHANCE)
        {
            long long loss = std::min(att.coins, MAX_CRIT_LOSS);
            att.coins      = std::max(0LL, att.coins - loss);
            vic->coins    += loss;
            att.jail_until = now + CD_JAIL;
            att.last_mug   = now;

            Anope::string msg = tpl(rand_pick_str(s_msg_mug_crit),
                mkv3("att", att.nick, "loss", fmt_coins(loss), "jail", fmt_dur(CD_JAIL)))
                + " [OOPS-JAIL -- you looked suspicious!] | "
                + att.nick + ": " + fmt_coins(att.coins)
                + " | " + vic->nick + ": " + fmt_coins(vic->coins);
            announce(src, msg);
            return;
        }

        int success_bonus = std::min(35, inv_sum(att, &ItemDef::mug_success_bonus));
        int eff_success   = std::min(95, SUCCESS_CHANCE + success_bonus);
        int roll          = ri(1, 100);

        // SUCCESS
        if (roll <= eff_success)
        {
            // Banana trap
            int banana_count = vic->inv[ITEM_BANANA];
            if (banana_count > 0)
            {
                int slip = std::min(banana_count * BANANA_SLIP_PCT, BANANA_SLIP_MAX);
                if (ri(1, 100) <= slip)
                {
                    do_crit_fail(src, att, vic, now, " [BANANA TRAP fired!]");
                    return;
                }
            }

            // Cloak dodge
            int immune = std::min(50, inv_sum(*vic, &ItemDef::immune_chance));
            if (immune > 0 && ri(1, 100) <= immune)
            {
                att.last_mug = now;
                Anope::string msg = "\002" + att.nick + "\002 tries to mug \002"
                    + vic->nick + "\002 but they vanish into the shadows. Nothing stolen!"
                    + " | " + att.nick + ": " + fmt_coins(att.coins)
                    + " | " + vic->nick + ": " + fmt_coins(vic->coins);
                announce(src, msg);
                return;
            }

            if (vic->coins <= 0)
            {
                att.last_mug = now;
                Anope::string msg = "\002" + att.nick + "\002 tried to mug \002"
                    + vic->nick + "\002 but they're flat broke. Nothing to steal!"
                    + " | " + att.nick + ": " + fmt_coins(att.coins)
                    + " | " + vic->nick + ": " + fmt_coins(vic->coins);
                announce(src, msg);
                return;
            }

            int steal_pct = ri(STEAL_MIN, STEAL_MAX)
                          + std::min(30, inv_sum(att, &ItemDef::steal_bonus));
            bool mega = (ri(1, 100) <= MEGA_STEAL_CHANCE);
            if (mega) steal_pct += MEGA_STEAL_BONUS;

            long long capped_bal = std::min(vic->coins, MAX_COINS);
            long long steal_raw = std::max(1LL, capped_bal * steal_pct / 100);

            bool whale_capped = false;
            if (vic->coins > RICH_THRESHOLD)
            {
                long long rich_cap = std::max(1LL, capped_bal * RICH_MAX_STEAL / 100);
                if (steal_raw > rich_cap)
                {
                    steal_raw   = rich_cap;
                    whale_capped = true;
                }
            }

            int reduction = std::min(60, inv_sum(*vic, &ItemDef::steal_reduction));
            long long steal = (reduction > 0)
                ? std::max(1LL, steal_raw * (100 - reduction) / 100)
                : steal_raw;

            vic->coins  -= steal;
            att.coins   += steal;
            att.last_mug = now;

            // Bounty claim
            long long bounty_claim = 0;
            Anope::string bkey = get_target_account_key(vic_nick);
            std::map<Anope::string, long long>::iterator bit = s_bounties.find(bkey);
            if (bit != s_bounties.end())
            {
                bounty_claim = bit->second;
                att.coins   += bounty_claim;
                s_bounties.erase(bit);
            }

            Anope::string base_msg = mega
                ? tpl(rand_pick_str(s_msg_mug_mega),
                       mkv3("att", att.nick, "vic", vic->nick, "steal", fmt_coins(steal)))
                : tpl(rand_pick_str(s_msg_mug_success),
                       mkv3("att", att.nick, "vic", vic->nick, "steal", fmt_coins(steal)));

            Anope::string whale_tag = whale_capped ? " [whale-capped]" : "";
            Anope::string bounty_tag = "";
            if (bounty_claim > 0)
                bounty_tag = " " + tpl(rand_pick_str(s_msg_bounty_claim),
                             mkv3("att", att.nick, "vic", vic->nick, "bounty", fmt_coins(bounty_claim)));

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
            long long loss = min3(att.coins * pct / 100, att.coins, MAX_FAIL_LOSS);
            loss = std::max(loss, 0LL);
            att.coins   -= loss;
            vic->coins  += loss;
            att.last_mug = now + CD_MUG_FAIL_XTRA;

            Anope::string msg = tpl(rand_pick_str(s_msg_mug_fail),
                mkv2("att", att.nick, "loss", fmt_coins(loss)))
                + " | " + att.nick + ": " + fmt_coins(att.coins)
                + " | " + vic->nick + ": " + fmt_coins(vic->coins);
            announce(src, msg);
            return;
        }

        // CRIT FAIL
        do_crit_fail(src, att, vic, now, "");
    }

private:
    void do_crit_fail(CommandSource &src, MugUser &att, MugUser *vic, time_t now,
                      const Anope::string &extra)
    {
        long long cur = att.coins;
        int pct = ri(CRIT_LOSS_MIN, CRIT_LOSS_MAX);
        long long loss = min3(cur * pct / 100, cur, MAX_CRIT_LOSS);
        loss = std::max(loss, 0LL);

        att.coins    -= loss;
        vic->coins   += loss;
        att.last_mug  = now;

        if (att.inv[ITEM_BAIL] > 0)
        {
            att.inv[ITEM_BAIL]--;
            att.jail_until = 0;
            Anope::string msg = tpl(rand_pick_str(s_msg_mug_crit),
                mkv3("att", att.nick, "loss", fmt_coins(loss), "jail", fmt_dur(CD_JAIL)))
                + extra
                + " BUT \002" + att.nick + "\002 had a Bail Bondsman and got out instantly!"
                + " (Mug cooldown still active.)"
                + " | " + att.nick + ": " + fmt_coins(att.coins)
                + " | " + vic->nick + ": " + fmt_coins(vic->coins);
            announce(src, msg);
        }
        else
        {
            att.jail_until = now + CD_JAIL;
            Anope::string msg = tpl(rand_pick_str(s_msg_mug_crit),
                mkv3("att", att.nick, "loss", fmt_coins(loss), "jail", fmt_dur(CD_JAIL)))
                + extra
                + " | " + att.nick + ": " + fmt_coins(att.coins)
                + " | " + vic->nick + ": " + fmt_coins(vic->coins);
            announce(src, msg);
        }
    }
};

// ---- BET ----
struct CommandMugBet : Command
{
    CommandMugBet(Module *c) : Command(c, "mugserv/BET", 1, 1)
    {
        SetDesc("Gamble coins");
        SetSyntax("<amount>");
    }

    void Execute(CommandSource &src, const std::vector<Anope::string> &params) anope_override
    {
        if (check_gate(src)) return;
        MugUser &u = s_users[src.nc->display.lower()];

        int rem = cd_rem(u.last_bet, CD_BET);
        if (rem > 0)
        {
            pm(src, tpl(rand_pick_str(s_msg_bet_cd), mkv1("t", fmt_dur(rem))));
            return;
        }

        long long amt = parse_ll(params[0]);
        if (amt < BET_MIN)
        {
            pm(src, "Minimum bet is " + fmt_coins(BET_MIN) + " coin.");
            return;
        }
        if (u.coins < amt)
        {
            pm(src, rand_pick_str(s_msg_broke));
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
            long long payout = amt * 2;
            u.coins += payout;
            msg = "\002" + u.nick + "\002 bets " + fmt_coins(amt)
                + " and \002WINS\002! Payout: " + fmt_coins(payout)
                + ". Balance: \002" + fmt_coins(u.coins) + "\002";
        }
        else
        {
            msg = "\002" + u.nick + "\002 bets " + fmt_coins(amt)
                + " and loses it all! Balance: \002" + fmt_coins(u.coins) + "\002";
        }

        announce(src, msg);
    }
};

// ---- BOUNTY ----
struct CommandMugBounty : Command
{
    CommandMugBounty(Module *c) : Command(c, "mugserv/BOUNTY", 2, 2)
    {
        SetDesc("Place a coin bounty on another registered player");
        SetSyntax("<nick> <amount>");
    }

    void Execute(CommandSource &src, const std::vector<Anope::string> &params) anope_override
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

        long long amt = parse_ll(params[1]);
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
            pm(src, rand_pick_str(s_msg_broke));
            return;
        }

        placer.coins -= amt;
        placer.last_bounty = Anope::CurTime;
        Anope::string vic_key = get_target_account_key(target);
        if (s_bounties.find(vic_key) != s_bounties.end())
            s_bounties[vic_key] = s_bounties[vic_key] + amt;
        else
            s_bounties[vic_key] = amt;

        Anope::string msg = tpl(rand_pick_str(s_msg_bounty_place),
            mkv2("vic", vic->nick, "amt", fmt_coins(amt)))
            + " (" + placer.nick + " now has " + fmt_coins(placer.coins) + " coins)";
        announce(src, msg);
    }
};

// ---- BOUNTIES ----
struct CommandMugBounties : Command
{
    CommandMugBounties(Module *c) : Command(c, "mugserv/BOUNTIES", 0, 0)
    {
        SetDesc("List the top active bounties");
    }

    void Execute(CommandSource &src, const std::vector<Anope::string> &) anope_override
    {
        if (s_bounties.empty())
        {
            pm(src, "No active bounties. Everyone is (unfortunately) safe.");
            return;
        }

        std::vector<std::pair<long long, Anope::string> > list;
        for (std::map<Anope::string, long long>::const_iterator it = s_bounties.begin();
             it != s_bounties.end(); ++it)
            list.push_back(std::make_pair(it->second, it->first));
        std::sort(list.begin(), list.end(), cmp_bounty_desc);

        Anope::string out = "Top bounties:";
        int shown = 0;
        for (size_t i = 0; i < list.size() && shown < 10; ++i, ++shown)
        {
            MugUser *u = get_user(list[i].second);
            Anope::string dn = u ? u->nick : list[i].second;
            out += " | " + dn + "(" + fmt_coins(list[i].first) + ")";
        }
        pm(src, out);
    }
};

// ---- SHOP ----
struct CommandMugShop : Command
{
    CommandMugShop(Module *c) : Command(c, "mugserv/SHOP", 0, 0)
    {
        SetDesc("View available items in the crime shop");
    }

    void Execute(CommandSource &src, const std::vector<Anope::string> &) anope_override
    {
        pm(src, "\002MugServ Crime Shop\002 -- use \002BUY <key>\002 to purchase:");
        pm(src, "-----------------------------------------------------");
        for (int i = 0; i < NUM_ITEMS; ++i)
        {
            pm(src, "  \002" + Anope::string(ITEMS[i].key) + "\002 -> "
                + Anope::string(ITEMS[i].name)
                + " (" + fmt_coins(ITEMS[i].price) + " coins)"
                + " -- " + Anope::string(ITEMS[i].desc)
                + " [max 3 per player]");
        }
        pm(src, "-----------------------------------------------------");
        pm(src, "Passive items (mask/knucks/luckycoin/vest/cloak/banana) work automatically.");
        pm(src, "Bail is a consumable: freed from jail instantly when you buy/use it.");
    }
};

// ---- BUY ----
struct CommandMugBuy : Command
{
    CommandMugBuy(Module *c) : Command(c, "mugserv/BUY", 1, 1)
    {
        SetDesc("Purchase an item from the shop");
        SetSyntax("<item-key>");
    }

    void Execute(CommandSource &src, const std::vector<Anope::string> &params) anope_override
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

        if (item.is_bail && u.jail_until > Anope::CurTime)
        {
            u.inv[idx]--;
            u.jail_until = 0;
            pm(src, "\002" + u.nick + "\002 bought \002" + Anope::string(item.name)
                    + "\002 for " + fmt_coins(item.price)
                    + " coins and got bailed out! Welcome back to freedom. "
                    "(Mug cooldown still active.) Balance: " + fmt_coins(u.coins));
        }
        else
        {
            pm(src, "Bought \002" + Anope::string(item.name) + "\002 for "
                    + fmt_coins(item.price) + " coins. Balance: " + fmt_coins(u.coins));
        }
    }
};

// ---- INV ----
struct CommandMugInv : Command
{
    CommandMugInv(Module *c) : Command(c, "mugserv/INV", 0, 0)
    {
        SetDesc("View your item inventory");
    }

    void Execute(CommandSource &src, const std::vector<Anope::string> &) anope_override
    {
        if (check_gate(src)) return;
        MugUser &u = s_users[src.nc->display.lower()];

        bool has_any = false;
        for (int i = 0; i < NUM_ITEMS; ++i)
            if (u.inv[i] > 0) { has_any = true; break; }

        if (!has_any)
        {
            pm(src, "Your inventory is empty. Send SHOP to browse items.");
            return;
        }

        pm(src, "\002" + u.nick + "'s inventory\002:");
        for (int i = 0; i < NUM_ITEMS; ++i)
        {
            if (u.inv[i] <= 0) continue;
            pm(src, "  \002" + Anope::string(ITEMS[i].key) + "\002 x"
                    + stringify(u.inv[i])
                    + " -- " + Anope::string(ITEMS[i].name)
                    + " (" + Anope::string(ITEMS[i].desc) + ")");
        }
    }
};

// ---- USE ----
struct CommandMugUse : Command
{
    CommandMugUse(Module *c) : Command(c, "mugserv/USE", 1, 1)
    {
        SetDesc("Use a consumable item (e.g., bail)");
        SetSyntax("<item-key>");
    }

    void Execute(CommandSource &src, const std::vector<Anope::string> &params) anope_override
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
                    + "\002 is a passive item -- it works automatically from your inventory. "
                    "No need to use it manually.");
            return;
        }

        if (u.jail_until > Anope::CurTime)
        {
            u.inv[idx]--;
            u.jail_until = 0;
            pm(src, "\002" + u.nick + "\002 used \002Bail Bondsman\002 and is FREE! "
                    "(Mug cooldown still active.) Stay out of trouble.");
        }
        else
        {
            pm(src, "You're not in jail. Your \002Bail Bondsman\002 stays in your pocket "
                    "for when you actually need it.");
        }
    }
};

// ---- TOP5 ----
struct CommandMugTop5 : Command
{
    CommandMugTop5(Module *c) : Command(c, "mugserv/TOP5", 0, 0)
    {
        SetDesc("Show the top 5 richest players");
    }

    void Execute(CommandSource &src, const std::vector<Anope::string> &) anope_override
    {
        std::vector<MugUser*> sorted;
        for (std::map<Anope::string, MugUser>::iterator it = s_users.begin(); it != s_users.end(); ++it)
            sorted.push_back(&it->second);
        std::sort(sorted.begin(), sorted.end(), cmp_coins_desc);
        if (sorted.size() > 5) sorted.resize(5);

        if (sorted.empty())
        {
            pm(src, "No coin data yet. Register and use COINS to get started!");
            return;
        }
        pm(src, "Top 5: " + format_lb(sorted, 1));
    }
};

// ---- TOP10 ----
struct CommandMugTop10 : Command
{
    CommandMugTop10(Module *c) : Command(c, "mugserv/TOP10", 0, 0)
    {
        SetDesc("Show the top 10 richest players");
    }

    void Execute(CommandSource &src, const std::vector<Anope::string> &) anope_override
    {
        std::vector<MugUser*> sorted;
        for (std::map<Anope::string, MugUser>::iterator it = s_users.begin(); it != s_users.end(); ++it)
            sorted.push_back(&it->second);
        std::sort(sorted.begin(), sorted.end(), cmp_coins_desc);
        if (sorted.size() > 10) sorted.resize(10);

        if (sorted.empty())
        {
            pm(src, "No coin data yet. Register and use COINS to get started!");
            return;
        }

        size_t half = sorted.size() > 5 ? 5 : sorted.size();
        std::vector<MugUser*> first(sorted.begin(), sorted.begin() + half);
        std::vector<MugUser*> second(sorted.begin() + half, sorted.end());

        pm(src, "Top 10 (1-5):   " + format_lb(first, 1));
        if (!second.empty())
            pm(src, "Top 10 (6-10):  " + format_lb(second, 6));
    }
};

// ---- Admin: MUGADD ----
struct CommandMugAdd : Command
{
    CommandMugAdd(Module *c) : Command(c, "mugserv/MUGADD", 2, 2)
    {
        SetDesc("[Admin] Add coins to a player");
        SetSyntax("<nick> <amount>");
    }

    void Execute(CommandSource &src, const std::vector<Anope::string> &params) anope_override
    {
        if (!is_admin(src))
        {
            pm(src, "Access denied.");
            return;
        }
        MugUser *u = get_user_by_target(params[0]);
        if (!u)
        {
            pm(src, params[0] + " is not registered with MugServ.");
            return;
        }
        long long amt = parse_ll(params[1]);
        if (amt <= 0) { pm(src, "Amount must be > 0."); return; }
        u->coins += amt;
        pm(src, "Added " + fmt_coins(amt) + " coins to \002" + u->nick
                + "\002. New balance: " + fmt_coins(u->coins));
    }
};

// ---- Admin: MUGSET ----
struct CommandMugSet : Command
{
    CommandMugSet(Module *c) : Command(c, "mugserv/MUGSET", 2, 2)
    {
        SetDesc("[Admin] Set a player's coin balance");
        SetSyntax("<nick> <amount>");
    }

    void Execute(CommandSource &src, const std::vector<Anope::string> &params) anope_override
    {
        if (!is_admin(src)) { pm(src, "Access denied."); return; }
        MugUser *u = get_user_by_target(params[0]);
        if (!u) { pm(src, params[0] + " is not registered."); return; }
        long long amt = parse_ll(params[1]);
        if (amt < 0) { pm(src, "Amount must be >= 0."); return; }
        u->coins = amt;
        pm(src, "Set \002" + u->nick + "\002's balance to " + fmt_coins(amt));
    }
};

// ---- Admin: MUGTAKE ----
struct CommandMugTake : Command
{
    CommandMugTake(Module *c) : Command(c, "mugserv/MUGTAKE", 2, 2)
    {
        SetDesc("[Admin] Remove coins from a player");
        SetSyntax("<nick> <amount>");
    }

    void Execute(CommandSource &src, const std::vector<Anope::string> &params) anope_override
    {
        if (!is_admin(src)) { pm(src, "Access denied."); return; }
        MugUser *u = get_user_by_target(params[0]);
        if (!u) { pm(src, params[0] + " is not registered."); return; }
        long long amt = parse_ll(params[1]);
        if (amt <= 0) { pm(src, "Amount must be > 0."); return; }
        u->coins = std::max(0LL, u->coins - amt);
        pm(src, "Took " + fmt_coins(amt) + " coins from \002" + u->nick
                + "\002. New balance: " + fmt_coins(u->coins));
    }
};

// ---- Admin: MUGRESET ----
struct CommandMugReset : Command
{
    CommandMugReset(Module *c) : Command(c, "mugserv/MUGRESET", 0, 1)
    {
        SetDesc("[Admin] Reset all player data to zero (DESTRUCTIVE)");
        SetSyntax("[confirm]");
    }

    void Execute(CommandSource &src, const std::vector<Anope::string> &params) anope_override
    {
        if (!is_admin(src)) { pm(src, "Access denied."); return; }

        if (params.empty() || params[0].lower() != "confirm")
        {
            pm(src, "This resets ALL player balances, cooldowns, inventories, and bounties.");
            pm(src, "Send \002MUGRESET confirm\002 to proceed.");
            return;
        }

        for (std::map<Anope::string, MugUser>::iterator it = s_users.begin(); it != s_users.end(); ++it)
        {
            MugUser &u = it->second;
            u.coins = 0; u.last_coins = 0; u.last_mug = 0; u.jail_until = 0;
            u.last_bet = 0; u.last_give = 0; u.last_bounty = 0;
            u.daily_given = 0; u.daily_reset = 0;
            for (int i = 0; i < NUM_ITEMS; ++i) u.inv[i] = 0;
        }
        s_bounties.clear();
        save_db();
        pm(src, "FULL RESET DONE. Everyone is broke again. Society restored.");
    }
};

// ---- Admin: MUGSTATS ----
struct CommandMugStats : Command
{
    CommandMugStats(Module *c) : Command(c, "mugserv/MUGSTATS", 0, 0)
    {
        SetDesc("[Admin] Economy overview statistics");
    }

    void Execute(CommandSource &src, const std::vector<Anope::string> &) anope_override
    {
        if (!is_admin(src)) { pm(src, "Access denied."); return; }

        long long total = 0;
        for (std::map<Anope::string, MugUser>::const_iterator it = s_users.begin(); it != s_users.end(); ++it)
            total += it->second.coins;

        std::vector<MugUser*> sorted;
        for (std::map<Anope::string, MugUser>::iterator it = s_users.begin(); it != s_users.end(); ++it)
            sorted.push_back(&it->second);
        std::sort(sorted.begin(), sorted.end(), cmp_coins_desc);
        if (sorted.size() > 5) sorted.resize(5);

        pm(src, "\002MugServ Economy Stats\002");
        pm(src, "  Players in DB: \002" + stringify(static_cast<int>(s_users.size())) + "\002");
        pm(src, "  Active bounties: \002" + stringify(static_cast<int>(s_bounties.size())) + "\002");
        pm(src, "  Total coins in economy: \002" + fmt_coins(total) + "\002");
        if (!sorted.empty())
            pm(src, "  Top 5: " + format_lb(sorted, 1));

        if (!s_channels.empty())
        {
            Anope::string chlist;
            for (std::set<Anope::string>::const_iterator ci = s_channels.begin(); ci != s_channels.end(); ++ci)
            {
                if (!chlist.empty()) chlist += " ";
                chlist += *ci;
            }
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

// ---- HELP ----
struct CommandMugHelp : Command
{
    CommandMugHelp(Module *c) : Command(c, "mugserv/HELP", 0, 1)
    {
        SetDesc("Show MugServ command help");
        SetSyntax("[command]");
    }

    void Execute(CommandSource &src, const std::vector<Anope::string> &) anope_override
    {
        pm(src, "\002MugServ Help\002 -- /msg MugServ <COMMAND>  or  " + s_cmd_prefix + "command in an active channel");
        pm(src, "");
        pm(src, "\002Access\002");
        pm(src, "  A NickServ account is required. Identify first: /msg NickServ IDENTIFY <pass>");
        pm(src, "  You are automatically enrolled the first time you send any command.");
        pm(src, "  In channels, prefix commands with \002" + s_cmd_prefix + "\002 (e.g. \002" + s_cmd_prefix + "coins\002).");
        pm(src, "");
        pm(src, "\002Economy\002");
        pm(src, "  COINS                 -- Collect coins (10-min cooldown, scales with wealth)");
        pm(src, "  BALANCE [nick]        -- Check your (or another player's) balance");
        pm(src, "  GIVE <nick> <amount>  -- Give coins to another player (5-min CD, daily cap)");
        pm(src, "");
        pm(src, "\002Bounties\002");
        pm(src, "  BOUNTY <nick> <amt>   -- Place a coin bounty on someone (costs you coins)");
        pm(src, "  BOUNTIES              -- List top active bounties");
        pm(src, "                          Mugger who hits a bounty target claims the pool.");
        pm(src, "");
        pm(src, "\002Mugging\002");
        pm(src, "  MUG <nick>            -- Attempt to mug a registered player");
        pm(src, "  ROB <nick>            -- Alias for MUG");
        pm(src, "    60% base success: steal 10-30% of victim's coins");
        pm(src, "    25% normal fail:  you drop coins (+ extra cooldown)");
        pm(src, "    15% crit fail:    you lose big + jail (no mugs until free)");
        pm(src, "    Rare: mega heists, oops-jail, whale protection at >10k coins");
        pm(src, "  JAIL                  -- Check your current jail status");
        pm(src, "");
        pm(src, "\002Gambling\002");
        pm(src, "  BET <amount>          -- 40% chance to double your bet (Lucky Coin improves odds)");
        pm(src, "  ROLL <amt> [type]     -- Dice casino. Types: high(2x) lucky7(4x) snake(30x) field(3x) hardway(8x) yolo(15x)");
        pm(src, "  PENNY                 -- Penny slot: 1 coin per pull, win up to 5,000! (15s CD)");
        pm(src, "  DOLLAR                -- Dollar slot: 100 coins per pull, win up to 50,000! (30s CD)");
        pm(src, "  ROULETTE <amt> <bet>  -- Roulette. Bets: red/black/odd/even/high/low (2x), 1st/2nd/3rd (3x), 0-36 (36x)");
        pm(src, "  BJ <amount>           -- Blackjack vs dealer. Then: HIT, STAND, DD (double down). BJ pays 2.5x");
        pm(src, "  HOLDEM <amount>       -- Texas Hold'em vs dealer. Pair 2x ... Royal Flush 50x");
        pm(src, "  Spam guard: 15+ commands in 60s = 30-min lockout");
        pm(src, "");
        pm(src, "\002Shop & Inventory\002");
        pm(src, "  SHOP                  -- Browse available items");
        pm(src, "  BUY <key>             -- Buy an item (max 3 of each)");
        pm(src, "  INV                   -- View your inventory");
        pm(src, "  USE <key>             -- Use a consumable item (e.g., bail)");
        pm(src, "");
        pm(src, "\002Items\002  (passive bonuses stack up to 3x)");
        pm(src, "  mask      120c  -- +7% mug success per stack");
        pm(src, "  knucks    250c  -- +6% steal per stack on success");
        pm(src, "  luckycoin 180c  -- +3 flat COINS bonus; +7% BET win chance per stack");
        pm(src, "  vest      220c  -- -20% stolen per stack (victim)");
        pm(src, "  cloak     500c  -- 15% dodge chance per stack (victim)");
        pm(src, "  banana     50c  -- 5% mugger-slip chance per stack (victim, triggers crit fail)");
        pm(src, "  bail     5000c  -- consumable: instantly frees you from jail once");
        pm(src, "");
        pm(src, "\002Leaderboards\002");
        pm(src, "  TOP5                  -- Top 5 richest players");
        pm(src, "  TOP10                 -- Top 10 richest players");
        pm(src, "");
        pm(src, "\002Admin (IRCops and configured admin_nicks only)\002");
        pm(src, "  MUGADD <nick> <amt>     -- Add coins");
        pm(src, "  MUGSET <nick> <amt>     -- Set balance");
        pm(src, "  MUGTAKE <nick> <amt>    -- Remove coins");
        pm(src, "  MUGRESET [confirm]      -- Reset ALL data");
        pm(src, "  MUGSTATS               -- Economy overview");
        pm(src, "  ENABLE <#channel>       -- Add a channel (bot joins + listens)");
        pm(src, "  DISABLE <#channel>      -- Remove a channel");
        pm(src, "  MUGTOGGLE [#ch] [on|off] -- Per-channel enable/disable toggle");
        pm(src, "  GODMODE [on|off] [nick] -- Toggle 99% luck (PM only)");
        pm(src, "  UNCOOLDOWN <nick>       -- Clear flood lockout (PM only)");
        pm(src, "  HIGHSCORE              -- Show all-time high balance");
        pm(src, "");
        pm(src, "\002Channel Usage\002");
        pm(src, "  In any active channel, prefix commands with \002" + s_cmd_prefix + "\002:");
        pm(src, "  " + s_cmd_prefix + "coins   " + s_cmd_prefix + "mug Nick   "
                + s_cmd_prefix + "bet 100   " + s_cmd_prefix + "top5");
        pm(src, "  SHOP, BUY, INV, USE and admin commands always reply via PM.");
        pm(src, "  You can also always /msg MugServ directly.");
    }
};

// ---- Admin: ENABLE ----
struct CommandMugEnable : Command
{
    CommandMugEnable(Module *c) : Command(c, "mugserv/ENABLE", 1, 1)
    {
        SetDesc("[Admin] Enable MugServ in a channel");
        SetSyntax("<#channel>");
    }

    void Execute(CommandSource &src, const std::vector<Anope::string> &params) anope_override
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
        if (s_bot)
        {
            Channel *c = Channel::Find(chan);
            if (!c || !c->FindUser(s_bot))
                s_bot->Join(chan);
        }
        save_db();
        pm(src, "MugServ is now active in " + chan + ".");
    }
};

// ---- Admin: DISABLE ----
struct CommandMugDisable : Command
{
    CommandMugDisable(Module *c) : Command(c, "mugserv/DISABLE", 1, 1)
    {
        SetDesc("[Admin] Disable MugServ in a channel");
        SetSyntax("<#channel>");
    }

    void Execute(CommandSource &src, const std::vector<Anope::string> &params) anope_override
    {
        if (!is_admin(src)) { pm(src, "Access denied."); return; }
        Anope::string chan = params[0].lower();
        if (!s_channels.count(chan))
        {
            pm(src, chan + " is not in the active channel list.");
            return;
        }
        s_channels.erase(chan);
        if (s_bot)
        {
            Channel *c = Channel::Find(chan);
            if (c && c->FindUser(s_bot))
                s_bot->Part(c);
        }
        save_db();
        pm(src, "MugServ disabled in " + chan + ".");
    }
};

// ---- MUGTOGGLE ----
struct CommandMugToggle : Command
{
    CommandMugToggle(Module *c) : Command(c, "mugserv/MUGTOGGLE", 0, 2)
    {
        SetDesc("[Admin] Enable/disable MugServ in a channel");
        SetSyntax("[#channel] [on|off]");
    }

    void Execute(CommandSource &src, const std::vector<Anope::string> &params) anope_override
    {
        if (!is_admin(src)) { pm(src, "Access denied."); return; }

        // Determine channel and action from params
        Anope::string chan;
        Anope::string action;
        if (!params.empty() && !params[0].empty() && params[0][0] == '#')
        {
            chan = params[0].lower();
            if (params.size() > 1) action = params[1].lower();
        }
        else if (!params.empty())
        {
            action = params[0].lower();
        }

        // If no channel given, use current context channel
        if (chan.empty() && !s_current_chan.empty())
            chan = s_current_chan;

        if (chan.empty())
        {
            // Show all statuses
            pm(src, "Usage: MUGTOGGLE [#channel] [on|off]");
            if (s_channel_toggles.empty())
            {
                pm(src, "  All channels: ENABLED (default)");
            }
            else
            {
                for (std::map<Anope::string, bool>::const_iterator it = s_channel_toggles.begin();
                     it != s_channel_toggles.end(); ++it)
                    pm(src, "  " + it->first + ": " + (it->second ? "ENABLED" : "DISABLED"));
            }
            return;
        }

        if (action == "on" || action == "enable" || action == "true" || action == "1")
        {
            _set_channel_enabled(chan, true);
            // Also join if not in channel
            if (s_bot)
            {
                Channel *c = Channel::Find(chan);
                if (!c || !c->FindUser(s_bot))
                    s_bot->Join(chan);
            }
            pm(src, "Mug game ENABLED in " + chan + ". Let the chaos begin.");
        }
        else if (action == "off" || action == "disable" || action == "false" || action == "0")
        {
            _set_channel_enabled(chan, false);
            pm(src, "Mug game DISABLED in " + chan + ". All gameplay commands paused.");
        }
        else
        {
            bool cur = _plugin_enabled(chan);
            pm(src, "Mug game in " + chan + ": " + (cur ? "ENABLED" : "DISABLED"));
            pm(src, "Usage: MUGTOGGLE [#channel] [on|off]");
        }
    }
};

// ---- GODMODE ----
struct CommandMugGodMode : Command
{
    CommandMugGodMode(Module *c) : Command(c, "mugserv/GODMODE", 0, 2)
    {
        SetDesc("[Admin] Toggle near-guaranteed luck for a player");
        SetSyntax("[on|off] [nick]");
    }

    void Execute(CommandSource &src, const std::vector<Anope::string> &params) anope_override
    {
        if (!is_admin(src)) { pm(src, "Access denied."); return; }

        if (params.empty())
        {
            // Show status
            Anope::string self = src.nc ? src.nc->display.lower() : "";
            pm(src, "God mode for " + src.GetNick() + ": " +
                    (_has_godmode(self) ? "ON" : "OFF"));
            if (!s_godmode.empty())
            {
                Anope::string lst;
                for (std::set<Anope::string>::const_iterator it = s_godmode.begin();
                     it != s_godmode.end(); ++it)
                {
                    if (!lst.empty()) lst += ", ";
                    lst += *it;
                }
                pm(src, "Currently enabled: " + lst);
            }
            return;
        }

        Anope::string action = params[0].lower();
        Anope::string target = (params.size() > 1) ? params[1] : src.GetNick();
        Anope::string tkey = target.lower();

        if (action == "on" || action == "enable" || action == "true" || action == "1")
        {
            s_godmode.insert(tkey);
            pm(src, "God mode ENABLED for " + target + ". 99% luck on everything.");
        }
        else if (action == "off" || action == "disable" || action == "false" || action == "0")
        {
            s_godmode.erase(tkey);
            pm(src, "God mode DISABLED for " + target + ". Back to mortal luck.");
        }
        else
        {
            pm(src, "Usage: GODMODE [on|off] [nick]");
        }
    }
};

// ---- UNCOOLDOWN ----
struct CommandMugUncooldown : Command
{
    CommandMugUncooldown(Module *c) : Command(c, "mugserv/UNCOOLDOWN", 1, 1)
    {
        SetDesc("[Admin] Clear a player's flood lockout");
        SetSyntax("<nick>");
    }

    void Execute(CommandSource &src, const std::vector<Anope::string> &params) anope_override
    {
        if (!is_admin(src)) { pm(src, "Access denied."); return; }
        Anope::string acct = params[0];
        NickAlias *na = NickAlias::Find(acct);
        if (na && na->nc) acct = na->nc->display;
        _flood_clear(acct);
        pm(src, "Flood lockout cleared for " + params[0] + ".");
    }
};

// ---- HIGHSCORE ----
struct CommandMugHighScore : Command
{
    CommandMugHighScore(Module *c) : Command(c, "mugserv/HIGHSCORE", 0, 0)
    {
        SetDesc("Show the all-time highest balance ever achieved");
    }

    void Execute(CommandSource &src, const std::vector<Anope::string> &) anope_override
    {
        if (s_highscore_nick.empty() || s_highscore_amount <= 0)
        {
            pm(src, "No high score recorded yet. Go earn some coins!");
            return;
        }
        announce(src, "All-time high score: \002" + s_highscore_nick + "\002 with \002"
                       + fmt_coins(s_highscore_amount) + "\002 coins!");
    }
};

// ===========================================================================
// Dice casino helpers
// ===========================================================================

struct DiceGame { const char *key, *desc, *payout; };
static const DiceGame DICE_GAMES[] = {
    { "high",    "Roll 2d6 -- 7+ wins",           "2x"  },
    { "lucky7",  "Roll exactly 7",                "4x"  },
    { "snake",   "Roll snake eyes (1+1)",          "30x" },
    { "field",   "Roll 2,3,4,9,10,11,12 wins",    "2x (3x on 2/12)" },
    { "hardway", "Roll doubles (not snake eyes)",  "8x"  },
    { "yolo",    "Roll 2 or 12",                   "15x" },
};
static const int NUM_DICE_GAMES = 6;

static const char *DICE_FACES[] = { "", "\xe2\x9a\x80", "\xe2\x9a\x81", "\xe2\x9a\x82",
                                       "\xe2\x9a\x83", "\xe2\x9a\x84", "\xe2\x9a\x85" };

static bool eval_dice(const Anope::string &type, int d1, int d2,
                      long long amount, bool &won, long long &payout, Anope::string &flavor)
{
    int total = d1 + d2;
    if (type == "high")    { won = total >= 7;              payout = won ? amount*2  : 0; flavor = "Total " + stringify(total) + (won ? " -- winner!" : " -- under 7."); }
    else if (type == "lucky7")  { won = total == 7;         payout = won ? amount*4  : 0; flavor = won ? "Lucky 7!" : "Total " + stringify(total) + " -- needed 7."; }
    else if (type == "snake")   { won = d1==1 && d2==1;     payout = won ? amount*30 : 0; flavor = won ? "SNAKE EYES! Legendary!" : "No snakes here."; }
    else if (type == "field")   { static const int fn[] = {2,3,4,9,10,11,12}; bool inf=false; for(int i=0;i<7;i++) if(total==fn[i]){inf=true;break;} won=inf; int mult=(total==2||total==12)?3:2; payout=won?amount*mult:0; flavor=won?("Field "+stringify(total)+"! "+(mult==3?"Triple!":"Double!")):"Total "+stringify(total)+" -- not in the field."; }
    else if (type == "hardway") { won = d1==d2 && !(d1==1&&d2==1); payout = won ? amount*8 : 0; flavor = won ? "Hard " + stringify(total) + "! Doubles pay fat!" : "No doubles."; }
    else if (type == "yolo")    { won = total==2||total==12; payout = won ? amount*15 : 0; flavor = won ? "YOLO PAYS!" : "YOLO didn't pay this time."; }
    else return false;
    return true;
}

// ---- ROLL ----
struct CommandMugRoll : Command
{
    CommandMugRoll(Module *c) : Command(c, "mugserv/ROLL", 1, 2)
    {
        SetDesc("Roll the dice casino");
        SetSyntax("<amount> [high|lucky7|snake|field|hardway|yolo]");
    }

    void Execute(CommandSource &src, const std::vector<Anope::string> &params) anope_override
    {
        if (check_gate(src)) return;
        if (_flood_check(src)) return;
        MugUser &u = s_users[src.nc->display.lower()];

        if (params.empty() || params[0].lower() == "help")
        {
            pm(src, "ROLL <amount> [type] -- Dice casino. Types:");
            for (int i = 0; i < NUM_DICE_GAMES; ++i)
                pm(src, "  " + Anope::string(DICE_GAMES[i].key) + " -- "
                        + Anope::string(DICE_GAMES[i].desc) + " -> " + Anope::string(DICE_GAMES[i].payout));
            return;
        }

        long long amount = parse_ll(params[0]);
        if (amount < 1) { pm(src, "Amount must be >= 1."); return; }

        Anope::string dice_type = (params.size() > 1) ? params[1].lower() : Anope::string("high");

        // Validate type
        bool valid = false;
        for (int i = 0; i < NUM_DICE_GAMES; ++i)
            if (dice_type == DICE_GAMES[i].key) { valid = true; break; }
        if (!valid) { pm(src, "Unknown dice type. Options: high, lucky7, snake, field, hardway, yolo"); return; }

        int rem = cd_rem(u.last_bet, CD_BET);
        if (rem > 0 && !is_admin(src))
        {
            pm(src, tpl(rand_pick_str(s_msg_bet_cd), mkv1("t", fmt_dur(rem))));
            return;
        }

        if (u.coins < amount) { pm(src, rand_pick_str(s_msg_broke)); return; }

        u.coins   -= amount;
        u.last_bet = Anope::CurTime;

        int d1, d2;
        if (_has_godmode(src.nc->display))
        {
            // Rig for a win
            if (dice_type == "snake")        { d1=1; d2=1; }
            else if (dice_type == "lucky7")  { d1=3; d2=4; }
            else if (dice_type == "hardway") { d1=3; d2=3; }
            else if (dice_type == "yolo")    { d1=6; d2=6; }
            else if (dice_type == "field")   { d1=6; d2=6; }
            else                             { d1=4; d2=4; }  // high: total 8
        }
        else
        {
            d1 = ri(1,6); d2 = ri(1,6);
        }

        bool won; long long payout; Anope::string flavor;
        eval_dice(dice_type, d1, d2, amount, won, payout, flavor);

        Anope::string f1 = (d1 >= 1 && d1 <= 6) ? Anope::string(DICE_FACES[d1]) : stringify(d1);
        Anope::string f2 = (d2 >= 1 && d2 <= 6) ? Anope::string(DICE_FACES[d2]) : stringify(d2);

        if (won)
        {
            u.coins += payout;
            _update_highscore(u);
            announce(src, "\002ROLL\002 " + f1 + f2 + " " + u.nick
                + " rolls " + stringify(d1) + "+" + stringify(d2) + "=" + stringify(d1+d2)
                + " on " + dice_type + "! " + flavor
                + " Payout: " + fmt_coins(payout) + ". Balance: \002" + fmt_coins(u.coins) + "\002");
        }
        else
        {
            announce(src, "\002ROLL\002 " + f1 + f2 + " " + u.nick
                + " rolls " + stringify(d1) + "+" + stringify(d2) + "=" + stringify(d1+d2)
                + " on " + dice_type + ". " + flavor
                + " Lost " + fmt_coins(amount) + ". Balance: \002" + fmt_coins(u.coins) + "\002");
        }
    }
};

// ===========================================================================
// Slot machine helpers
// ===========================================================================

struct SlotPrize { int weight; long long payout; const char *reels; const char *msg; };

static const SlotPrize PENNY_PRIZES[] = {
    { 35, 0,    "\xf0\x9f\x92\x80\xf0\x9f\x92\x80\xf0\x9f\x92\x80", "Nothing. The machine laughs at you." },
    { 20, 0,    "\xf0\x9f\x8d\x8b\xf0\x9f\x8d\x92\xf0\x9f\x92\x80", "Two fruits and a skull. Almost." },
    { 12, 2,    "\xf0\x9f\x8d\x92\xf0\x9f\x8d\x92\xf0\x9f\x8d\x8b", "Two cherries! {coins} back." },
    { 8,  5,    "\xf0\x9f\x8d\x92\xf0\x9f\x8d\x92\xf0\x9f\x8d\x92", "Triple cherries! +{coins}!" },
    { 6,  15,   "\xf0\x9f\x8d\x8b\xf0\x9f\x8d\x8b\xf0\x9f\x8d\x8b", "Lemons! Sour but sweet -- +{coins}!" },
    { 5,  50,   "\xf0\x9f\x94\x94\xf0\x9f\x94\x94\xf0\x9f\x94\x94", "DING DING DING! +{coins}!" },
    { 4,  150,  "\xf0\x9f\x92\x8e\xf0\x9f\x92\x8e\xf0\x9f\x94\x94", "Diamonds and a bell! +{coins}!" },
    { 3,  500,  "\xf0\x9f\x92\x8e\xf0\x9f\x92\x8e\xf0\x9f\x92\x8e", "TRIPLE DIAMONDS! +{coins}!" },
    { 2,  1500, "\xf0\x9f\x94\xa5\xf0\x9f\x94\xa5\xf0\x9f\x94\xa5", "FIRE SPIN!!! +{coins}! The machine is SMOKING!" },
    { 1,  5000, "\xf0\x9f\x91\x91\xf0\x9f\x91\x91\xf0\x9f\x91\x91", "JACKPOT!!! +{coins}!!! THE CROWD GOES WILD!!!" },
};
static const int NUM_PENNY_PRIZES = 10;

static const SlotPrize DOLLAR_PRIZES[] = {
    { 30, 0,     "\xe2\x98\xa0\xef\xb8\x8f\xe2\x98\xa0\xef\xb8\x8f\xe2\x98\xa0\xef\xb8\x8f", "Dead on arrival. The machine devours your dollar." },
    { 18, 0,     "\xf0\x9f\x8d\x87\xf0\x9f\x8d\x8a\xe2\x98\xa0\xef\xb8\x8f", "Fruit salad of failure. Nothing." },
    { 12, 50,    "\xf0\x9f\x8d\x8a\xf0\x9f\x8d\x8a\xf0\x9f\x8d\x87", "Two oranges! Partial refund -- {coins} back." },
    { 9,  200,   "\xf0\x9f\x8d\x8a\xf0\x9f\x8d\x8a\xf0\x9f\x8d\x8a", "Triple oranges! +{coins}!" },
    { 7,  500,   "\xf0\x9f\x8d\x87\xf0\x9f\x8d\x87\xf0\x9f\x8d\x87", "Grapes! Wine money -- +{coins}!" },
    { 6,  1500,  "\xf0\x9f\x92\xb0\xf0\x9f\x92\xb0\xf0\x9f\x8d\x87", "Two money bags! +{coins}!" },
    { 5,  3000,  "\xf0\x9f\x92\xb0\xf0\x9f\x92\xb0\xf0\x9f\x92\xb0", "TRIPLE MONEY BAGS! +{coins}!" },
    { 4,  8000,  "\xf0\x9f\x8c\x9f\xf0\x9f\x8c\x9f\xf0\x9f\x92\xb0", "Shooting stars! +{coins}!" },
    { 3,  15000, "\xf0\x9f\x8c\x9f\xf0\x9f\x8c\x9f\xf0\x9f\x8c\x9f", "TRIPLE STARS! +{coins}! The machine is GLOWING!" },
    { 2,  30000, "\xf0\x9f\x92\x8e\xf0\x9f\x94\xa5\xf0\x9f\x92\x8e", "INFERNO DIAMONDS! +{coins}! Security is on their way!" },
    { 1,  50000, "\xf0\x9f\x8f\x86\xf0\x9f\x8f\x86\xf0\x9f\x8f\x86", "MEGA JACKPOT!!! +{coins}!!! THE FLOOR ERUPTS!!!" },
};
static const int NUM_DOLLAR_PRIZES = 11;

static void spin_slot(const SlotPrize *prizes, int count, bool godmode,
                      long long &payout, Anope::string &reels, Anope::string &msg)
{
    if (godmode)
    {
        const SlotPrize &p = prizes[count - 1];
        payout = p.payout; reels = p.reels; msg = p.msg;
        return;
    }
    int total = 0;
    for (int i = 0; i < count; ++i) total += prizes[i].weight;
    int roll = ri(1, total);
    int cum = 0;
    for (int i = 0; i < count; ++i)
    {
        cum += prizes[i].weight;
        if (roll <= cum)
        {
            payout = prizes[i].payout;
            reels  = prizes[i].reels;
            msg    = prizes[i].msg;
            return;
        }
    }
    payout = 0; reels = prizes[0].reels; msg = prizes[0].msg;
}

// ---- PENNY ----
struct CommandMugPenny : Command
{
    CommandMugPenny(Module *c) : Command(c, "mugserv/PENNY", 0, 0)
    {
        SetDesc("Pull the penny slot machine (1 coin per spin)");
    }

    void Execute(CommandSource &src, const std::vector<Anope::string> &) anope_override
    {
        if (check_gate(src)) return;
        if (_flood_check(src)) return;
        MugUser &u = s_users[src.nc->display.lower()];
        time_t now = Anope::CurTime;

        // Penny has its own cooldown stored in last_bet only if we share;
        // we use a dedicated field via MugUser::last_coins trick — actually
        // we reuse last_bet to share the CD as per Sopel design.
        // For penny we store timestamp in a side-map using acct key.
        static std::map<Anope::string, time_t> s_last_penny;
        Anope::string key = src.nc->display.lower();
        time_t &last_penny = s_last_penny[key];
        int rem = (now - last_penny < static_cast<time_t>(PENNY_COOLDOWN))
                  ? static_cast<int>(PENNY_COOLDOWN - (now - last_penny)) : 0;
        if (rem > 0 && !is_admin(src))
        {
            pm(src, "The machine needs a second to cool down. " + fmt_dur(rem) + ".");
            return;
        }

        if (u.coins < PENNY_COST)
        {
            pm(src, "You don't even have a single coin for the penny slot.");
            return;
        }

        u.coins -= PENNY_COST;
        last_penny = now;

        long long payout; Anope::string reels, msg;
        spin_slot(PENNY_PRIZES, NUM_PENNY_PRIZES,
                  _has_godmode(src.nc->display), payout, reels, msg);

        if (payout > 0)
        {
            u.coins += payout;
            _update_highscore(u);
        }

        Anope::string msg_f = tpl(msg, mkv1("coins", fmt_coins(payout)));
        announce(src, "\002PENNY\002 [" + reels + "] " + u.nick + " -- " + msg_f
                      + " Balance: \002" + fmt_coins(u.coins) + "\002");
    }
};

// ---- DOLLAR ----
struct CommandMugDollar : Command
{
    CommandMugDollar(Module *c) : Command(c, "mugserv/DOLLAR", 0, 0)
    {
        SetDesc("Pull the dollar slot machine (100 coins per spin)");
    }

    void Execute(CommandSource &src, const std::vector<Anope::string> &) anope_override
    {
        if (check_gate(src)) return;
        if (_flood_check(src)) return;
        MugUser &u = s_users[src.nc->display.lower()];
        time_t now = Anope::CurTime;

        static std::map<Anope::string, time_t> s_last_dollar;
        Anope::string key = src.nc->display.lower();
        time_t &last_dollar = s_last_dollar[key];
        int rem = (now - last_dollar < static_cast<time_t>(DOLLAR_COOLDOWN))
                  ? static_cast<int>(DOLLAR_COOLDOWN - (now - last_dollar)) : 0;
        if (rem > 0 && !is_admin(src))
        {
            pm(src, "The dollar machine is recalibrating. " + fmt_dur(rem) + ".");
            return;
        }

        if (u.coins < DOLLAR_COST)
        {
            pm(src, "You need at least " + fmt_coins(DOLLAR_COST) + " for the dollar machine.");
            return;
        }

        u.coins -= DOLLAR_COST;
        last_dollar = now;

        long long payout; Anope::string reels, msg;
        spin_slot(DOLLAR_PRIZES, NUM_DOLLAR_PRIZES,
                  _has_godmode(src.nc->display), payout, reels, msg);

        if (payout > 0)
        {
            u.coins += payout;
            _update_highscore(u);
        }

        Anope::string msg_f = tpl(msg, mkv1("coins", fmt_coins(payout)));
        announce(src, "\002DOLLAR\002 [" + reels + "] " + u.nick + " -- " + msg_f
                      + " Balance: \002" + fmt_coins(u.coins) + "\002");
    }
};

// ===========================================================================
// Roulette helpers
// ===========================================================================

static const int ROULETTE_REDS[] = {1,3,5,7,9,12,14,16,18,19,21,23,25,27,30,32,34,36};
static const int NUM_ROULETTE_REDS = 18;

static bool roulette_is_red(int n)
{
    for (int i = 0; i < NUM_ROULETTE_REDS; ++i)
        if (ROULETTE_REDS[i] == n) return true;
    return false;
}

static Anope::string roulette_color(int n)
{
    if (n == 0) return "green";
    return roulette_is_red(n) ? "red" : "black";
}

// Returns false if bet_str is invalid
static bool roulette_eval(const Anope::string &bet, int number,
                          bool &won, int &mult, Anope::string &desc)
{
    Anope::string b = bet.lower();
    Anope::string col = roulette_color(number);
    if (b == "red")    { won = col=="red";   mult=2; desc="Red";       return true; }
    if (b == "black")  { won = col=="black"; mult=2; desc="Black";     return true; }
    if (b == "odd")    { won = number!=0 && number%2==1; mult=2; desc="Odd";  return true; }
    if (b == "even")   { won = number!=0 && number%2==0; mult=2; desc="Even"; return true; }
    if (b == "low")    { won = number>=1 && number<=18;  mult=2; desc="Low (1-18)";  return true; }
    if (b == "high")   { won = number>=19 && number<=36; mult=2; desc="High (19-36)"; return true; }
    if (b == "1st")    { won = number>=1 && number<=12;  mult=3; desc="1st Dozen"; return true; }
    if (b == "2nd")    { won = number>=13 && number<=24; mult=3; desc="2nd Dozen"; return true; }
    if (b == "3rd")    { won = number>=25 && number<=36; mult=3; desc="3rd Dozen"; return true; }
    // Straight number
    std::istringstream iss(b.c_str());
    int target = -1;
    if ((iss >> target) && target >= 0 && target <= 36)
    {
        won = number == target; mult = 36;
        desc = "Straight " + stringify(target);
        return true;
    }
    return false;
}

// ---- ROULETTE ----
struct CommandMugRoulette : Command
{
    CommandMugRoulette(Module *c) : Command(c, "mugserv/ROULETTE", 2, 2)
    {
        SetDesc("Spin the roulette wheel");
        SetSyntax("<amount> <red|black|odd|even|high|low|1st|2nd|3rd|0-36>");
    }

    void Execute(CommandSource &src, const std::vector<Anope::string> &params) anope_override
    {
        if (check_gate(src)) return;
        if (_flood_check(src)) return;
        MugUser &u = s_users[src.nc->display.lower()];

        long long amount = parse_ll(params[0]);
        if (amount < 1) { pm(src, "Amount must be >= 1."); return; }

        Anope::string bet_str = params[1].lower();

        // Validate bet (use number 0 as test)
        bool twon; int tmult; Anope::string tdesc;
        if (!roulette_eval(bet_str, 0, twon, tmult, tdesc))
        {
            pm(src, "Invalid bet. Options: red, black, odd, even, high, low, 1st, 2nd, 3rd, or 0-36.");
            return;
        }

        int rem = cd_rem(u.last_bet, CD_BET);
        if (rem > 0 && !is_admin(src))
        {
            pm(src, tpl(rand_pick_str(s_msg_bet_cd), mkv1("t", fmt_dur(rem))));
            return;
        }

        if (u.coins < amount) { pm(src, rand_pick_str(s_msg_broke)); return; }

        u.coins   -= amount;
        u.last_bet = Anope::CurTime;

        int number;
        if (_has_godmode(src.nc->display))
        {
            // Rig for a win
            if (bet_str == "red")        { number = ROULETTE_REDS[ri(0, NUM_ROULETTE_REDS-1)]; }
            else if (bet_str == "black")
            {
                int blacks[] = {2,4,6,8,10,11,13,15,17,20,22,24,26,28,29,31,33,35};
                number = blacks[ri(0,17)];
            }
            else if (bet_str == "odd")   { number = ri(0,17)*2+1; }
            else if (bet_str == "even")  { number = ri(0,17)*2+2; }
            else if (bet_str == "low")   { number = ri(1,18); }
            else if (bet_str == "high")  { number = ri(19,36); }
            else if (bet_str == "1st")   { number = ri(1,12); }
            else if (bet_str == "2nd")   { number = ri(13,24); }
            else if (bet_str == "3rd")   { number = ri(25,36); }
            else { std::istringstream iss(bet_str.c_str()); iss >> number; }
        }
        else
        {
            number = ri(0, 36);
        }

        bool r_won; int r_mult; Anope::string r_desc;
        roulette_eval(bet_str, number, r_won, r_mult, r_desc);

        Anope::string col = roulette_color(number);
        Anope::string col_tag = (col=="red") ? "\002RED\002" : (col=="black") ? "\002BLACK\002" : "\002GREEN\002";

        if (r_won)
        {
            long long payout = amount * static_cast<long long>(r_mult);
            u.coins += payout;
            _update_highscore(u);
            announce(src, "\002ROULETTE\002 " + col_tag + " " + stringify(number)
                + " | " + u.nick + " bet " + r_desc + " -- \002WIN!\002 "
                + "Payout: " + fmt_coins(payout) + " (" + stringify(r_mult) + "x). "
                + "Balance: \002" + fmt_coins(u.coins) + "\002");
        }
        else
        {
            announce(src, "\002ROULETTE\002 " + col_tag + " " + stringify(number)
                + " | " + u.nick + " bet " + r_desc + " -- nope. "
                + "Lost " + fmt_coins(amount) + ". Balance: \002" + fmt_coins(u.coins) + "\002");
        }
    }
};

// ===========================================================================
// Blackjack helpers
// ===========================================================================

static void bj_new_deck(BJHand &h)
{
    static const char *SUITS[] = { "s", "h", "d", "c" };
    static const char *RANKS[] = { "A","2","3","4","5","6","7","8","9","10","J","Q","K" };
    h.deck.clear();
    for (int s = 0; s < 4; ++s)
        for (int r = 0; r < 13; ++r)
        {
            BJHand::Card c;
            c.rank = RANKS[r]; c.suit = SUITS[s];
            h.deck.push_back(c);
        }
    // Shuffle (Fisher-Yates with ri)
    for (int i = static_cast<int>(h.deck.size()) - 1; i > 0; --i)
    {
        int j = ri(0, i);
        BJHand::Card tmp = h.deck[i];
        h.deck[i] = h.deck[j];
        h.deck[j] = tmp;
    }
}

static BJHand::Card bj_draw(BJHand &h)
{
    BJHand::Card c = h.deck.back();
    h.deck.pop_back();
    return c;
}

static Anope::string bj_card_str(const BJHand::Card &c) { return c.rank + c.suit; }

static Anope::string bj_hand_str(const std::vector<BJHand::Card> &hand)
{
    Anope::string s;
    for (size_t i = 0; i < hand.size(); ++i)
    {
        if (i) s += " ";
        s += bj_card_str(hand[i]);
    }
    return s;
}

static int bj_card_val(const Anope::string &rank)
{
    if (rank == "A") return 11;
    if (rank == "K" || rank == "Q" || rank == "J") return 10;
    std::istringstream iss(rank.c_str()); int v=0; iss>>v; return v;
}

static int bj_hand_val(const std::vector<BJHand::Card> &hand)
{
    int total = 0, aces = 0;
    for (size_t i = 0; i < hand.size(); ++i)
    {
        total += bj_card_val(hand[i].rank);
        if (hand[i].rank == "A") ++aces;
    }
    while (total > 21 && aces > 0) { total -= 10; --aces; }
    return total;
}

static bool bj_is_blackjack(const std::vector<BJHand::Card> &hand)
{
    return hand.size() == 2 && bj_hand_val(hand) == 21;
}

static void bj_dealer_play(BJHand &h, bool godmode)
{
    if (godmode)
    {
        while (bj_hand_val(h.dealer) < 22 && !h.deck.empty())
            h.dealer.push_back(bj_draw(h));
        return;
    }
    while (bj_hand_val(h.dealer) < 17 && !h.deck.empty())
        h.dealer.push_back(bj_draw(h));
}

static void bj_resolve(CommandSource &src, const Anope::string &nick,
                       const Anope::string &acct_key, BJHand &game)
{
    MugUser &u = s_users[acct_key];
    int hv = bj_hand_val(game.hand);
    int dv = bj_hand_val(game.dealer);
    long long amount = game.amount;
    Anope::string chan = game.channel;

    bool pbj = bj_is_blackjack(game.hand);
    bool dbj = bj_is_blackjack(game.dealer);

    Anope::string dstr = bj_hand_str(game.dealer);

    if (pbj && dbj)
    {
        u.coins += amount;
        IRCD->SendPrivmsg(MessageSource(s_bot), chan, "BJ: Dealer %s (%d) -- Both blackjack! Push. %s gets %s back. Balance: \002%s\002",
            dstr.c_str(), dv, nick.c_str(), fmt_coins(amount).c_str(), fmt_coins(u.coins).c_str());
    }
    else if (pbj)
    {
        long long payout = amount * 5 / 2; // 2.5x
        u.coins += payout;
        _update_highscore(u);
        IRCD->SendPrivmsg(MessageSource(s_bot), chan, "BJ: BLACKJACK! %s wins %s (2.5x)! Balance: \002%s\002",
            nick.c_str(), fmt_coins(payout).c_str(), fmt_coins(u.coins).c_str());
    }
    else if (hv > 21)
    {
        IRCD->SendPrivmsg(MessageSource(s_bot), chan, "BJ: BUST! %s (%d). Dealer: %s (%d). %s lost %s. Balance: \002%s\002",
            bj_hand_str(game.hand).c_str(), hv, dstr.c_str(), dv,
            nick.c_str(), fmt_coins(amount).c_str(), fmt_coins(u.coins).c_str());
    }
    else if (dv > 21)
    {
        long long payout = amount * 2;
        u.coins += payout;
        _update_highscore(u);
        IRCD->SendPrivmsg(MessageSource(s_bot), chan, "BJ: Dealer BUSTS! %s (%d). %s wins %s! Balance: \002%s\002",
            dstr.c_str(), dv, nick.c_str(), fmt_coins(payout).c_str(), fmt_coins(u.coins).c_str());
    }
    else if (hv > dv)
    {
        long long payout = amount * 2;
        u.coins += payout;
        _update_highscore(u);
        IRCD->SendPrivmsg(MessageSource(s_bot), chan, "BJ: %s (%d) vs Dealer %s (%d) -- %s WINS! +%s. Balance: \002%s\002",
            bj_hand_str(game.hand).c_str(), hv, dstr.c_str(), dv,
            nick.c_str(), fmt_coins(payout).c_str(), fmt_coins(u.coins).c_str());
    }
    else if (hv < dv)
    {
        IRCD->SendPrivmsg(MessageSource(s_bot), chan, "BJ: %s (%d) vs Dealer %s (%d) -- Dealer wins. %s lost %s. Balance: \002%s\002",
            bj_hand_str(game.hand).c_str(), hv, dstr.c_str(), dv,
            nick.c_str(), fmt_coins(amount).c_str(), fmt_coins(u.coins).c_str());
    }
    else
    {
        u.coins += amount;
        IRCD->SendPrivmsg(MessageSource(s_bot), chan, "BJ: %s (%d) vs Dealer %s (%d) -- Push! %s gets %s back. Balance: \002%s\002",
            bj_hand_str(game.hand).c_str(), hv, dstr.c_str(), dv,
            nick.c_str(), fmt_coins(amount).c_str(), fmt_coins(u.coins).c_str());
    }
}

// ---- BJ (start blackjack) ----
struct CommandMugBJ : Command
{
    CommandMugBJ(Module *c) : Command(c, "mugserv/BJ", 1, 1)
    {
        SetDesc("Start a blackjack hand vs the dealer");
        SetSyntax("<amount>");
    }

    void Execute(CommandSource &src, const std::vector<Anope::string> &params) anope_override
    {
        if (check_gate(src)) return;
        if (_flood_check(src)) return;
        Anope::string acct = src.nc->display.lower();
        MugUser &u = s_users[acct];

        if (s_bj_hands.count(acct))
        {
            BJHand &game = s_bj_hands[acct];
            pm(src, "You already have a hand: " + bj_hand_str(game.hand)
                    + " (" + stringify(bj_hand_val(game.hand)) + "). Use HIT, STAND, or DD.");
            return;
        }

        int rem = cd_rem(u.last_bet, CD_BET);
        if (rem > 0 && !is_admin(src))
        {
            pm(src, tpl(rand_pick_str(s_msg_bet_cd), mkv1("t", fmt_dur(rem))));
            return;
        }

        long long amount = parse_ll(params[0]);
        if (amount < 1) { pm(src, "Minimum bet is 1 coin."); return; }
        if (u.coins < amount) { pm(src, rand_pick_str(s_msg_broke)); return; }

        u.coins   -= amount;
        u.last_bet = Anope::CurTime;

        BJHand game;
        bj_new_deck(game);
        game.hand.push_back(bj_draw(game));
        game.hand.push_back(bj_draw(game));
        game.dealer.push_back(bj_draw(game));
        game.dealer.push_back(bj_draw(game));
        game.amount  = amount;
        game.channel = s_current_chan.empty() ? Anope::string("") : s_current_chan;
        game.doubled = false;

        if (bj_is_blackjack(game.hand))
        {
            bj_dealer_play(game, _has_godmode(src.nc->display));
            bj_resolve(src, src.GetNick(), acct, game);
            return;
        }

        s_bj_hands[acct] = game;

        Anope::string dealer_show = bj_card_str(game.dealer[0]);
        announce(src, "\002BJ\002 " + src.GetNick()
            + " -- Hand: " + bj_hand_str(game.hand)
            + " (" + stringify(bj_hand_val(game.hand)) + ")"
            + " | Dealer shows: " + dealer_show
            + " | Use HIT, STAND, or DD");
    }
};

// ---- HIT ----
struct CommandMugHit : Command
{
    CommandMugHit(Module *c) : Command(c, "mugserv/HIT", 0, 0)
    {
        SetDesc("Draw another card in blackjack");
    }

    void Execute(CommandSource &src, const std::vector<Anope::string> &) anope_override
    {
        if (!src.nc) { pm(src, "Identify with NickServ first."); return; }
        Anope::string acct = src.nc->display.lower();
        std::map<Anope::string, BJHand>::iterator it = s_bj_hands.find(acct);
        if (it == s_bj_hands.end())
        {
            pm(src, "You don't have a hand. Start one with BJ <amount>.");
            return;
        }
        BJHand &game = it->second;
        game.hand.push_back(bj_draw(game));
        int hv = bj_hand_val(game.hand);

        if (hv > 21)
        {
            BJHand copy = game;
            s_bj_hands.erase(it);
            bj_dealer_play(copy, false);
            bj_resolve(src, src.GetNick(), acct, copy);
            return;
        }

        announce(src, "\002BJ HIT\002 " + src.GetNick()
            + " drew " + bj_card_str(game.hand.back())
            + " -- Hand: " + bj_hand_str(game.hand)
            + " (" + stringify(hv) + ") | HIT, STAND, or DD");
    }
};

// ---- STAND ----
struct CommandMugStand : Command
{
    CommandMugStand(Module *c) : Command(c, "mugserv/STAND", 0, 0)
    {
        SetDesc("Keep your hand, dealer plays");
    }

    void Execute(CommandSource &src, const std::vector<Anope::string> &) anope_override
    {
        if (!src.nc) { pm(src, "Identify with NickServ first."); return; }
        Anope::string acct = src.nc->display.lower();
        std::map<Anope::string, BJHand>::iterator it = s_bj_hands.find(acct);
        if (it == s_bj_hands.end())
        {
            pm(src, "You don't have a hand. Start one with BJ <amount>.");
            return;
        }
        BJHand game = it->second;
        s_bj_hands.erase(it);
        bj_dealer_play(game, _has_godmode(src.nc->display));
        bj_resolve(src, src.GetNick(), acct, game);
    }
};

// ---- DD (double down) ----
struct CommandMugDD : Command
{
    CommandMugDD(Module *c) : Command(c, "mugserv/DD", 0, 0)
    {
        SetDesc("Double down in blackjack: double bet, draw one card, auto-stand");
    }

    void Execute(CommandSource &src, const std::vector<Anope::string> &) anope_override
    {
        if (!src.nc) { pm(src, "Identify with NickServ first."); return; }
        Anope::string acct = src.nc->display.lower();
        std::map<Anope::string, BJHand>::iterator it = s_bj_hands.find(acct);
        if (it == s_bj_hands.end())
        {
            pm(src, "You don't have a hand. Start one with BJ <amount>.");
            return;
        }
        BJHand &game = it->second;
        if (game.hand.size() != 2)
        {
            pm(src, "You can only double down on your first two cards.");
            return;
        }
        if (game.doubled) { pm(src, "Already doubled!"); return; }

        MugUser &u = s_users[acct];
        if (u.coins < game.amount)
        {
            pm(src, "You need " + fmt_coins(game.amount) + " more to double down.");
            return;
        }
        u.coins     -= game.amount;
        game.amount *= 2;
        game.doubled = true;
        game.hand.push_back(bj_draw(game));

        BJHand copy = game;
        s_bj_hands.erase(it);

        announce(src, "\002BJ DD\002 DOUBLE DOWN! " + src.GetNick()
            + " drew " + bj_card_str(copy.hand.back())
            + " -- Hand: " + bj_hand_str(copy.hand)
            + " (" + stringify(bj_hand_val(copy.hand)) + ")");

        bj_dealer_play(copy, _has_godmode(src.nc->display));
        bj_resolve(src, src.GetNick(), acct, copy);
    }
};

// ===========================================================================
// Texas Hold'em helpers
// ===========================================================================

static const int HOLDEM_RANK_VALUES[] = {0,0,2,3,4,5,6,7,8,9,10,11,12,13,14};
// rank char -> value: A=14

static int holdem_rank_val(const Anope::string &rank)
{
    if (rank == "A") return 14;
    if (rank == "K") return 13;
    if (rank == "Q") return 12;
    if (rank == "J") return 11;
    std::istringstream iss(rank.c_str()); int v=0; iss>>v; return v;
}

struct HoldemScore
{
    int rank; // 0=high card ... 9=royal flush
    int tb[5]; // tiebreakers
    HoldemScore() : rank(0) { for(int i=0;i<5;i++) tb[i]=0; }
    bool operator>(const HoldemScore &o) const
    {
        if (rank != o.rank) return rank > o.rank;
        for (int i=0;i<5;i++) if(tb[i]!=o.tb[i]) return tb[i]>o.tb[i];
        return false;
    }
    bool operator==(const HoldemScore &o) const
    {
        if (rank != o.rank) return false;
        for (int i=0;i<5;i++) if(tb[i]!=o.tb[i]) return false;
        return true;
    }
};

static HoldemScore holdem_score_five(const BJHand::Card five[5])
{
    int ranks[5]; Anope::string suits[5];
    for (int i=0;i<5;i++) { ranks[i]=holdem_rank_val(five[i].rank); suits[i]=five[i].suit; }
    // Sort ranks descending
    for (int i=0;i<4;i++) for(int j=i+1;j<5;j++) if(ranks[j]>ranks[i]){ int t=ranks[i];ranks[i]=ranks[j];ranks[j]=t; }
    bool is_flush = (suits[0]==suits[1]&&suits[1]==suits[2]&&suits[2]==suits[3]&&suits[3]==suits[4]);
    bool is_straight = (ranks[0]-ranks[4]==4 && (ranks[0]!=ranks[1]&&ranks[1]!=ranks[2]&&ranks[2]!=ranks[3]&&ranks[3]!=ranks[4]));
    // Ace-low straight
    bool ace_low = (ranks[0]==14&&ranks[1]==5&&ranks[2]==4&&ranks[3]==3&&ranks[4]==2);
    int straight_high = ace_low ? 5 : ranks[0];

    // Count rank groups
    int cnt[15] = {};
    for (int i=0;i<5;i++) cnt[ranks[i]]++;
    int quad=-1,trip=-1,pair1=-1,pair2=-1;
    for (int r=14;r>=2;r--)
    {
        if(cnt[r]==4) quad=r;
        else if(cnt[r]==3) trip=r;
        else if(cnt[r]==2) { if(pair1<0) pair1=r; else pair2=r; }
    }

    HoldemScore s;
    if (is_flush && (is_straight||ace_low))
    {
        s.rank = (straight_high==14&&!ace_low) ? 9 : 8;
        s.tb[0] = straight_high; return s;
    }
    if (quad>=0) { s.rank=7; s.tb[0]=quad; for(int r=14;r>=2;r--) if(cnt[r]==1){s.tb[1]=r;break;} return s; }
    if (trip>=0&&pair1>=0) { s.rank=6; s.tb[0]=trip; s.tb[1]=pair1; return s; }
    if (is_flush) { s.rank=5; for(int i=0;i<5;i++) s.tb[i]=ranks[i]; return s; }
    if (is_straight||ace_low) { s.rank=4; s.tb[0]=straight_high; return s; }
    if (trip>=0) { s.rank=3; s.tb[0]=trip; int ki=0; for(int r=14;r>=2;r--) if(cnt[r]==1&&ki<2){s.tb[1+ki]=r;ki++;} return s; }
    if (pair1>=0&&pair2>=0) { s.rank=2; s.tb[0]=pair1; s.tb[1]=pair2; for(int r=14;r>=2;r--) if(cnt[r]==1){s.tb[2]=r;break;} return s; }
    if (pair1>=0) { s.rank=1; s.tb[0]=pair1; int ki=0; for(int r=14;r>=2;r--) if(cnt[r]==1&&ki<3){s.tb[1+ki]=r;ki++;} return s; }
    s.rank=0; for(int i=0;i<5;i++) s.tb[i]=ranks[i]; return s;
}

static HoldemScore holdem_best_hand(const std::vector<BJHand::Card> &all7, Anope::string &hand_name)
{
    static const char *HAND_NAMES[] = { "High Card","One Pair","Two Pair","Three of a Kind",
        "Straight","Flush","Full House","Four of a Kind","Straight Flush","Royal Flush" };
    HoldemScore best;
    bool first = true;
    // Iterate all C(7,5) = 21 combinations
    for (int a=0;a<7;a++) for(int b=a+1;b<7;b++)
    {
        // pick 5 that are NOT a and b
        BJHand::Card five[5]; int fi=0;
        for(int i=0;i<7;i++) if(i!=a&&i!=b) five[fi++]=all7[i];
        HoldemScore sc = holdem_score_five(five);
        if (first || sc > best) { best = sc; first = false; }
    }
    hand_name = (best.rank>=0&&best.rank<=9) ? HAND_NAMES[best.rank] : "Unknown";
    return best;
}

static const int HOLDEM_PAYOUTS[] = { 2,2,2,2,3,4,6,12,25,50 };

// ---- HOLDEM ----
struct CommandMugHoldem : Command
{
    CommandMugHoldem(Module *c) : Command(c, "mugserv/HOLDEM", 1, 1)
    {
        SetDesc("Heads-up Texas Hold'em vs the dealer");
        SetSyntax("<amount>");
    }

    void Execute(CommandSource &src, const std::vector<Anope::string> &params) anope_override
    {
        if (check_gate(src)) return;
        if (_flood_check(src)) return;
        MugUser &u = s_users[src.nc->display.lower()];

        int rem = cd_rem(u.last_bet, CD_BET);
        if (rem > 0 && !is_admin(src))
        {
            pm(src, tpl(rand_pick_str(s_msg_bet_cd), mkv1("t", fmt_dur(rem))));
            return;
        }

        long long amount = parse_ll(params[0]);
        if (amount < 1) { pm(src, "Minimum bet is 1 coin."); return; }
        if (u.coins < amount) { pm(src, rand_pick_str(s_msg_broke)); return; }

        u.coins   -= amount;
        u.last_bet = Anope::CurTime;

        // Build deck via a BJHand (reuses the same shuffle logic)
        BJHand tmp;
        bj_new_deck(tmp);

        std::vector<BJHand::Card> p_hole, d_hole, community;
        p_hole.push_back(bj_draw(tmp)); p_hole.push_back(bj_draw(tmp));
        d_hole.push_back(bj_draw(tmp)); d_hole.push_back(bj_draw(tmp));
        for (int i=0;i<5;i++) community.push_back(bj_draw(tmp));

        std::vector<BJHand::Card> p_all = p_hole; p_all.insert(p_all.end(), community.begin(), community.end());
        std::vector<BJHand::Card> d_all = d_hole; d_all.insert(d_all.end(), community.begin(), community.end());

        Anope::string p_name, d_name;
        HoldemScore p_score = holdem_best_hand(p_all, p_name);
        HoldemScore d_score = holdem_best_hand(d_all, d_name);

        // God mode: re-deal player hole until player wins
        if (_has_godmode(src.nc->display) && !(p_score > d_score))
        {
            for (int attempt = 0; attempt < 50; ++attempt)
            {
                BJHand tmp2; bj_new_deck(tmp2);
                p_hole.clear(); d_hole.clear();
                p_hole.push_back(bj_draw(tmp2)); p_hole.push_back(bj_draw(tmp2));
                d_hole.push_back(bj_draw(tmp2)); d_hole.push_back(bj_draw(tmp2));
                p_all = p_hole; p_all.insert(p_all.end(), community.begin(), community.end());
                d_all = d_hole; d_all.insert(d_all.end(), community.begin(), community.end());
                p_score = holdem_best_hand(p_all, p_name);
                d_score = holdem_best_hand(d_all, d_name);
                if (p_score > d_score) break;
            }
        }

        Anope::string hole_str = bj_hand_str(p_hole);
        Anope::string d_hole_str = bj_hand_str(d_hole);
        Anope::string comm_str = bj_hand_str(community);

        if (p_score == d_score)
        {
            u.coins += amount;
            announce(src, "\002HOLDEM\002 " + hole_str + " | Board: " + comm_str
                + " | Dealer: " + d_hole_str + " -- Both " + p_name + "! Push. "
                + src.GetNick() + " gets " + fmt_coins(amount) + " back. Balance: \002"
                + fmt_coins(u.coins) + "\002");
        }
        else if (p_score > d_score)
        {
            int mult = HOLDEM_PAYOUTS[p_score.rank];
            long long payout = amount * static_cast<long long>(mult);
            u.coins += payout;
            _update_highscore(u);
            announce(src, "\002HOLDEM\002 " + hole_str + " | Board: " + comm_str
                + " | Dealer: " + d_hole_str + " -- \002" + p_name + "\002 beats "
                + d_name + "! " + src.GetNick() + " wins " + fmt_coins(payout)
                + " (" + stringify(mult) + "x)! Balance: \002" + fmt_coins(u.coins) + "\002");
        }
        else
        {
            announce(src, "\002HOLDEM\002 " + hole_str + " | Board: " + comm_str
                + " | Dealer: " + d_hole_str + " -- Dealer's \002" + d_name + "\002 beats "
                + p_name + ". " + src.GetNick() + " lost " + fmt_coins(amount)
                + ". Balance: \002" + fmt_coins(u.coins) + "\002");
        }
    }
};

// ===========================================================================
// Module class
// ===========================================================================

class ModuleMugServ : public Module
{
    CommandMugCoins       cmd_coins;
    CommandMugBalance     cmd_balance;
    CommandMugGive        cmd_give;
    CommandMugMug         cmd_mug;
    CommandMugBet         cmd_bet;
    CommandMugBounty      cmd_bounty;
    CommandMugBounties    cmd_bounties;
    CommandMugJail        cmd_jail;
    CommandMugShop        cmd_shop;
    CommandMugBuy         cmd_buy;
    CommandMugInv         cmd_inv;
    CommandMugUse         cmd_use;
    CommandMugTop5        cmd_top5;
    CommandMugTop10       cmd_top10;
    CommandMugAdd         cmd_mugadd;
    CommandMugSet         cmd_mugset;
    CommandMugTake        cmd_mugtake;
    CommandMugReset       cmd_mugreset;
    CommandMugStats       cmd_mugstats;
    CommandMugHelp        cmd_help;
    CommandMugEnable      cmd_enable;
    CommandMugDisable     cmd_disable;
    // New commands
    CommandMugToggle      cmd_mugtoggle;
    CommandMugGodMode     cmd_godmode;
    CommandMugUncooldown  cmd_uncooldown;
    CommandMugHighScore   cmd_highscore;
    CommandMugRoll        cmd_roll;
    CommandMugPenny       cmd_penny;
    CommandMugDollar      cmd_dollar;
    CommandMugRoulette    cmd_roulette;
    CommandMugBJ          cmd_bj;
    CommandMugHit         cmd_hit;
    CommandMugStand       cmd_stand;
    CommandMugDD          cmd_dd;
    CommandMugHoldem      cmd_holdem;

    MugSaveTimer          save_timer;

public:
    ModuleMugServ(const Anope::string &modname, const Anope::string &creator)
        : Module(modname, creator, THIRD)
        , cmd_coins(this),      cmd_balance(this)
        , cmd_give(this),       cmd_mug(this),       cmd_bet(this)
        , cmd_bounty(this),     cmd_bounties(this),  cmd_jail(this)
        , cmd_shop(this),       cmd_buy(this),       cmd_inv(this)
        , cmd_use(this),        cmd_top5(this),      cmd_top10(this)
        , cmd_mugadd(this),     cmd_mugset(this),    cmd_mugtake(this)
        , cmd_mugreset(this),   cmd_mugstats(this),  cmd_help(this)
        , cmd_enable(this),     cmd_disable(this)
        , cmd_mugtoggle(this),  cmd_godmode(this),   cmd_uncooldown(this)
        , cmd_highscore(this)
        , cmd_roll(this),       cmd_penny(this),     cmd_dollar(this)
        , cmd_roulette(this)
        , cmd_bj(this),         cmd_hit(this),       cmd_stand(this)
        , cmd_dd(this),         cmd_holdem(this)
        , save_timer(this)
    {
        s_module = this;
        init_msgs();
        load_db();
    }

    ~ModuleMugServ()
    {
        save_db();
        s_module = NULL;
    }

    void OnReload(Configuration::Conf *conf) anope_override
    {
        Configuration::Block *block = conf->GetModule(this);

        const Anope::string cname = block->Get<Anope::string>("client", "MugServ");
        s_bot = BotInfo::Find(cname, true);
        if (!s_bot)
            throw ConfigException(this->name + ": no service bot named \"" + cname + "\". "
                "Add a 'service { nick = \"" + cname + "\"; ... }' block to services.conf.");

        {
            Anope::string ch_str = block->Get<Anope::string>("channels", "");
            if (!ch_str.empty())
            {
                spacesepstream ss(ch_str);
                Anope::string tok;
                while (ss.GetToken(tok))
                    s_channels.insert(tok.lower());
            }
        }

        s_cmd_prefix = block->Get<Anope::string>("cmd_prefix", "!");
        if (s_cmd_prefix.empty()) s_cmd_prefix = "!";

        s_admin_nicks.clear();
        Anope::string an_str = block->Get<Anope::string>("admin_nicks", "");
        if (!an_str.empty())
        {
            spacesepstream ss(an_str);
            Anope::string tok;
            while (ss.GetToken(tok))
                s_admin_nicks.push_back(tok.lower());
        }

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
        s_bot->SetCommand("HELP",       "mugserv/HELP");
        s_bot->SetCommand("COMMANDS",   "mugserv/HELP");
        s_bot->SetCommand("ENABLE",     "mugserv/ENABLE");
        s_bot->SetCommand("DISABLE",    "mugserv/DISABLE");
        s_bot->SetCommand("MUGTOGGLE",  "mugserv/MUGTOGGLE");
        s_bot->SetCommand("TOGGLE",     "mugserv/MUGTOGGLE");
        s_bot->SetCommand("GODMODE",    "mugserv/GODMODE");
        s_bot->SetCommand("UNCOOLDOWN", "mugserv/UNCOOLDOWN");
        s_bot->SetCommand("HIGHSCORE",  "mugserv/HIGHSCORE");
        s_bot->SetCommand("ROLL",       "mugserv/ROLL");
        s_bot->SetCommand("DICE",       "mugserv/ROLL");
        s_bot->SetCommand("PENNY",      "mugserv/PENNY");
        s_bot->SetCommand("DOLLAR",     "mugserv/DOLLAR");
        s_bot->SetCommand("ROULETTE",   "mugserv/ROULETTE");
        s_bot->SetCommand("BJ",         "mugserv/BJ");
        s_bot->SetCommand("BLACKJACK",  "mugserv/BJ");
        s_bot->SetCommand("HIT",        "mugserv/HIT");
        s_bot->SetCommand("STAND",      "mugserv/STAND");
        s_bot->SetCommand("DD",         "mugserv/DD");
        s_bot->SetCommand("HOLDEM",     "mugserv/HOLDEM");

        for (std::set<Anope::string>::const_iterator ci = s_channels.begin(); ci != s_channels.end(); ++ci)
        {
            Channel *c = Channel::Find(*ci);
            if (!c || !c->FindUser(s_bot))
                s_bot->Join(*ci);
        }
    }

    // Intercept channel messages for !command triggers.
    void OnPrivmsg(User *u, Channel *c, Anope::string &msg) anope_override
    {
        if (!u || !c || msg.empty())
            return;
        Anope::string chanlow = c->name.lower();
        if (!s_channels.count(chanlow))
            return;
        if (!_plugin_enabled(chanlow))
            return;
        if (!s_bot)
            return;
        if (msg.length() <= s_cmd_prefix.length())
            return;
        if (msg.substr(0, s_cmd_prefix.length()) != s_cmd_prefix)
            return;

        // Strip prefix; split verb and args.
        Anope::string rest = msg.substr(s_cmd_prefix.length());
        size_t sp = rest.find(' ');
        Anope::string verb  = (sp == Anope::string::npos) ? rest : rest.substr(0, sp);
        Anope::string argstr = (sp == Anope::string::npos) ? Anope::string("") : rest.substr(sp + 1);
        if (verb.empty()) return;

        Anope::string lverb = verb.lower();

        // Commands that must be used in PM only.
        if (lverb == "shop" || lverb == "buy" || lverb == "inv" || lverb == "inventory"
            || lverb == "use" || lverb == "mugadd" || lverb == "mugset" || lverb == "mugtake"
            || lverb == "mugreset" || lverb == "mugstats" || lverb == "enable" || lverb == "disable"
            || lverb == "godmode" || lverb == "uncooldown")
        {
            IRCD->SendPrivmsg(MessageSource(s_bot), c->name, "%s: Please /msg %s %s%s for that command.",
                u->nick.c_str(), s_bot->nick.c_str(), verb.upper().c_str(),
                argstr.empty() ? "" : (" " + argstr).c_str());
            return;
        }

        // NickServ gate.
        NickAlias *na = NickAlias::Find(u->nick);
        if (!na || !na->nc)
        {
            IRCD->SendPrivmsg(MessageSource(s_bot), c->name, "%s: You must be identified with NickServ to play MugServ.",
                u->nick.c_str());
            return;
        }

        NickCore *nc = na->nc;
        Anope::string acct_key = nc->display.lower();

        // Auto-enroll.
        if (s_users.find(acct_key) == s_users.end())
        {
            MugUser nu;
            nu.account = acct_key;
            nu.nick    = u->nick;
            s_users[acct_key] = nu;
            IRCD->SendPrivmsg(MessageSource(s_bot), c->name, "%s: Welcome to MugServ! You've been enrolled. Type %shelp for commands.",
                u->nick.c_str(), s_cmd_prefix.c_str());
        }
        else
        {
            s_users[acct_key].nick = u->nick;
        }

        // Parse params.
        std::vector<Anope::string> params;
        if (!argstr.empty())
        {
            spacesepstream ss(argstr);
            Anope::string tok;
            while (ss.GetToken(tok))
                params.push_back(tok);
        }

        // Resolve canonical verb name.
        Anope::string svcmd = verb.upper();
        if (svcmd == "ROB")       svcmd = "MUG";
        if (svcmd == "BAL")       svcmd = "BALANCE";
        if (svcmd == "INVENTORY") svcmd = "INV";
        if (svcmd == "COMMANDS")  svcmd = "HELP";
        if (svcmd == "TOGGLE")    svcmd = "MUGTOGGLE";
        if (svcmd == "DICE")      svcmd = "ROLL";
        if (svcmd == "BLACKJACK") svcmd = "BJ";

        CommandInfo::map::iterator ci = s_bot->commands.find(svcmd);
        if (ci == s_bot->commands.end()) return;

        Command *cmd = static_cast<Command *>(Service::FindService("Command", ci->second.name));
        if (!cmd) return;

        if (params.size() < cmd->min_params)
        {
            IRCD->SendPrivmsg(MessageSource(s_bot), c->name, "%s: Not enough parameters for %s%s. /msg %s %s for usage.",
                u->nick.c_str(), s_cmd_prefix.c_str(),
                svcmd.lower().c_str(), s_bot->nick.c_str(), svcmd.c_str());
            return;
        }

        // Set channel context so announce() routes to this channel.
        s_current_chan = chanlow;
        CommandSource fake_src(u->nick, u, nc, NULL, s_bot);
        cmd->Execute(fake_src, params);
        s_current_chan = "";
    }

    void OnSaveDatabase() anope_override
    {
        save_db();
    }
};

MODULE_INIT(ModuleMugServ)
