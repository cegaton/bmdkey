# Simple GUI Launcher for bmdkey

# A clean way to control `bmdkey` is:

# * keep `bmdkey` as the backend executable
# * create a small GUI wrapper that:

  # * launches it
  # * stops it
  # * shows status
  # * edits the key / wheel mapping (~/.config/bmdkey/bmdkey.conf)

  # bmdkey reloads the config file by itself whenever it changes, so pressing
  # Save in the mapping tabs applies the change immediately, no restart needed.

  #IMPORTANT:

  # BMDKEY_PATH below defaults to the bmdkey executable sitting next to this
  # script. Change it if you keep the executable somewhere else.



import os
import queue
import subprocess
import signal
import threading
import tkinter as tk
from tkinter import ttk, messagebox

HERE = os.path.dirname(os.path.abspath(__file__))
BMDKEY_PATH = os.path.join(HERE, "bmdkey")

CONFIG_PATH = os.path.join(
    os.environ.get("XDG_CONFIG_HOME") or os.path.expanduser("~/.config"),
    "bmdkey", "bmdkey.conf")

process = None
log_queue = queue.Queue()

# (config name, label printed on the key), roughly in panel order.
SPEED_EDITOR_KEYS = [
    ("SMART_INSRT", "SMART INSRT"), ("APPEND", "APPND"),
    ("RIPL_OWR", "RIPL O/WR"), ("CLOSE_UP", "CLOSE UP"),
    ("PLACE_ON_TOP", "PLACE ON TOP"), ("SRC_OWR", "SRC O/WR"),
    ("IN", "IN"), ("OUT", "OUT"), ("TRIM_IN", "TRIM IN"),
    ("TRIM_OUT", "TRIM OUT"), ("ROLL", "ROLL"), ("SLIP_SRC", "SLIP SRC"),
    ("SLIP_DEST", "SLIP DEST"), ("TRANS_DUR", "TRANS DUR"),
    ("CUT", "CUT"), ("DIS", "DIS"), ("SMTH_CUT", "SMTH CUT"),
    ("ESC", "ESC"), ("SYNC_BIN", "SYNC BIN"),
    ("AUDIO_LEVEL", "AUDIO LEVEL"), ("FULL_VIEW", "FULL VIEW"),
    ("TRANS", "TRANS"), ("SPLIT", "SPLIT"), ("SNAP", "SNAP"),
    ("RIPL_DEL", "RIPL DEL"),
    ("SOURCE", "SOURCE"), ("TIMELINE", "TIMELINE"),
    ("CAM1", "CAM 1"), ("CAM2", "CAM 2"), ("CAM3", "CAM 3"),
    ("CAM4", "CAM 4"), ("CAM5", "CAM 5"), ("CAM6", "CAM 6"),
    ("CAM7", "CAM 7"), ("CAM8", "CAM 8"), ("CAM9", "CAM 9"),
    ("LIVE_OWR", "LIVE O/WR"), ("VIDEO_ONLY", "VIDEO ONLY"),
    ("AUDIO_ONLY", "AUDIO ONLY"), ("STOP_PLAY", "STOP/PLAY"),
]

WHEEL_MODES = [("jog", "JOG"), ("shtl", "SHTL"), ("scrl", "SCRL")]

ACTION_PRESETS = [
    "scroll_up", "scroll_down", "scroll_left", "scroll_right",
    "ctrl+scroll_up", "ctrl+scroll_down", "shift+scroll_up",
    "shift+scroll_down", "click_left", "click_middle", "click_right",
    "Left", "Right", "Up", "Down", "space", "Return", "Escape",
    "exec:", "none",
]

ACTION_HELP = (
    "Action syntax:  i   F13   ctrl+shift+z   alt+Left   scroll_up   "
    "click_left\n"
    "Sequence:  ctrl+c, ctrl+v        Command:  exec:notify-send hi\n"
    "Modifiers: ctrl shift alt super altgr. Write 'plus' for the + key. "
    "Empty = unmapped.")

FALLBACK_CONFIG = """# bmdkey configuration (see 'bmdkey --print-default-config')

[general]
default_mode = jog
key_leds = on
sequence_delay_ms = 10

[keys]

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
"""


# --------------------------------------------------------------------------
# Config file: edited line-by-line so your comments and layout survive.
# --------------------------------------------------------------------------

