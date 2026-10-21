bmdkey (configurable fork)
==========================

Use a **Blackmagic Design Speed Editor** as a programmable macro pad and jog
wheel on Linux (X11), without DaVinci Resolve.

This fork of [David Given's bmdkey](https://github.com/davidgiven/bmdkey) adds
a plain-text config file, so you can remap every key and the wheel **without
recompiling**. The file is reloaded automatically as soon as you save it.

Features
--------

- **Config file instead of recompiling.** All mappings live in
  `~/.config/bmdkey/bmdkey.conf`, and bmdkey picks up changes the moment you
  save.
- **Key combos.** For example `ctrl+b`, `ctrl+shift+z`, `alt+Left`, `super+1`.
  A combo is held for as long as you hold the key, so auto-repeat and holding
  modifiers work.
- **Key sequences.** For example `ctrl+c, ctrl+v` plays several combos from
  one press.
- **Mouse actions.** Scroll up/down/left/right and left/middle/right clicks,
  with modifiers if you want them (`ctrl+scroll_up`).
- **Shell commands.** `exec:any command` runs a script or program.
- **Three wheel modes.** The **SHTL**, **JOG** and **SCRL** keys switch the
  wheel between three independently configured modes. Each mode has its own
  step size and its own left/right actions, and the key for the active mode
  lights up.
- **GUI.** `bdm_gui.py` starts and stops bmdkey, shows its log, and edits the
  mapping. It can also record a combo you press on your normal keyboard.
- **Learn mode.** `bmdkey --learn` prints the name of each key you press and
  each wheel movement.
- **Survives unplugging.** If the Speed Editor is disconnected, bmdkey
  releases any held keys and waits for it to come back.

> Only works while DaVinci Resolve is **closed**, because Resolve takes over
> the device.

Building
--------

Dependencies on Debian / Ubuntu / Linux Mint:

```bash
sudo apt install build-essential pkg-config \
    libhidapi-dev libfmt-dev libx11-dev libxtst-dev python3-tk
```

Build:

```bash
git clone https://github.com/cegaton/bmdkey
cd bmdkey
make
./bmdkey --help
```

`python3-tk` is only needed for the GUI. If you're rebuilding over an older
checkout, delete the `.obj` folder first (`rm -rf .obj`).

### USB permissions (one time)

To run bmdkey as your normal user (recommended, not with `sudo`), add a udev
rule:

```bash
echo 'SUBSYSTEM=="usb", ATTRS{idVendor}=="1edb", ATTRS{idProduct}=="da0e", MODE="0666"
KERNEL=="hidraw*", ATTRS{idVendor}=="1edb", ATTRS{idProduct}=="da0e", MODE="0666"' \
  | sudo tee /etc/udev/rules.d/99-speed-editor.rules
sudo udevadm control --reload-rules && sudo udevadm trigger
```

Then unplug the Speed Editor and plug it back in. If bmdkey prints
`Waiting...` forever while the device is connected, this step is missing.

Running
-------

```bash
./bmdkey              # normal use
./bmdkey --learn      # also print every key / wheel event
./bmdkey -c my.conf   # use a different config file
```

On first run bmdkey creates `~/.config/bmdkey/bmdkey.conf` with a default
mapping (the same one as `bmdkey.conf.example`). Edit it with any text editor.
When you save, bmdkey prints a new `Loaded ...` line and the new mapping is
live. `kill -HUP $(pidof bmdkey)` also forces a reload.

If a line has a mistake, bmdkey reports it with its line number, skips it,
and loads everything else.

Configuration
-------------

```ini
[general]
default_mode = jog          # wheel mode at startup: jog, shtl or scrl
key_leds = on               # light the LEDs on CUT, DIS, CAM1..9, ...
sequence_delay_ms = 10      # pause between the steps of a sequence

[keys]
STOP_PLAY = space
IN        = i
OUT       = o
CUT       = ctrl+b
TRANS     = ctrl+c, ctrl+v
SNAP      = exec:notify-send "Snap pressed"
CAM1      = ctrl+scroll_up

[wheel.jog]                 # selected with the JOG key
step  = 12000
left  = Left
right = Right

[wheel.shtl]                # selected with the SHTL key
step  = 3000
left  = Left
right = Right

[wheel.scrl]                # selected with the SCRL key
step  = 6000
left  = scroll_up
right = scroll_down
```

> In the real file, comments must be on their own line, starting with `#` or
> `;`. They're shown inline here only for brevity.

### Actions

| Write                          | Does                                                        |
|--------------------------------|-------------------------------------------------------------|
| `i`, `F13`, `space`, `Left`    | A single key. Uses X keysym names; `B` (uppercase) sends shift+b. |
| `ctrl+shift+z`                 | Key combo. Modifiers: `ctrl` `shift` `alt` `super` `altgr`.  |
| `scroll_up` `scroll_down`      | Mouse wheel. Also `scroll_left` and `scroll_right`.         |
| `click_left` `click_middle` `click_right` | Mouse buttons. Also `buttonN`.                  |
| `ctrl+scroll_up`               | A mouse action with modifiers.                              |
| `ctrl+c, ctrl+v`               | A sequence: comma-separated, played in order.               |
| `exec:command args`            | Runs the command through `/bin/sh`.                         |
| `none` or empty                | Does nothing.                                               |

Write `plus` for the `+` key. Handy aliases: `esc`, `enter`, `del`, `bksp`,
`pgup`, `pgdn`, `tab`.

A **single** combo or mouse click is held for as long as you hold the Speed
Editor key. Sequences, scrolls and `exec:` fire once on press.

Keysyms your keyboard layout doesn't have (F13–F24, for example) are mapped
onto spare keycodes automatically, so applications see them as normal keys.

### Key names

Use these on the left of `=` in `[keys]`. They match what's printed on the
keys, and `bmdkey --learn` shows them too.

| | | | | |
|---|---|---|---|---|
| `SMART_INSRT` | `APPEND` | `RIPL_OWR` | `CLOSE_UP` | `PLACE_ON_TOP` |
| `SRC_OWR` | `IN` | `OUT` | `TRIM_IN` | `TRIM_OUT` |
| `ROLL` | `SLIP_SRC` | `SLIP_DEST` | `TRANS_DUR` | `CUT` |
| `DIS` | `SMTH_CUT` | `SOURCE` | `TIMELINE` | `ESC` |
| `SYNC_BIN` | `AUDIO_LEVEL` | `FULL_VIEW` | `TRANS` | `SPLIT` |
| `SNAP` | `RIPL_DEL` | `CAM1` … `CAM9` | `LIVE_OWR` | `VIDEO_ONLY` |
| `AUDIO_ONLY` | `STOP_PLAY` | | | |

`SHTL`, `JOG` and `SCRL` are reserved for switching wheel modes. Raw codes
such as `0x2a` are also accepted.

### Wheel

`step` is how far the wheel has to turn, in device units, for one action.
Smaller means more actions per turn; 12000 is roughly 30 per revolution.
Wheel actions can be anything a key can do, including sequences and `exec:`.

GUI
---

```bash
python3 bdm_gui.py
```

- **Run.** Start/stop bmdkey and see its log. Tick *Learn mode* before
  starting if you want key names in the log.
- **Keys.** Select a key, then type an action, pick one from *insert…*, or
  click **Capture** and press a combo on your normal keyboard. Click
  **Apply**, then **Save** (or Ctrl+S).
- **Wheel.** Step and left/right actions for SHTL / JOG / SCRL, plus the
  general settings.

Saving keeps the comments and layout of your config file. If bmdkey is
running, the change applies immediately. The GUI expects the `bmdkey` binary
to be in the same folder as `bdm_gui.py`; change `BMDKEY_PATH` at the top of
the script if yours is elsewhere.

Only one copy of bmdkey can talk to the device at a time.

Start at login
--------------

Add one of these to your desktop's *Startup Applications*:

```
/path/to/bmdkey/bmdkey
python3 /path/to/bmdkey/bdm_gui.py
```

Troubleshooting
---------------

| Symptom | Fix |
|---|---|
| `Waiting...` forever | Resolve is open, another bmdkey is running, or the udev rule is missing. |
| Keys do nothing in one app | Check the mapping with `--learn`. Some apps ignore synthetic events for F13–F24, so try another key. |
| `LED update failed` in the log | Harmless; please open an issue. The mode lights on SHTL/JOG/SCRL are the least-tested part. |
| Config changes ignored | Make sure you're editing the file named in the `Loaded ...` line. Running under `sudo` uses `/root/.config`. |

Wayland sessions aren't supported; bmdkey sends events through X11/XTest.

Changes from upstream
---------------------

- Mappings moved from `main.cc` into a config file with automatic reloading.
- Key combos, sequences, mouse buttons/scroll and shell commands as actions.
- SHTL / JOG / SCRL select wheel modes, each with its own LED.
- Output via XTest directly (libfakekey is no longer required).
- LIVE O/WR fixed. Its code is `0x30`; it was listed as a duplicate of
  AUDIO LEVEL (`0x2c`) and never worked.
- Reconnects after the device is unplugged instead of crashing.
- `bdm_gui.py`: log view, mapping editor, path no longer hard-coded.

Credits
-------

- Original bmdkey © 2024 [David Given](https://github.com/davidgiven/bmdkey)
  ([video of it being written](https://www.youtube.com/watch?v=UoIlwze5xp4)).
- Device unlock algorithm from
  [smunaut/blackmagic-misc](https://github.com/smunaut/blackmagic-misc).
  Without it the device is a paperweight unless Resolve is running.
- Configurable fork maintained by [cegaton](https://github.com/cegaton/bmdkey).

License
-------

Two-clause BSD, as in the original. See [LICENSE](LICENSE). The original
copyright belongs to David Given; this fork's changes are offered under the
same license.
