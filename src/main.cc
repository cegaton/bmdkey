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

[general]
# Wheel mode at startup: jog, shtl or scrl
default_mode = jog
# Light the LEDs on the keys that have them (CUT, DIS, CAM1..9, ...)
key_leds = on
# Pause between the steps of a sequence, in milliseconds
sequence_delay_ms = 10

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
            else
                warn(fmt::format("unknown setting '{}'", key));
        }
        else if (section == "keys")
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
                warn(fmt::format(
                    "{} is reserved for switching wheel modes", key));
                continue;
            }
            std::string error;
            auto action = parseAction(value, error);
            if (!action)
            {
                warn(fmt::format("{}: {}", key, error));
                continue;
            }
            cfg.keys[*code] = *action;
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

    fmt::print("Loaded {} ({} key mappings{})\n",
        path,
        cfg.keys.size(),
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
        auto it = heldKeys.find(keynum);
        if (it != heldKeys.end())
        {
            releasePressed(it->second);
            heldKeys.erase(it);
        }
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

    const Action& action = it->second;
    if (action.holdable())
        heldKeys[keynum] = pressStep(action.steps[0]);
    else
        tapAction(action);
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
    if (device)
        updateKeyLeds(*device);
}

static void usage()
{
    fmt::print(
        "Usage: bmdkey [-c CONFIG] [--learn]\n"
        "  -c, --config FILE       config file (default {})\n"
        "  -l, --learn             print each key/wheel event as it happens\n"
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
            authenticate(device);

            device.send({3, 0, 0, 0, 0, 0, 0});
            updateKeyLeds(device);
            wheelPosition = sentWheelPosition = 0;
            setWheelMode(device, wheelMode);
            currentKeyboardState.clear();

            auto nextAuth = std::chrono::steady_clock::now() +
                            std::chrono::milliseconds(TIMEOUT_MS);
            while (running)
            {
                processXEvents();
                reloadConfigIfChanged(&device);

                try
                {
                    auto data = device.recv(POLL_MS);

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

                if (std::chrono::steady_clock::now() >= nextAuth)
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