_KEY_ALIASES = {"RIPL_O_WR": "RIPL_OWR", "SRC_O_WR": "SRC_OWR",
                "LIVE_O_WR": "LIVE_OWR", "PLAY": "STOP_PLAY",
                "STOP": "STOP_PLAY"}


def _norm(section, key):
    if section == "keys":
        k = key.upper()
        for c in " -/":
            k = k.replace(c, "_")
        return _KEY_ALIASES.get(k, k)
    return key.lower()


class ConfigDoc:
    def __init__(self, path):
        self.path = path
        self.lines = []
        self.load()

    def load(self):
        if os.path.exists(self.path):
            with open(self.path) as f:
                self.lines = f.read().splitlines()
            return
        text = FALLBACK_CONFIG
        try:
            text = subprocess.run([BMDKEY_PATH, "--print-default-config"],
                                  capture_output=True, text=True,
                                  check=True).stdout
        except (OSError, subprocess.CalledProcessError):
            pass
        self.lines = text.splitlines()

    def _index(self):
        """{(section, normalised key): line number} plus section extents."""
        entries, sections, section = {}, {}, ""
        for i, raw in enumerate(self.lines):
            line = raw.strip()
            if not line or line[0] in "#;":
                continue
            if line.startswith("[") and line.endswith("]"):
                section = line[1:-1].strip().lower()
                sections.setdefault(section, [i, i])
                continue
            if "=" in line:
                key = line.split("=", 1)[0].strip()
                entries[(section, _norm(section, key))] = i
                if section in sections:
                    sections[section][1] = i
        return entries, sections

    def get(self, section, key, default=""):
        entries, _ = self._index()
        i = entries.get((section, _norm(section, key)))
        if i is None:
            return default
        return self.lines[i].split("=", 1)[1].strip()

    def set(self, section, key, value):
        entries, sections = self._index()
        i = entries.get((section, _norm(section, key)))
        value = value.strip()

        if not value:
            if i is not None:
                del self.lines[i]
            return

        if i is not None:
            before = self.lines[i].split("=", 1)[0]
            self.lines[i] = f"{before}= {value}"
            return

        new_line = f"{key:<12} = {value}" if section == "keys" \
            else f"{key} = {value}"
        if section in sections:
            self.lines.insert(sections[section][1] + 1, new_line)
        else:
            if self.lines and self.lines[-1].strip():
                self.lines.append("")
            self.lines += [f"[{section}]", new_line]

    def save(self):
        os.makedirs(os.path.dirname(self.path), exist_ok=True)
        tmp = self.path + ".tmp"
        with open(tmp, "w") as f:
            f.write("\n".join(self.lines) + "\n")
        os.replace(tmp, self.path)   # bmdkey notices and reloads


# --------------------------------------------------------------------------
# Running the daemon
# --------------------------------------------------------------------------

def _reader(proc):
    for line in proc.stdout:
        log_queue.put(line)
    log_queue.put(None)


def start_bmdkey():
    global process

    if process is None:
        args = [BMDKEY_PATH, "-c", CONFIG_PATH]
        if learn_var.get():
            args.append("--learn")
        try:
            process = subprocess.Popen(
                args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                text=True, bufsize=1)
        except OSError as e:
            messagebox.showerror("bmdkey", f"Can't start {BMDKEY_PATH}:\n{e}")
            return
        threading.Thread(target=_reader, args=(process,), daemon=True).start()

        status_label.config(text="bmdkey is RUNNING")


def stop_bmdkey():
    global process

    if process is not None:
        process.terminate()
        try:
            process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
        process = None

        status_label.config(text="bmdkey is STOPPED")


def toggle_bmdkey():
    if process is None:
        start_bmdkey()
    else:
        stop_bmdkey()


def poll_log():
    global process
    try:
        while True:
            line = log_queue.get_nowait()
            if line is None:
                # The daemon exited on its own (crash, or killed elsewhere).
                if process is not None and process.poll() is not None:
                    process = None
                    status_label.config(text="bmdkey is STOPPED")
                continue
            log_text.configure(state="normal")
            log_text.insert("end", line)
            log_text.see("end")
            log_text.configure(state="disabled")
    except queue.Empty:
        pass
    root.after(100, poll_log)


# --------------------------------------------------------------------------
# Mapping editor
# --------------------------------------------------------------------------

