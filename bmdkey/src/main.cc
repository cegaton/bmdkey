#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <exception>
#include <vector>
#include <set>
#include <algorithm>
#include <ranges>
#include <map>
#include <chrono>
#include <string>
#include <fstream>
#include <sstream>
#include <optional>
#include <fcntl.h>
#include <linux/uhid.h>
#include <hidapi.h>
#include <fmt/format.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <X11/XKBlib.h>
#include <X11/extensions/XTest.h>

#define MAX_STR 255
#define WHEEL_STEP 12000
#define TIMEOUT_MS (60 * 1000)
/* How long recv() waits before we go round the loop to check for config
 * changes. Short enough that saving the config feels instant. */
#define POLL_MS 250

static Display* display;
static Window window;
static std::set<uint16_t> currentKeyboardState;
static int minKeycode;
static int maxKeycode;
static volatile sig_atomic_t running = true;
static volatile sig_atomic_t reloadRequested = false;
static bool learnMode = false;
static bool traceMode = false;

void signalHandler(int)
{
    running = false;
}

void hupHandler(int)
{
    reloadRequested = true;
}

/* ------------------------------------------------------------------------ */
/* Speed Editor key codes, named as printed on the keys.                     */
/* ------------------------------------------------------------------------ */

static const std::vector<std::pair<std::string, int>> KEY_NAMES = {
    {"SMART_INSRT",  0x01},
    {"APPEND",       0x02},
    {"RIPL_OWR",     0x03},
    {"CLOSE_UP",     0x04},
    {"PLACE_ON_TOP", 0x05},
    {"SRC_OWR",      0x06},
    {"IN",           0x07},
    {"OUT",          0x08},
    {"TRIM_IN",      0x09},
    {"TRIM_OUT",     0x0a},
    {"ROLL",         0x0b},
    {"SLIP_SRC",     0x0c},
    {"SLIP_DEST",    0x0d},
    {"TRANS_DUR",    0x0e},
    {"CUT",          0x0f},
    {"DIS",          0x10},
    {"SMTH_CUT",     0x11},
    {"SOURCE",       0x1a},
    {"TIMELINE",     0x1b},
    {"SHTL",         0x1c},
    {"JOG",          0x1d},
    {"SCRL",         0x1e},
    {"SYNC_BIN",     0x1f},
    {"TRANS",        0x22},
    {"VIDEO_ONLY",   0x25},
    {"AUDIO_ONLY",   0x26},
    {"RIPL_DEL",     0x2b},
    {"AUDIO_LEVEL",  0x2c},
    {"FULL_VIEW",    0x2d},
    {"SNAP",         0x2e},
    {"SPLIT",        0x2f},
    {"LIVE_OWR",     0x30},
    {"ESC",          0x31},
    {"CAM1",         0x33},
    {"CAM2",         0x34},
    {"CAM3",         0x35},
    {"CAM4",         0x36},
    {"CAM5",         0x37},
    {"CAM6",         0x38},
    {"CAM7",         0x39},
    {"CAM8",         0x3a},
    {"CAM9",         0x3b},
    {"STOP_PLAY",    0x3c},
};

static const int KEY_SHTL = 0x1c;
static const int KEY_JOG = 0x1d;
static const int KEY_SCRL = 0x1e;

enum WheelMode
{
    MODE_JOG = 0,
    MODE_SHTL = 1,
    MODE_SCRL = 2,
    MODE_COUNT = 3
};

static const char* MODE_NAMES[MODE_COUNT] = {"jog", "shtl", "scrl"};
/* Bits for the LEDs inside the SHTL/JOG/SCRL keys (HID report 4). */
static const uint8_t MODE_LEDS[MODE_COUNT] = {1 << 0, 1 << 1, 1 << 2};

static std::string trim(const std::string& s)
{
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos)
        return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

static std::string lower(std::string s)
{
    for (auto& c : s)
        c = tolower((unsigned char)c);
    return s;
}

static std::string keyName(int code)
{
    for (auto& [name, c] : KEY_NAMES)
        if (c == code)
            return name;
    return fmt::format("0x{:02x}", code);
}

static std::optional<int> keyCodeFromName(const std::string& raw)
{
    std::string s;
    for (char c : raw)
    {
        if ((c == ' ') || (c == '-') || (c == '/'))
            c = '_';
        s += toupper((unsigned char)c);
    }
    /* Allow the slash spelling printed on the keys: RIPL O/WR -> RIPL_O_WR */
    static const std::map<std::string, std::string> aliases = {
        {"RIPL_O_WR", "RIPL_OWR"},
        {"SRC_O_WR",  "SRC_OWR" },
        {"LIVE_O_WR", "LIVE_OWR"},
        {"PLAY",      "STOP_PLAY"},
        {"STOP",      "STOP_PLAY"},
    };
    if (auto it = aliases.find(s); it != aliases.end())
        s = it->second;

    for (auto& [name, c] : KEY_NAMES)
        if (name == s)
            return c;

    /* Raw codes for anything not in the table, e.g. 0x2a. */
    if ((s.size() > 2) && (s[0] == '0') && (s[1] == 'X'))
    {
        char* end;
        long v = strtol(s.c_str() + 2, &end, 16);
        if (*end == 0)
            return (int)v;
    }
    return std::nullopt;
}

/* ------------------------------------------------------------------------ */
/* Actions                                                                   */
/* ------------------------------------------------------------------------ */

enum class StepType
{
    Key,
    Button,
    Exec
};

struct Step
{
    StepType type = StepType::Key;
    std::vector<KeySym> mods;
    KeySym keysym = NoSymbol;
    unsigned button = 0;
    std::string command;
};

struct Action
{
    std::vector<Step> steps;
    std::string text; /* as written in the config, for log output */

    bool empty() const
    {
        return steps.empty();
    }

    /* A single key combo or mouse click follows the physical key: held while
     * the key is held. Everything else (sequences, scroll, exec) fires once on
     * press. */
    bool holdable() const
    {
        if (steps.size() != 1)
            return false;
        const Step& s = steps[0];
        if (s.type == StepType::Key)
            return true;
        if (s.type == StepType::Button)
            return (s.button < 4) || (s.button > 7);
        return false;
    }
};

struct WheelSettings
{
    int step = WHEEL_STEP;
    Action left;
    Action right;
};

struct Config
{
    std::map<int, Action> keys;
    WheelSettings wheel[MODE_COUNT];
    int defaultMode = MODE_JOG;
    bool keyLeds = true;
    int sequenceDelayMs = 10;
    /* Resolve pass-through: keys listed here are taken over while Resolve
     * has the virtual Speed Editor open; every other key stays native. */
    bool resolvePassthrough = true;
    std::map<int, Action> resolveKeys;
};

static Config config;
static std::string configPath;

