# Simple GUI Launcher for bmdkey

# A clean way to control `bmdkey` is:

# * keep `bmdkey` as the backend executable
# * create a small GUI wrapper that:

  # * launches it
  # * stops it
  # * shows status
  
  #IMPORTANT:
  
  #change the path for the bmdkey executable at subprocess.Popen 



import subprocess
import signal
import tkinter as tk
from tkinter import ttk

process = None


def start_bmdkey():
    global process

    if process is None:
        process = subprocess.Popen([
            "/home/chilam/bmdkey-master/bmdkey"
        ])

        status_label.config(text="bmdkey is RUNNING")


def stop_bmdkey():
    global process

    if process is not None:
        process.terminate()
        process.wait()
        process = None

        status_label.config(text="bmdkey is STOPPED")


def toggle_bmdkey():
    if process is None:
        start_bmdkey()
    else:
        stop_bmdkey()


root = tk.Tk()
root.title("bmdkey controller")
root.geometry("300x140")

frame = ttk.Frame(root, padding=20)
frame.pack(fill="both", expand=True)

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


root.protocol("WM_DELETE_WINDOW", lambda: (
    stop_bmdkey(),
    root.destroy()
))

root.mainloop()