def combo_from_event(event):
    """Turn a Tk key event into bmdkey syntax, e.g. ctrl+shift+z."""
    if event.keysym in ("Control_L", "Control_R", "Shift_L", "Shift_R",
                        "Alt_L", "Alt_R", "Super_L", "Super_R", "Meta_L",
                        "Meta_R", "ISO_Level3_Shift", "Caps_Lock"):
        return None
    mods = []
    if event.state & 0x4:
        mods.append("ctrl")
    if event.state & 0x1:
        mods.append("shift")
    if event.state & 0x8:
        mods.append("alt")
    if event.state & 0x40:
        mods.append("super")
    key = event.keysym
    if len(key) == 1 and key.isalpha():
        key = key.lower()          # shift is already in mods
    if key == "plus" or key == "+":
        key = "plus"
    return "+".join(mods + [key])


class ActionEntry(ttk.Frame):
    """Entry + preset dropdown + 'Capture' (press a combo to record it)."""

    def __init__(self, parent, width=20):
        super().__init__(parent)
        self.var = tk.StringVar()

        # Packed right-to-left first so the buttons keep their space and the
        # entry takes whatever is left.
        self.capture_btn = ttk.Button(self, text="Capture", width=8,
                                      command=self._start_capture)
        self.capture_btn.pack(side="right", padx=(4, 0))

        self.preset = ttk.Combobox(self, values=ACTION_PRESETS, width=14,
                                   state="readonly")
        self.preset.set("insert…")
        self.preset.bind("<<ComboboxSelected>>", self._insert_preset)
        self.preset.pack(side="right", padx=(4, 0))

        self.entry = ttk.Entry(self, textvariable=self.var, width=width)
        self.entry.pack(side="left", fill="x", expand=True)
        self._capturing = False

    def _insert_preset(self, _):
        p = self.preset.get()
        self.preset.set("insert…")
        if p == "none":
            self.var.set("")
        elif p == "exec:":
            self.var.set("exec:")
            self.entry.icursor("end")
        elif self.var.get().strip() and not self.var.get().startswith("exec:"):
            self.var.set(self.var.get().rstrip(", ") + ", " + p)
        else:
            self.var.set(p)
        self.entry.focus_set()

    def _start_capture(self):
        self._capturing = True
        self.capture_btn.config(text="Press…")
        self.entry.focus_set()
        self.entry.bind("<KeyPress>", self._on_key)
        self.entry.bind("<FocusOut>", lambda e: self._stop_capture())

    def _stop_capture(self):
        self._capturing = False
        self.capture_btn.config(text="Capture")
        self.entry.unbind("<KeyPress>")
        self.entry.unbind("<FocusOut>")

    def _on_key(self, event):
        combo = combo_from_event(event)
        if combo is None:
            return "break"
        self.var.set(combo)
        self._stop_capture()
        return "break"

    def get(self):
        return self.var.get().strip()

    def set(self, value):
        self.var.set(value)