static KeySym lookupKeysym(const std::string& name)
{
    static const std::map<std::string, std::string> aliases = {
        {"esc",       "Escape"   },
        {"escape",    "Escape"   },
        {"enter",     "Return"   },
        {"return",    "Return"   },
        {"del",       "Delete"   },
        {"delete",    "Delete"   },
        {"ins",       "Insert"   },
        {"insert",    "Insert"   },
        {"bksp",      "BackSpace"},
        {"backspace", "BackSpace"},
        {"pgup",      "Prior"    },
        {"pageup",    "Prior"    },
        {"pgdn",      "Next"     },
        {"pagedown",  "Next"     },
        {"tab",       "Tab"      },
        {"left",      "Left"     },
        {"right",     "Right"    },
        {"up",        "Up"       },
        {"down",      "Down"     },
        {"home",      "Home"     },
        {"end",       "End"      },
    };

    if (auto it = aliases.find(lower(name)); it != aliases.end())
        return XStringToKeysym(it->second.c_str());

    KeySym ks = XStringToKeysym(name.c_str());
    if (ks != NoSymbol)
        return ks;

    /* f5 -> F5, return -> Return, etc. */
    std::string cap = lower(name);
    cap[0] = toupper((unsigned char)cap[0]);
    ks = XStringToKeysym(cap.c_str());
    if (ks != NoSymbol)
        return ks;

    return XStringToKeysym(lower(name).c_str());
}

static std::optional<KeySym> lookupModifier(const std::string& name)
{
    std::string n = lower(name);
    if ((n == "ctrl") || (n == "control"))
        return XK_Control_L;
    if (n == "shift")
        return XK_Shift_L;
    if (n == "alt")
        return XK_Alt_L;
    if ((n == "super") || (n == "win") || (n == "cmd"))
        return XK_Super_L;
    if (n == "altgr")
        return XK_ISO_Level3_Shift;
    return std::nullopt;
}

static std::optional<unsigned> lookupButton(const std::string& name)
{
    static const std::map<std::string, unsigned> buttons = {
        {"click_left",   1},
        {"mouse_left",   1},
        {"click_middle", 2},
        {"mouse_middle", 2},
        {"click_right",  3},
        {"mouse_right",  3},
        {"scroll_up",    4},
        {"scroll_down",  5},
        {"scroll_left",  6},
        {"scroll_right", 7},
        {"mouse_back",   8},
        {"mouse_forward", 9},
    };
    std::string n = lower(name);
    if (auto it = buttons.find(n); it != buttons.end())
        return it->second;
    if (n.rfind("button", 0) == 0)
    {
        char* end;
        long v = strtol(n.c_str() + 6, &end, 10);
        if ((*end == 0) && (v >= 1) && (v <= 32))
            return (unsigned)v;
    }
    return std::nullopt;
}

static std::vector<std::string> split(const std::string& s, char sep)
{
    std::vector<std::string> out;
    std::string cur;
    for (char c : s)
    {
        if (c == sep)
        {
            out.push_back(cur);
            cur.clear();
        }
        else
            cur += c;
    }
    out.push_back(cur);
    return out;
}

/* Syntax:
 *   none                     do nothing
 *   ctrl+shift+z             key combo (held while the key is held)
 *   scroll_up, click_left    mouse; modifiers allowed (ctrl+scroll_up)
 *   ctrl+c, ctrl+v           comma-separated sequence (fires on press)
 *   exec:some command        run via /bin/sh (fires on press)
 */
static std::optional<Action> parseAction(
    const std::string& raw, std::string& error)
{
    Action action;
    action.text = trim(raw);
    std::string value = action.text;

    if (value.empty() || (lower(value) == "none"))
        return action;

    if (lower(value.substr(0, 5)) == "exec:")
    {
        Step step;
        step.type = StepType::Exec;
        step.command = trim(value.substr(5));
        if (step.command.empty())
        {
            error = "exec: needs a command";
            return std::nullopt;
        }
        action.steps.push_back(step);
        return action;
    }

    for (auto& part : split(value, ','))
    {
        std::string combo = trim(part);
        if (combo.empty())
        {
            error = "empty step in sequence";
            return std::nullopt;
        }

        auto tokens = split(combo, '+');
        Step step;
        for (size_t i = 0; i < tokens.size(); i++)
        {
            std::string tok = trim(tokens[i]);
            if (tok.empty())
            {
                error = fmt::format(
                    "bad combo '{}' (write 'plus' for the + key)", combo);
                return std::nullopt;
            }

            bool last = (i == tokens.size() - 1);
            if (!last)
            {
                auto mod = lookupModifier(tok);
                if (!mod)
                {
                    error = fmt::format("unknown modifier '{}'", tok);
                    return std::nullopt;
                }
                step.mods.push_back(*mod);
                continue;
            }

            if (auto button = lookupButton(tok))
            {
                step.type = StepType::Button;
                step.button = *button;
            }
            else
            {
                KeySym ks = lookupKeysym(tok);
                if (ks == NoSymbol)
                {
                    error = fmt::format("unknown key '{}'", tok);
                    return std::nullopt;
                }
                step.type = StepType::Key;
                step.keysym = ks;
            }
        }
        action.steps.push_back(step);
    }
    return action;
}

/* ------------------------------------------------------------------------ */
/* Config file                                                               */
/* ------------------------------------------------------------------------ */

static const char DEFAULT_CONFIG[] = R"CONF(# bmdkey configuration
#
# This file is reloaded automatically whenever you save it, so there's no need
# to restart bmdkey. Send it SIGHUP to force a reload.
#
# ACTIONS (the value after the '=')
#   none                      do nothing
#   i   F13   space   Left    a single key (X keysym names; case matters for
#                             letters: 'B' sends shift+b)
#   ctrl+shift+z              key combo. Modifiers: ctrl shift alt super altgr
#   scroll_up  scroll_down    mouse wheel (also scroll_left / scroll_right)
#   click_left click_right    mouse buttons (also click_middle, buttonN)
#   ctrl+scroll_up            modifiers work with mouse actions too
#   ctrl+c, ctrl+v            a sequence: comma-separated, played in order
#   exec:notify-send hello    run a shell command
#
# A single key combo or mouse click is held for as long as you hold the Speed
# Editor key (so auto-repeat and click-and-drag work). Sequences, scrolls and
# exec: fire once when the key is pressed.
#
# Comments must be on their own line, starting with # or ;
#
# KEY NAMES (as printed on the Speed Editor; run 'bmdkey --learn' to see them):
#   SMART_INSRT APPEND RIPL_OWR CLOSE_UP PLACE_ON_TOP SRC_OWR
#   IN OUT TRIM_IN TRIM_OUT ROLL SLIP_SRC SLIP_DEST TRANS_DUR
#   CUT DIS SMTH_CUT SOURCE TIMELINE
#   ESC SYNC_BIN AUDIO_LEVEL FULL_VIEW TRANS SPLIT SNAP RIPL_DEL
#   CAM1 .. CAM9 LIVE_OWR VIDEO_ONLY AUDIO_ONLY STOP_PLAY
#
# SHTL, JOG and SCRL are reserved: they switch the wheel between the three
# [wheel.*] sections below, and light up to show which one is active.
#
# DAVINCI RESOLVE: while Resolve is running, bmdkey hands it a virtual Speed
# Editor, so the keys, wheel and lights work natively in Resolve. [keys] and
# [wheel.*] are then ignored; only keys listed in [resolve.keys] are taken
# over. Start bmdkey before Resolve.

[general]
# Wheel mode at startup: jog, shtl or scrl
default_mode = jog
# Light the LEDs on the keys that have them (CUT, DIS, CAM1..9, ...)
key_leds = on
# Pause between the steps of a sequence, in milliseconds
sequence_delay_ms = 10
# Pass the Speed Editor through to DaVinci Resolve (needs the udev rules;
# takes effect when bmdkey restarts)
resolve_passthrough = on

[keys]
SMART_INSRT  = F1
APPEND       = F2
RIPL_OWR     = F3
CLOSE_UP     = F4
PLACE_ON_TOP = F5
SRC_OWR      = F6
IN           = i
OUT          = o
TRIM_IN      = F9
TRIM_OUT     = F10
ROLL         = F11
SLIP_SRC     = F12
SLIP_DEST    = F13
TRANS_DUR    = F14
CUT          = F15
DIS          = F16
SMTH_CUT     = F17
SOURCE       = F18
TIMELINE     = F19

ESC          = shift+F1
SYNC_BIN     = shift+F2
AUDIO_LEVEL  = shift+F3
FULL_VIEW    = shift+F4
TRANS        = shift+F5
SPLIT        = shift+F6
SNAP         = shift+F7
RIPL_DEL     = shift+F8

CAM1         = shift+F10
CAM2         = shift+F11
CAM3         = shift+F12
CAM4         = shift+F13
CAM5         = shift+F14
CAM6         = shift+F15
CAM7         = shift+F16
CAM8         = shift+F17
CAM9         = shift+F18
LIVE_OWR     = shift+F19
VIDEO_ONLY   = shift+F20
AUDIO_ONLY   = shift+F21
STOP_PLAY    = space

# Each wheel mode: 'step' is how far the wheel turns (in device units) per
# action; smaller = more actions per turn. 12000 is roughly 30 per revolution.

[wheel.jog]
step  = 12000
left  = Left
right = Right

[wheel.shtl]
step  = 3000
left  = Left
right = Right

[wheel.scrl]
step  = 6000
left  = scroll_up
right = scroll_down

# Keys to take over inside DaVinci Resolve. Anything not listed here keeps its
# normal Resolve function. Same action syntax as [keys]. Examples:
[resolve.keys]
# CAM9      = ctrl+shift+F9
# SYNC_BIN  = exec:notify-send "Sync bin pressed"
)CONF";

static std::optional<bool> parseBool(const std::string& v)
{
    std::string l = lower(v);
    if ((l == "on") || (l == "yes") || (l == "true") || (l == "1"))
        return true;
    if ((l == "off") || (l == "no") || (l == "false") || (l == "0"))
        return false;
    return std::nullopt;
}

static std::optional<int> parseMode(const std::string& v)
{
    std::string l = lower(v);
    for (int i = 0; i < MODE_COUNT; i++)
        if (l == MODE_NAMES[i])
            return i;
    return std::nullopt;
}

static std::optional<int> parseInt(const std::string& v)
{
    char* end;
    long n = strtol(v.c_str(), &end, 10);
    if (v.empty() || *end)
        return std::nullopt;
    return (int)n;
}

/* Returns false only if the file can't be read at all. Bad lines are reported
 * and skipped; everything else still loads. */
static bool loadConfig(const std::string& path, Config& out)
{
    std::ifstream f(path);
    if (!f)
        return false;

    Config cfg;
    /* The config file is the whole truth: start the wheel modes empty so a
     * section you delete really does nothing. */
    for (auto& w : cfg.wheel)
        w = WheelSettings();

    std::string section;
    std::string line;
    int lineno = 0;
    int problems = 0;
    auto warn = [&](const std::string& msg)
    {
        fmt::print(stderr, "{}:{}: {}\n", path, lineno, msg);
        problems++;
    };

    while (std::getline(f, line))
    {
        lineno++;
        line = trim(line);
        if (line.empty() || (line[0] == '#') || (line[0] == ';'))
            continue;

        if (line[0] == '[')
        {
            if (line.back() != ']')
            {
                warn("bad section header");
                continue;
            }
            section = lower(trim(line.substr(1, line.size() - 2)));
            continue;
        }

        size_t eq = line.find('=');
        if (eq == std::string::npos)
        {
            warn("expected 'name = value'");
            continue;
        }
        std::string key = trim(line.substr(0, eq));
        std::string value = trim(line.substr(eq + 1));

        if (section == "general")
        {
            std::string k = lower(key);
            if (k == "default_mode")
            {
                if (auto m = parseMode(value))
                    cfg.defaultMode = *m;
                else
                    warn("default_mode must be jog, shtl or scrl");
            }
            else if (k == "key_leds")
            {
                if (auto b = parseBool(value))
                    cfg.keyLeds = *b;
                else
                    warn("key_leds must be on or off");
            }
            else if (k == "sequence_delay_ms")
            {
                auto n = parseInt(value);
                if (n && (*n >= 0))
                    cfg.sequenceDelayMs = *n;
                else
                    warn("sequence_delay_ms must be a number >= 0");
            }
            else if (k == "resolve_passthrough")
            {
                if (auto b = parseBool(value))
                    cfg.resolvePassthrough = *b;
                else
                    warn("resolve_passthrough must be on or off");
            }
            else
                warn(fmt::format("unknown setting '{}'", key));
        }
        else if ((section == "keys") || (section == "resolve.keys"))
        {
            auto code = keyCodeFromName(key);
            if (!code)
            {
                warn(fmt::format("unknown Speed Editor key '{}'", key));
                continue;
            }
            if ((*code == KEY_SHTL) || (*code == KEY_JOG) ||
                (*code == KEY_SCRL))
            {
                warn(fmt::format("{} is reserved for the wheel modes", key));
                continue;
            }
            std::string error;
            auto action = parseAction(value, error);
            if (!action)
            {
                warn(fmt::format("{}: {}", key, error));
                continue;
            }
            if (section == "keys")
                cfg.keys[*code] = *action;
            else
                cfg.resolveKeys[*code] = *action;
        }
        else if (section.rfind("wheel.", 0) == 0)
        {
            auto mode = parseMode(section.substr(6));
            if (!mode)
            {
                warn(fmt::format("unknown wheel mode [{}]", section));
                continue;
            }
            WheelSettings& w = cfg.wheel[*mode];
            std::string k = lower(key);
            if (k == "step")
            {
                auto n = parseInt(value);
                if (n && (*n > 0))
                    w.step = *n;
                else
                    warn("step must be a number > 0");
            }
            else if ((k == "left") || (k == "right"))
            {
                std::string error;
                auto action = parseAction(value, error);
                if (!action)
                    warn(fmt::format("{}: {}", key, error));
                else if (k == "left")
                    w.left = *action;
                else
                    w.right = *action;
            }
            else
                warn(fmt::format("unknown wheel setting '{}'", key));
        }
        else if (section.empty())
            warn("setting outside any [section]");
        else
            warn(fmt::format("unknown section [{}]", section));
    }

    fmt::print("Loaded {} ({} key mappings, {} for Resolve{})\n",
        path,
        cfg.keys.size(),
        cfg.resolveKeys.size(),
        problems ? fmt::format(", {} problem(s) skipped", problems) : "");
    out = cfg;
    return true;
}

static std::string defaultConfigPath()
{
    const char* xdg = getenv("XDG_CONFIG_HOME");
    std::string base;
    if (xdg && *xdg)
        base = xdg;
    else
    {
        const char* home = getenv("HOME");
        base = std::string(home ? home : ".") + "/.config";
    }
    return base + "/bmdkey/bmdkey.conf";
}

static void mkdirs(const std::string& dir)
{
    for (size_t i = 1; i <= dir.size(); i++)
        if ((i == dir.size()) || (dir[i] == '/'))
            mkdir(dir.substr(0, i).c_str(), 0755);
}