class MappingEditor:
    def __init__(self, keys_tab, wheel_tab):
        self.doc = ConfigDoc(CONFIG_PATH)
        self.dirty = False
        self._build_keys(keys_tab)
        self._build_wheel(wheel_tab)
        self.refresh()

    # ---- Keys tab -------------------------------------------------------
    def _build_keys(self, tab):
        cols = ("label", "action")
        self.tree = ttk.Treeview(tab, columns=cols, show="headings",
                                 height=14, selectmode="browse")
        self.tree.heading("label", text="Speed Editor key")
        self.tree.heading("action", text="Action")
        self.tree.column("label", width=150, stretch=False)
        self.tree.column("action", width=380)
        sb = ttk.Scrollbar(tab, orient="vertical", command=self.tree.yview)
        self.tree.configure(yscrollcommand=sb.set)
        self.tree.grid(row=0, column=0, sticky="nsew")
        sb.grid(row=0, column=1, sticky="ns")
        self.tree.bind("<<TreeviewSelect>>", self._on_select)

        edit = ttk.LabelFrame(tab, text="Edit selected key", padding=8)
        edit.grid(row=1, column=0, columnspan=2, sticky="ew", pady=(8, 0))
        self.sel_label = ttk.Label(edit, text="(select a key)",
                                   font=("Sans", 10, "bold"))
        self.sel_label.grid(row=0, column=0, sticky="w")
        ttk.Button(edit, text="Apply", command=self._apply_key).grid(
            row=0, column=2)
        ttk.Button(edit, text="Clear", command=self._clear_key).grid(
            row=0, column=3, padx=(4, 0))
        self.key_action = ActionEntry(edit)
        self.key_action.grid(row=1, column=0, columnspan=4, sticky="ew",
                             pady=(6, 0))
        self.key_action.entry.bind("<Return>", lambda e: self._apply_key())
        edit.columnconfigure(1, weight=1)

        ttk.Label(tab, text=ACTION_HELP, foreground="#666",
                  justify="left").grid(row=2, column=0, columnspan=2,
                                       sticky="w", pady=(6, 0))
        tab.rowconfigure(0, weight=1)
        tab.columnconfigure(0, weight=1)

    def _on_select(self, _):
        sel = self.tree.selection()
        if not sel:
            return
        name = sel[0]
        label = dict(SPEED_EDITOR_KEYS)[name]
        self.sel_label.config(text=label)
        self.key_action.set(self.doc.get("keys", name))

    def _apply_key(self):
        sel = self.tree.selection()
        if not sel:
            return
        self.doc.set("keys", sel[0], self.key_action.get())
        self._mark_dirty()
        self._refresh_tree(keep=sel[0])

    def _clear_key(self):
        self.key_action.set("")
        self._apply_key()

    def _refresh_tree(self, keep=None):
        self.tree.delete(*self.tree.get_children())
        for name, label in SPEED_EDITOR_KEYS:
            action = self.doc.get("keys", name) or "—"
            self.tree.insert("", "end", iid=name, values=(label, action))
        if keep:
            self.tree.selection_set(keep)
            self.tree.see(keep)

    # ---- Wheel tab ------------------------------------------------------
    def _build_wheel(self, tab):
        ttk.Label(tab, text="Press SHTL, JOG or SCRL on the Speed Editor to "
                  "switch the wheel between these modes.").grid(
            row=0, column=0, columnspan=2, sticky="w", pady=(0, 8))
        self.wheel_widgets = {}
        for r, (mode, label) in enumerate(WHEEL_MODES, start=1):
            box = ttk.LabelFrame(tab, text=label, padding=8)
            box.grid(row=r, column=0, columnspan=2, sticky="ew", pady=4)
            ttk.Label(box, text="Step").grid(row=0, column=0, sticky="w")
            step = ttk.Spinbox(box, from_=100, to=100000, increment=500,
                               width=8)
            step.grid(row=0, column=1, sticky="w", padx=6)
            ttk.Label(box, text="smaller = more actions per turn",
                      foreground="#666").grid(row=0, column=2, sticky="w")
            ttk.Label(box, text="Turn left").grid(row=1, column=0, sticky="w")
            left = ActionEntry(box)
            left.grid(row=1, column=1, columnspan=2, sticky="ew", padx=6,
                      pady=2)
            ttk.Label(box, text="Turn right").grid(row=2, column=0, sticky="w")
            right = ActionEntry(box)
            right.grid(row=2, column=1, columnspan=2, sticky="ew", padx=6,
                       pady=2)
            box.columnconfigure(2, weight=1)
            for w in (step, left.var, right.var):
                if isinstance(w, tk.StringVar):
                    w.trace_add("write", lambda *a: self._mark_dirty())
                else:
                    w.bind("<KeyRelease>", lambda e: self._mark_dirty())
                    w.configure(command=self._mark_dirty)
            self.wheel_widgets[mode] = (step, left, right)

        gen = ttk.LabelFrame(tab, text="General", padding=8)
        gen.grid(row=4, column=0, columnspan=2, sticky="ew", pady=4)
        ttk.Label(gen, text="Start in mode").grid(row=0, column=0, sticky="w")
        self.default_mode = ttk.Combobox(
            gen, values=[m for m, _ in WHEEL_MODES], width=6,
            state="readonly")
        self.default_mode.grid(row=0, column=1, sticky="w", padx=6)
        self.default_mode.bind("<<ComboboxSelected>>",
                               lambda e: self._mark_dirty())
        self.key_leds = tk.BooleanVar()
        ttk.Checkbutton(gen, text="Key LEDs on", variable=self.key_leds,
                        command=self._mark_dirty).grid(row=0, column=2,
                                                       padx=12)
        ttk.Label(gen, text="Sequence delay (ms)").grid(row=0, column=3)
        self.seq_delay = ttk.Spinbox(gen, from_=0, to=1000, width=5,
                                     command=self._mark_dirty)
        self.seq_delay.bind("<KeyRelease>", lambda e: self._mark_dirty())
        self.seq_delay.grid(row=0, column=4, padx=6)
        tab.columnconfigure(0, weight=1)

    def _collect_wheel(self):
        for mode, (step, left, right) in self.wheel_widgets.items():
            sec = f"wheel.{mode}"
            self.doc.set(sec, "step", step.get())
            self.doc.set(sec, "left", left.get())
            self.doc.set(sec, "right", right.get())
        self.doc.set("general", "default_mode", self.default_mode.get())
        self.doc.set("general", "key_leds",
                     "on" if self.key_leds.get() else "off")
        self.doc.set("general", "sequence_delay_ms", self.seq_delay.get())

    # ---- shared ---------------------------------------------------------
    def _mark_dirty(self):
        if getattr(self, "_loading", False):
            return
        self.dirty = True
        save_status.config(text="Unsaved changes")

    def refresh(self):
        self._loading = True
        self._refresh_tree()
        for mode, (step, left, right) in self.wheel_widgets.items():
            sec = f"wheel.{mode}"
            step.set(self.doc.get(sec, "step", "12000"))
            left.set(self.doc.get(sec, "left"))
            right.set(self.doc.get(sec, "right"))
        self.default_mode.set(self.doc.get("general", "default_mode", "jog"))
        self.key_leds.set(
            self.doc.get("general", "key_leds", "on").lower()
            in ("on", "yes", "true", "1"))
        self.seq_delay.set(self.doc.get("general", "sequence_delay_ms", "10"))
        self._loading = False
        self.dirty = False
        save_status.config(text=CONFIG_PATH)

    def save(self):
        self._collect_wheel()
        try:
            self.doc.save()
        except OSError as e:
            messagebox.showerror("bmdkey", f"Can't save {CONFIG_PATH}:\n{e}")
            return
        self.dirty = False
        save_status.config(
            text="Saved — applied" if process is not None
            else "Saved (applies when bmdkey runs)")

    def revert(self):
        self.doc.load()
        self.refresh()