static void ensureConfigExists(const std::string& path)
{
    struct stat st;
    if (stat(path.c_str(), &st) == 0)
        return;

    size_t slash = path.rfind('/');
    if (slash != std::string::npos)
        mkdirs(path.substr(0, slash));

    std::ofstream f(path);
    if (!f)
    {
        fmt::print(stderr, "Can't create {}: {}\n", path, strerror(errno));
        return;
    }
    f << DEFAULT_CONFIG;
    fmt::print("Created default config at {}\n", path);
}

struct FileStamp
{
    bool exists = false;
    struct timespec mtime = {};
    off_t size = 0;

    bool operator==(const FileStamp& o) const
    {
        return (exists == o.exists) && (mtime.tv_sec == o.mtime.tv_sec) &&
               (mtime.tv_nsec == o.mtime.tv_nsec) && (size == o.size);
    }
};

static FileStamp stampOf(const std::string& path)
{
    FileStamp s;
    struct stat st;
    if (stat(path.c_str(), &st) == 0)
    {
        s.exists = true;
        s.mtime = st.st_mtim;
        s.size = st.st_size;
    }
    return s;
}

/* ------------------------------------------------------------------------ */
/* Sending events to X                                                       */
/* ------------------------------------------------------------------------ */

static std::vector<KeyCode> spareKeycodes;
static size_t nextSpare = 0;
static std::map<KeySym, KeyCode> remapped;
static std::map<KeyCode, int> modifierRefs;

/* Keycodes with nothing on them, which we can borrow for keysyms the current
 * layout doesn't have (F13-F24 usually). */
static void findSpareKeycodes()
{
    int perCode;
    int count = maxKeycode - minKeycode + 1;
    KeySym* syms = XGetKeyboardMapping(display, minKeycode, count, &perCode);
    for (int code = maxKeycode; code >= minKeycode; code--)
    {
        bool empty = true;
        for (int i = 0; i < perCode; i++)
            if (syms[(code - minKeycode) * perCode + i] != NoSymbol)
                empty = false;
        if (empty)
            spareKeycodes.push_back(code);
        if (spareKeycodes.size() >= 32)
            break;
    }
    XFree(syms);
    if (spareKeycodes.empty())
        spareKeycodes.push_back(maxKeycode - 1); /* what the original did */
}

struct ResolvedKey
{
    KeyCode code = 0;
    bool needsShift = false;
};

static ResolvedKey resolveKeysym(KeySym keysym)
{
    ResolvedKey r;
    if (auto it = remapped.find(keysym); it != remapped.end())
    {
        r.code = it->second;
        return r;
    }

    r.code = XKeysymToKeycode(display, keysym);
    if (r.code)
    {
        /* e.g. 'B' or 'exclam' live on the shifted level. */
        r.needsShift = (XkbKeycodeToKeysym(display, r.code, 0, 0) != keysym) &&
                       (XkbKeycodeToKeysym(display, r.code, 0, 1) == keysym);
        return r;
    }

    KeyCode code = spareKeycodes[nextSpare];
    nextSpare = (nextSpare + 1) % spareKeycodes.size();
    std::erase_if(remapped, [&](auto& kv) { return kv.second == code; });

    KeySym keysyms[] = {keysym, keysym};
    XChangeKeyboardMapping(display, code, 2, keysyms, 1);
    XSync(display, False);
    remapped[keysym] = code;
    r.code = code;
    return r;
}

static void modifierDown(KeyCode code)
{
    if (modifierRefs[code]++ == 0)
        XTestFakeKeyEvent(display, code, True, CurrentTime);
}

static void modifierUp(KeyCode code)
{
    int& refs = modifierRefs[code];
    if ((refs > 0) && (--refs == 0))
        XTestFakeKeyEvent(display, code, False, CurrentTime);
}

static void runCommand(const std::string& command)
{
    /* Double fork so the command is reparented to init and we never have to
     * reap it. */
    pid_t pid = fork();
    if (pid == 0)
    {
        if (fork() == 0)
        {
            setsid();
            signal(SIGINT, SIG_DFL);
            signal(SIGTERM, SIG_DFL);
            signal(SIGHUP, SIG_DFL);
            for (int fd = 3; fd < 1024; fd++)
                close(fd);
            execl("/bin/sh", "sh", "-c", command.c_str(), (char*)nullptr);
            _exit(127);
        }
        _exit(0);
    }
    if (pid > 0)
        waitpid(pid, nullptr, 0);
}

/* What one step currently has pressed, so it can be released later. */
struct Pressed
{
    std::vector<KeyCode> mods;
    KeyCode key = 0;
    unsigned button = 0;
};

static Pressed pressStep(const Step& step)
{
    Pressed p;
    if (step.type == StepType::Exec)
    {
        runCommand(step.command);
        return p;
    }

    for (KeySym mod : step.mods)
    {
        KeyCode code = resolveKeysym(mod).code;
        if (code)
        {
            modifierDown(code);
            p.mods.push_back(code);
        }
    }

    if (step.type == StepType::Key)
    {
        ResolvedKey r = resolveKeysym(step.keysym);
        if (r.needsShift)
        {
            KeyCode shift = XKeysymToKeycode(display, XK_Shift_L);
            modifierDown(shift);
            p.mods.push_back(shift);
        }
        XTestFakeKeyEvent(display, r.code, True, CurrentTime);
        p.key = r.code;
    }
    else
    {
        XTestFakeButtonEvent(display, step.button, True, CurrentTime);
        p.button = step.button;
    }
    XSync(display, False);
    return p;
}

static void releasePressed(const Pressed& p)
{
    if (p.key)
        XTestFakeKeyEvent(display, p.key, False, CurrentTime);
    if (p.button)
        XTestFakeButtonEvent(display, p.button, False, CurrentTime);
    for (auto it = p.mods.rbegin(); it != p.mods.rend(); ++it)
        modifierUp(*it);
    XSync(display, False);
}

/* Fire-once: press and release every step in order. */
static void tapAction(const Action& action)
{
    for (size_t i = 0; i < action.steps.size(); i++)
    {
        if (i && config.sequenceDelayMs)
            usleep(config.sequenceDelayMs * 1000);
        releasePressed(pressStep(action.steps[i]));
    }
}

/* ------------------------------------------------------------------------ */
/* Device state                                                              */
/* ------------------------------------------------------------------------ */

static std::map<uint16_t, Pressed> heldKeys;
static int wheelMode = MODE_JOG;
static int32_t sentWheelPosition = 0;
static int32_t wheelPosition = 0;

static void releaseAll()
{
    for (auto& [k, p] : heldKeys)
        releasePressed(p);
    heldKeys.clear();
    for (auto& [code, refs] : modifierRefs)
        if (refs > 0)
            XTestFakeKeyEvent(display, code, False, CurrentTime);
    modifierRefs.clear();
    if (display)
        XSync(display, False);
}

static void checkerror(int res)
{
    if (res == -1)
    {
        printf("error\n");
        exit(1);
    }
}

static uint64_t rol8(uint64_t v)
{
    return ((v << 56) | (v >> 8)) & 0xffffffffffffffff;
}

static uint64_t rol8n(uint64_t v, int n)
{
    while (n--)
        v = rol8(v);
    return v;
}

/* Authentication code borrowed from https://github.com/smunaut/blackmagic-misc.
 */
static uint64_t calculateKeyboardResponse(uint64_t challenge)
{
    static const uint64_t auth_even_tbl[] = {
        0x3ae1206f97c10bc8,
        0x2a9ab32bebf244c6,
        0x20a6f8b8df9adf0a,
        0xaf80ece52cfc1719,
        0xec2ee2f7414fd151,
        0xb055adfd73344a15,
        0xa63d2e3059001187,
        0x751bf623f42e0dde,
    };
    static const uint64_t auth_odd_tbl[] = {
        0x3e22b34f502e7fde,
        0x24656b981875ab1c,
        0xa17f3456df7bf8c3,
        0x6df72e1941aef698,
        0x72226f011e66ab94,
        0x3831a3c606296b42,
        0xfd7ff81881332c89,
        0x61a3f6474ff236c6,
    };
    static const uint64_t mask = 0xa79a63f585d37bf0;

    uint64_t n = challenge & 7;
    uint64_t v = rol8n(challenge, n);

    uint64_t k;
    if ((v & 1) == ((0x78 >> n) & 1))
        k = auth_even_tbl[n];
    else
    {
        v = v ^ rol8(v);
        k = auth_odd_tbl[n];
    }

    return v ^ (rol8(v) & mask) ^ k;
}

static uint64_t getInt16(const uint8_t* p)
{
    return ((uint64_t)p[0] << 0) | ((uint64_t)p[1] << 8);
}

static uint32_t getInt32(const uint8_t* p)
{
    return ((uint32_t)getInt16(p) << 0) | ((uint32_t)getInt16(p + 2) << 16);
}

static uint64_t getInt64(const uint8_t* p)
{
    return ((uint64_t)getInt32(p) << 0) | ((uint64_t)getInt32(p + 4) << 32);
}

static void putInt64(uint8_t* p, uint64_t value)
{
    p[0] = value >> 0;
    p[1] = value >> 8;
    p[2] = value >> 16;
    p[3] = value >> 24;
    p[4] = value >> 32;
    p[5] = value >> 40;
    p[6] = value >> 48;
    p[7] = value >> 56;
}

class HidException : public std::exception
{
public:
    HidException(const std::string& s): _message(s) {}

    const char* what() const noexcept override
    {
        return _message.c_str();
    }

private:
    std::string _message;
};

class TimeoutException : public std::exception
{
};

class HidDevice
{
public:
    HidDevice(uint16_t product, uint16_t vendor)
    {
        for (;;)
        {
            _handle = hid_open(product, vendor, nullptr);
            if (_handle)
                break;
            if (!running)
                throw HidException("interrupted");

            fmt::print("Waiting...\n");
            sleep(1);
        }

        hid_set_nonblocking(_handle, false);
    }

    ~HidDevice()
    {
        if (_handle)
            hid_close(_handle);
    }

    void sendFeatureReport(const std::vector<uint8_t>& report)
    {
        checkError(hid_send_feature_report(_handle, &report[0], report.size()));
    }

    std::vector<uint8_t> recvFeatureReport(int id, int length)
    {
        std::vector<uint8_t> data(10);
        data.at(0) = id;

        int res = hid_get_feature_report(_handle, &data[0], length);
        checkError(res);
        data.resize(res);

        return data;
    }

    void send(const std::vector<uint8_t>& data)
    {
        checkError(hid_write(_handle, &data[0], data.size()));
    }

    std::vector<uint8_t> recv(int msTimeout)
    {
        std::vector<uint8_t> data(64);

        int res = hid_read_timeout(_handle, &data[0], data.size(), msTimeout);
        if (res == 0)
            throw TimeoutException();

        checkError(res);
        data.resize(res);

        return data;
    }

    /* Raw calls for Resolve pass-through. They return -1 on failure instead
     * of throwing, so one bad request from Resolve can't drop the device. */
    int getFeatureRaw(uint8_t* buf, size_t len)
    {
        return hid_get_feature_report(_handle, buf, len);
    }

    int sendFeatureRaw(const uint8_t* buf, size_t len)
    {
        return hid_send_feature_report(_handle, buf, len);
    }

    int writeRaw(const uint8_t* buf, size_t len)
    {
        return hid_write(_handle, buf, len);
    }

    std::string lastError()
    {
        const wchar_t* w = hid_error(_handle);
        if (!w)
            return "unknown error";
        char buf[256];
        size_t n = wcstombs(buf, w, sizeof(buf) - 1);
        if (n == (size_t)-1)
            return "unknown error";
        buf[n] = 0;
        return buf;
    }

private:
    void checkError(int res)
    {
        if (res == -1)
            throw HidException(fmt::format("failed: {}", strerror(errno)));
    }

    hid_device* _handle;
};

static void authenticate(HidDevice& device)
{
    device.sendFeatureReport({6, 0, 0, 0, 0, 0, 0, 0, 0, 0});
    auto challengeFromKeyboard = device.recvFeatureReport(6, 10);

    device.sendFeatureReport({6, 1, 0, 0, 0, 0, 0, 0, 0, 0});
    device.recvFeatureReport(6, 10);

    std::vector<uint8_t> responseToKeyboard = {6, 3, 0, 0, 0, 0, 0, 0, 0, 0};
    putInt64(&responseToKeyboard[2],
        calculateKeyboardResponse(getInt64(&challengeFromKeyboard[2])));
    device.sendFeatureReport(responseToKeyboard);

    auto result = device.recvFeatureReport(6, 10);
    if ((result.at(0) != 6) || (result.at(1) != 4))
        throw HidException("unable to authenticate keyboard");
    fmt::print("Authenticated\n");
}

/* LEDs are cosmetic: never let a failure here take the daemon down. */
static void sendLeds(HidDevice& device, const std::vector<uint8_t>& report)
{
    try
    {
        device.send(report);
    }
    catch (const HidException& e)
    {
        fmt::print(stderr, "LED update failed: {}\n", e.what());
    }
}

static void updateKeyLeds(HidDevice& device)
{
    uint8_t v = config.keyLeds ? 0xff : 0x00;
    sendLeds(device, {2, v, v, v, v});
}

static void setWheelMode(HidDevice& device, int mode)
{
    wheelMode = mode;
    /* Forget any part-turn from the previous mode. */
    sentWheelPosition = wheelPosition;
    sendLeds(device, {4, MODE_LEDS[mode]});
    fmt::print("Wheel mode: {}\n", MODE_NAMES[mode]);
}

static void startKeyAction(int keynum, const Action& action)
{
    if (action.holdable())
        heldKeys[keynum] = pressStep(action.steps[0]);
    else
        tapAction(action);
}

static void stopKeyAction(int keynum)
{
    auto it = heldKeys.find(keynum);
    if (it != heldKeys.end())
    {
        releasePressed(it->second);
        heldKeys.erase(it);
    }
}

static void pressReleaseKey(HidDevice& device, int keynum, bool pressed)
{
    if (keynum == KEY_JOG || keynum == KEY_SHTL || keynum == KEY_SCRL)
    {
        if (pressed)
            setWheelMode(device,
                (keynum == KEY_JOG)    ? MODE_JOG
                : (keynum == KEY_SHTL) ? MODE_SHTL
                                       : MODE_SCRL);
        return;
    }

    if (!pressed)
    {
        stopKeyAction(keynum);
        return;
    }

    auto it = config.keys.find(keynum);
    if (learnMode)
        fmt::print("Key {:<12} (0x{:02x}) -> {}\n",
            keyName(keynum),
            keynum,
            (it == config.keys.end() || it->second.empty()) ? "(unmapped)"
                                                            : it->second.text);
    if ((it == config.keys.end()) || it->second.empty())
        return;

    startKeyAction(keynum, it->second);
}