root = tk.Tk()
root.title("bmdkey controller")
root.geometry("680x590")

notebook = ttk.Notebook(root)
notebook.pack(fill="both", expand=True, padx=8, pady=(8, 0))

# ---- Run tab (the original launcher) ------------------------------------
frame = ttk.Frame(notebook, padding=20)
notebook.add(frame, text="Run")

status_label = ttk.Label(
    frame,
    text="bmdkey is STOPPED",
    font=("Sans", 12)
)
status_label.pack(pady=10)

button = ttk.Button(
    frame,
    text="Start / Stop",
    command=toggle_bmdkey
)
button.pack(pady=10)

learn_var = tk.BooleanVar(value=False)
ttk.Checkbutton(frame, text="Learn mode (log every key and wheel event; "
                "applies on next start)", variable=learn_var).pack()

log_text = tk.Text(frame, height=14, state="disabled", wrap="word",
                   font=("Monospace", 9))
log_text.pack(fill="both", expand=True, pady=(10, 0))

# ---- Mapping tabs --------------------------------------------------------
keys_tab = ttk.Frame(notebook, padding=10)
wheel_tab = ttk.Frame(notebook, padding=10)
notebook.add(keys_tab, text="Keys")
notebook.add(wheel_tab, text="Wheel")

bottom = ttk.Frame(root, padding=(8, 6))
bottom.pack(fill="x")
save_status = ttk.Label(bottom, text="", foreground="#666")
save_status.pack(side="left")

editor = MappingEditor(keys_tab, wheel_tab)

ttk.Button(bottom, text="Save", command=editor.save).pack(side="right")
ttk.Button(bottom, text="Revert", command=editor.revert).pack(
    side="right", padx=6)
root.bind("<Control-s>", lambda e: editor.save())


def on_close():
    if editor.dirty and messagebox.askyesno(
            "bmdkey", "Save mapping changes before closing?"):
        editor.save()
    stop_bmdkey()
    root.destroy()


root.protocol("WM_DELETE_WINDOW", on_close)

root.after(100, poll_log)
root.mainloop()