static void handleWheel(int32_t delta)
{
    wheelPosition += delta;
    const WheelSettings& w = config.wheel[wheelMode];
    int32_t step = std::max(1, w.step);

    if (learnMode)
        fmt::print("Wheel {} delta {}\n", MODE_NAMES[wheelMode], delta);

    for (;;)
    {
        int32_t totalDelta = wheelPosition - sentWheelPosition;
        if (abs(totalDelta) < step)
            break;

        tapAction((totalDelta < 0) ? w.left : w.right);
        sentWheelPosition += (totalDelta < 0) ? -step : step;
    }
}

/* Xlib caches the keyboard mapping; keep it current when we (or a layout
 * switch) change it. */
static void processXEvents()
{
    while (XPending(display))
    {
        XEvent ev;
        XNextEvent(display, &ev);
        if (ev.type == MappingNotify)
            XRefreshKeyboardMapping(&ev.xmapping);
    }
}

static FileStamp configStamp;
static bool resolveAttached = false;

static void reloadConfigIfChanged(HidDevice* device)
{
    FileStamp now = stampOf(configPath);
    if (!reloadRequested && (now == configStamp))
        return;
    reloadRequested = false;
    configStamp = now;

    Config fresh;
    if (!loadConfig(configPath, fresh))
    {
        fmt::print(stderr,
            "Can't read {}: {} (keeping current mapping)\n",
            configPath,
            strerror(errno));
        return;
    }
    releaseAll();
    config = fresh;
    if (device && !resolveAttached)
        updateKeyLeds(*device);
}


/* ------------------------------------------------------------------------ */
/* DaVinci Resolve pass-through                                              */
/*                                                                           */
/*   real Speed Editor <-> bmdkey <-> virtual Speed Editor (uhid) <-> Resolve */
/*                                                                           */
/* bmdkey opens the real device through libusb, which detaches the kernel   */
/* driver, so its /dev/hidraw node disappears and Resolve can only find the  */
/* virtual one. While Resolve has the virtual device open, everything is     */
/* relayed both ways -- including Resolve's own unlock handshake, which the  */
/* real device answers -- except keys listed in [resolve.keys].              */
/* ------------------------------------------------------------------------ */

/* The real Speed Editor's HID report descriptor (from
 * /sys/class/hidraw/hidrawN/device/report_descriptor). */
static const uint8_t SPEED_EDITOR_DESCRIPTOR[] = {
    0x06, 0x00, 0xff, 0x09, 0x00, 0xa1, 0x01, 0x09, 0x01, 0xa1, 0x01, 0x85,
    0x01, 0x05, 0x06, 0x15, 0x00, 0x26, 0xff, 0x00, 0x09, 0x2a, 0x75, 0x08,
    0x95, 0x04, 0xb1, 0x81, 0x09, 0x2d, 0x09, 0x2e, 0x09, 0x2f, 0x75, 0x08,
    0x95, 0x03, 0xb1, 0x81, 0xc0, 0x09, 0x08, 0xa1, 0x01, 0x85, 0x08, 0x06,
    0x07, 0xff, 0x15, 0x00, 0x26, 0xff, 0x00, 0x09, 0x00, 0x75, 0x08, 0x95,
    0x20, 0xb1, 0x81, 0xc0, 0x09, 0x02, 0xa1, 0x01, 0x85, 0x03, 0x06, 0x01,
    0xff, 0x15, 0x00, 0x26, 0xff, 0x00, 0x09, 0x00, 0x75, 0x08, 0x95, 0x01,
    0x81, 0x02, 0x09, 0x01, 0x75, 0x08, 0x95, 0x04, 0x81, 0x02, 0x09, 0x02,
    0x75, 0x08, 0x95, 0x01, 0x81, 0x02, 0x09, 0x00, 0x75, 0x08, 0x95, 0x01,
    0x91, 0x02, 0x09, 0x01, 0x75, 0x08, 0x95, 0x04, 0x91, 0x02, 0x09, 0x02,
    0x75, 0x08, 0x95, 0x01, 0x91, 0x02, 0xc0, 0x09, 0x03, 0xa1, 0x01, 0x85,
    0x04, 0x06, 0x02, 0xff, 0x15, 0x00, 0x26, 0xff, 0x00, 0x19, 0x00, 0x29,
    0xff, 0x75, 0x10, 0x95, 0x06, 0x81, 0x00, 0x15, 0x00, 0x25, 0x01, 0x19,
    0x01, 0x29, 0x08, 0x75, 0x01, 0x95, 0x08, 0x91, 0x02, 0xc0, 0x09, 0x06,
    0xa1, 0x01, 0x85, 0x02, 0x06, 0x05, 0xff, 0x15, 0x00, 0x25, 0x01, 0x19,
    0x01, 0x29, 0x20, 0x75, 0x01, 0x95, 0x20, 0x91, 0x02, 0xc0, 0x09, 0x04,
    0xa1, 0x01, 0x85, 0x05, 0x06, 0x03, 0xff, 0x15, 0x00, 0x26, 0xff, 0x00,
    0x09, 0x00, 0x75, 0x08, 0x95, 0x01, 0xb1, 0x82, 0x09, 0x01, 0x75, 0x08,
    0x95, 0x04, 0xb1, 0x82, 0x09, 0x02, 0x75, 0x08, 0x95, 0x02, 0xb1, 0x82,
    0xc0, 0x09, 0x05, 0xa1, 0x01, 0x85, 0x06, 0x06, 0x04, 0xff, 0x15, 0x00,
    0x26, 0xff, 0x00, 0x09, 0x00, 0x75, 0x08, 0x95, 0x01, 0xb1, 0x82, 0x09,
    0x01, 0x75, 0x08, 0x95, 0x08, 0xb1, 0x82, 0xc0, 0x09, 0x07, 0xa1, 0x01,
    0x85, 0x07, 0x06, 0x06, 0xff, 0x15, 0x00, 0x26, 0xff, 0x00, 0x09, 0x00,
    0x75, 0x08, 0x95, 0x01, 0x81, 0x02, 0x09, 0x01, 0x75, 0x08, 0x95, 0x01,
    0x81, 0x02, 0x09, 0x00, 0x75, 0x08, 0x95, 0x01, 0xb1, 0x02, 0x09, 0x01,
    0x75, 0x08, 0x95, 0x01, 0xb1, 0x02, 0xc0, 0xc0,
};
static_assert(sizeof(SPEED_EDITOR_DESCRIPTOR) == 308);

class VirtualSpeedEditor
{
public:
    /* Returns false (after explaining why) if /dev/uhid can't be used; bmdkey
     * then simply runs without pass-through. */
    bool create()
    {
        _fd = open("/dev/uhid", O_RDWR | O_CLOEXEC | O_NONBLOCK);
        if (_fd < 0)
        {
            fmt::print(stderr,
                "Resolve pass-through off: can't open /dev/uhid ({}). "
                "Install 99-bmdkey.rules (see README).\n",
                strerror(errno));
            return false;
        }

        struct uhid_event ev = {};
        ev.type = UHID_CREATE2;
        auto& c = ev.u.create2;
        strncpy((char*)c.name,
            "Blackmagic Design DaVinci Resolve Speed Editor",
            sizeof(c.name) - 1);
        strncpy((char*)c.phys, "bmdkey/virtual", sizeof(c.phys) - 1);
        strncpy((char*)c.uniq, "bmdkey-virtual", sizeof(c.uniq) - 1);
        c.rd_size = sizeof(SPEED_EDITOR_DESCRIPTOR);
        c.bus = 0x03; /* BUS_USB */
        c.vendor = 0x1edb;
        c.product = 0xda0e;
        c.version = 0x0100;
        memcpy(c.rd_data, SPEED_EDITOR_DESCRIPTOR, c.rd_size);
        if (!sendEvent(ev))
        {
            close(_fd);
            _fd = -1;
            return false;
        }
        fmt::print("Virtual Speed Editor ready for DaVinci Resolve\n");
        return true;
    }

    ~VirtualSpeedEditor()
    {
        if (_fd >= 0)
        {
            struct uhid_event ev = {};
            ev.type = UHID_DESTROY;
            sendEvent(ev);
            close(_fd);
        }
    }

    bool active() const
    {
        return _fd >= 0;
    }

    int fd() const
    {
        return _fd;
    }

    /* An input report (keys, wheel, battery...) for Resolve to read. */
    void sendInput(const uint8_t* data, size_t len)
    {
        struct uhid_event ev = {};
        ev.type = UHID_INPUT2;
        ev.u.input2.size = std::min(len, sizeof(ev.u.input2.data));
        memcpy(ev.u.input2.data, data, ev.u.input2.size);
        sendEvent(ev);
    }

    bool sendEvent(const struct uhid_event& ev)
    {
        if (write(_fd, &ev, sizeof(ev)) != (ssize_t)sizeof(ev))
        {
            fmt::print(stderr, "uhid write failed: {}\n", strerror(errno));
            return false;
        }
        return true;
    }

    /* Next pending event, without blocking. */
    bool readEvent(struct uhid_event& ev)
    {
        return read(_fd, &ev, sizeof(ev)) > 0;
    }

private:
    int _fd = -1;
};

/* Keys whose press was passed to Resolve; their release must follow. */
static std::set<uint16_t> forwardedKeys;
static std::vector<uint8_t> lastForwardedKeyReport;
static bool resolveUnlockedReported = false;
static std::chrono::steady_clock::time_point nextAuth;

static void restoreStandalone(HidDevice& device);

static void onResolveAttach()
{
    releaseAll();
    forwardedKeys.clear();
    lastForwardedKeyReport.clear();
    resolveUnlockedReported = false;
    resolveAttached = true;
    fmt::print("Resolve connected: Speed Editor passed through "
               "({} key(s) taken over)\n",
        config.resolveKeys.size());
}

static void onResolveDetach(HidDevice& device)
{
    resolveAttached = false;
    releaseAll();
    forwardedKeys.clear();
    fmt::print("Resolve disconnected: back to the standalone mapping\n");
    restoreStandalone(device);
}

/* Feature report sizes from the descriptor, including the report id byte.
 * The device must be asked for exactly this much: a larger request may be
 * refused, and Resolve gives up on the device at the first error. */
static size_t featureReportSize(uint8_t id)
{
    switch (id)
    {
        case 1: return 8;
        case 5: return 8;
        case 6: return 10;
        case 7: return 3;
        case 8: return 33;
        default: return 64;
    }
}

static std::string hexBytes(const uint8_t* p, size_t n)
{
    std::string s;
    for (size_t i = 0; i < n; i++)
        s += fmt::format("{:02x} ", p[i]);
    return s;
}

/* --trace: every message between Resolve and the device. */
template <typename... Args>
static void trace(fmt::format_string<Args...> f, Args&&... args)
{
    if (traceMode)
        fmt::print("[trace] {}\n", fmt::format(f, std::forward<Args>(args)...));
}

/* Handle everything Resolve asked of the virtual device since last time. */
static void serviceVirtual(VirtualSpeedEditor& virt, HidDevice& device)
{
    struct uhid_event ev;
    while (virt.readEvent(ev))
    {
        switch (ev.type)
        {
            case UHID_OPEN:
                trace("OPEN");
                onResolveAttach();
                break;

            case UHID_CLOSE:
                trace("CLOSE");
                onResolveDetach(device);
                break;

            case UHID_OUTPUT:
            {
                /* Lights, wheel mode. */
                auto& o = ev.u.output;
                if (o.rtype == UHID_OUTPUT_REPORT && o.size)
                {
                    int n = device.writeRaw(o.data, o.size);
                    trace("OUTPUT  {}-> {}",
                        hexBytes(o.data, o.size),
                        (n >= 0) ? "ok" : "FAILED: " + device.lastError());
                }
                else
                    trace("OUTPUT  type {} ignored", (int)o.rtype);
                break;
            }

            case UHID_GET_REPORT:
            {
                auto& g = ev.u.get_report;
                struct uhid_event reply = {};
                reply.type = UHID_GET_REPORT_REPLY;
                reply.u.get_report_reply.id = g.id;
                int n = -1;
                uint8_t buf[64] = {g.rnum};
                if (g.rtype == UHID_FEATURE_REPORT)
                {
                    n = device.getFeatureRaw(buf, featureReportSize(g.rnum));
                    if (n > 0)
                    {
                        memcpy(reply.u.get_report_reply.data, buf, n);
                        reply.u.get_report_reply.size = n;
                        /* 06 04 = the device accepted Resolve's unlock. */
                        if ((g.rnum == 6) && (n > 1) && (buf[1] == 4) &&
                            !resolveUnlockedReported)
                        {
                            fmt::print("Resolve unlocked the Speed Editor\n");
                            resolveUnlockedReported = true;
                        }
                    }
                }
                reply.u.get_report_reply.err = (n > 0) ? 0 : EIO;
                if (g.rtype != UHID_FEATURE_REPORT)
                    trace("GET     report {} type {}: not supported",
                        (int)g.rnum, (int)g.rtype);
                else
                    trace("GET     report {} -> {}",
                        (int)g.rnum,
                        (n > 0) ? hexBytes(buf, n)
                                : "FAILED: " + device.lastError());
                virt.sendEvent(reply);
                break;
            }

            case UHID_SET_REPORT:
            {
                auto& r = ev.u.set_report;
                int n = -1;
                if (r.rtype == UHID_FEATURE_REPORT)
                    n = device.sendFeatureRaw(r.data, r.size);
                else if (r.rtype == UHID_OUTPUT_REPORT)
                    n = device.writeRaw(r.data, r.size);

                struct uhid_event reply = {};
                reply.type = UHID_SET_REPORT_REPLY;
                reply.u.set_report_reply.id = r.id;
                reply.u.set_report_reply.err = (n >= 0) ? 0 : EIO;
                trace("SET     {}{}-> {}",
                    (r.rtype == UHID_FEATURE_REPORT) ? "feature " : "",
                    hexBytes(r.data, r.size),
                    (n >= 0) ? "ok" : "FAILED: " + device.lastError());
                virt.sendEvent(reply);
                break;
            }

            default: /* UHID_START, UHID_STOP */
                break;
        }
    }
}

/* Key packet while Resolve is attached: run [resolve.keys] ourselves, pass
 * everything else on with those keys removed. */
static void proxyKeyPacket(
    VirtualSpeedEditor& virt, const std::vector<uint8_t>& data)
{
    std::vector<uint16_t> order;
    std::set<uint16_t> newState;
    for (int i = 0; (i < 6) && (2 + i * 2 < (int)data.size()); i++)
    {
        uint16_t k = getInt16(&data[1 + i * 2]);
        if (k)
        {
            order.push_back(k);
            newState.insert(k);
        }
    }

    for (uint16_t k : newState)
    {
        if (currentKeyboardState.count(k))
            continue;
        /* Newly pressed: ours or Resolve's? Decided now, for the whole
         * press, so a config reload can't strand a key in Resolve. */
        auto it = config.resolveKeys.find(k);
        bool ours = (it != config.resolveKeys.end()) && !it->second.empty();
        if (learnMode)
            fmt::print("Key {:<12} (0x{:02x}) -> {}\n",
                keyName(k),
                k,
                ours ? it->second.text : std::string("Resolve (native)"));
        if (ours)
            startKeyAction(k, it->second);
        else
            forwardedKeys.insert(k);
    }
    for (uint16_t k : currentKeyboardState)
    {
        if (newState.count(k))
            continue;
        if (!forwardedKeys.erase(k))
            stopKeyAction(k);
    }
    currentKeyboardState = newState;

    std::vector<uint8_t> out(13, 0);
    out[0] = 4;
    int slot = 0;
    for (uint16_t k : order)
        if (forwardedKeys.count(k))
        {
            out[1 + slot * 2] = k & 0xff;
            out[2 + slot * 2] = k >> 8;
            slot++;
        }
    if (out != lastForwardedKeyReport)
    {
        virt.sendInput(out.data(), out.size());
        lastForwardedKeyReport = out;
    }
}

/* Unlock the device ourselves and put back our lights and wheel mode. Used
 * at connect time and whenever Resolve lets go of the device. */
static void restoreStandalone(HidDevice& device)
{
    authenticate(device);
    device.send({3, 0, 0, 0, 0, 0, 0});
    updateKeyLeds(device);
    wheelPosition = sentWheelPosition = 0;
    setWheelMode(device, wheelMode);
    currentKeyboardState.clear();
    nextAuth = std::chrono::steady_clock::now() +
               std::chrono::milliseconds(TIMEOUT_MS);
}

static void usage()
{
    fmt::print(
        "Usage: bmdkey [-c CONFIG] [--learn] [--trace]\n"
        "  -c, --config FILE       config file (default {})\n"
        "  -l, --learn             print each key/wheel event as it happens\n"
        "  -t, --trace             print all traffic between Resolve and the "
        "device\n"
        "      --print-default-config\n"
        "                          write the built-in default config to "
        "stdout\n",
        defaultConfigPath());
}

int main(int argc, char** argv)
{
    /* Line-buffer output so a GUI reading our pipe sees messages instantly. */
    setvbuf(stdout, nullptr, _IOLBF, 0);
    setvbuf(stderr, nullptr, _IOLBF, 0);

    configPath = defaultConfigPath();
    for (int i = 1; i < argc; i++)
    {
        std::string a = argv[i];
        if (((a == "-c") || (a == "--config")) && (i + 1 < argc))
            configPath = argv[++i];
        else if ((a == "-l") || (a == "--learn"))
            learnMode = true;
        else if ((a == "-t") || (a == "--trace"))
            traceMode = true;
        else if (a == "--print-default-config")
        {
            fputs(DEFAULT_CONFIG, stdout);
            return 0;
        }
        else
        {
            usage();
            return (a == "-h") || (a == "--help") ? 0 : 1;
        }
    }

    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);
    signal(SIGHUP, hupHandler);
    display = XOpenDisplay(nullptr);
    if (!display)
    {
        fmt::print(stderr, "Can't open X display\n");
        return 1;
    }
    window = XDefaultRootWindow(display);
    int xtestEvent, xtestError, xtestMajor, xtestMinor;
    if (!XTestQueryExtension(
            display, &xtestEvent, &xtestError, &xtestMajor, &xtestMinor))
    {
        fmt::print(stderr, "X server has no XTEST extension\n");
        return 1;
    }
    atexit(releaseAll);

    XDisplayKeycodes(display, &minKeycode, &maxKeycode);
    findSpareKeycodes();

    ensureConfigExists(configPath);
    configStamp = stampOf(configPath);
    if (!loadConfig(configPath, config))
    {
        fmt::print(stderr, "Can't read {}: {}\n", configPath, strerror(errno));
        return 1;
    }
    wheelMode = config.defaultMode;

    hid_init();
    while (running)
    {
        try
        {
            HidDevice device(0x1edb, 0xda0e);
            resolveAttached = false;
            restoreStandalone(device);

            /* Created only while the real device is connected, so Resolve
             * sees it come and go along with the hardware. */
            VirtualSpeedEditor virt;
            if (config.resolvePassthrough)
                virt.create();
            /* With pass-through we must answer Resolve quickly, so poll the
             * device with a short timeout. */
            int readTimeout = virt.active() ? 5 : POLL_MS;

            while (running)
            {
                processXEvents();
                reloadConfigIfChanged(&device);
                if (virt.active())
                    serviceVirtual(virt, device);

                try
                {
                    auto data = device.recv(readTimeout);

                    if (resolveAttached)
                    {
                        if (data[0] == 4)
                            proxyKeyPacket(virt, data);
                        else
                            virt.sendInput(data.data(), data.size());
                        continue;
                    }

                    switch (data[0])
                    {
                        case 3:
                            /* wheel packet */
                            handleWheel((int32_t)getInt32(&data[2]));
                            break;

                        case 4:
                        {
                            /* Keyboard packet */
                            std::set<uint16_t> newKeyboardState;
                            for (int i = 0; i < 6; i++)
                            {
                                uint16_t keycode = getInt16(&data[1 + i * 2]);
                                if (keycode)
                                    newKeyboardState.insert(keycode);
                            }

                            std::set<uint16_t> keysPressed;
                            std::ranges::set_difference(newKeyboardState,
                                currentKeyboardState,
                                std::inserter(
                                    keysPressed, keysPressed.begin()));

                            std::set<uint16_t> keysReleased;
                            std::ranges::set_difference(currentKeyboardState,
                                newKeyboardState,
                                std::inserter(
                                    keysReleased, keysReleased.begin()));

                            for (uint16_t k : keysPressed)
                                pressReleaseKey(device, k, true);
                            for (uint16_t k : keysReleased)
                                pressReleaseKey(device, k, false);

                            currentKeyboardState = newKeyboardState;
                            break;
                        }

                        default:
                            fmt::print("Unhandled packet: ");
                            for (uint8_t c : data)
                                fmt::print("{:02x} ", c);
                            fmt::print("\n");
                    }
                }
                catch (const TimeoutException& e)
                {
                }

                /* While Resolve is attached it does its own unlocking. */
                if (!resolveAttached &&
                    (std::chrono::steady_clock::now() >= nextAuth))
                {
                    authenticate(device);
                    nextAuth = std::chrono::steady_clock::now() +
                               std::chrono::milliseconds(TIMEOUT_MS);
                }
            }
        }
        catch (const HidException& e)
        {
            /* Unplugged, or the read failed: let go of everything and wait
             * for it to come back rather than crashing with keys held down. */
            releaseAll();
            if (running)
            {
                fmt::print(stderr, "Device error ({}), reconnecting...\n",
                    e.what());
                sleep(1);
            }
        }
    }
    hid_exit();
    return 0;
}
