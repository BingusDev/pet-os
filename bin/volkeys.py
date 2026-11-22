#!/usr/bin/env python3
# CardputerZero volume keys: fn+D / fn+S / fn+A arrive as KEY_VOLUMEUP / KEY_VOLUMEDOWN /
# KEY_MUTE on the tca8418c keypad. Map them to PipeWire (5% steps, capped at 100%) and
# write "<percent> <muted>" to $XDG_RUNTIME_DIR/alicepet-volume for AlicePET's volume bar.
import glob, os, re, struct, subprocess, time

EV_KEY, MUTE, DOWN, UP = 1, 113, 114, 115
SINK = "@DEFAULT_AUDIO_SINK@"
ACT = {UP: ["wpctl", "set-volume", "-l", "1.0", SINK, "5%+"],
       DOWN: ["wpctl", "set-volume", SINK, "5%-"],
       MUTE: ["wpctl", "set-mute", SINK, "toggle"]}
STATE = os.path.join(os.environ.get("XDG_RUNTIME_DIR", "/tmp"), "alicepet-volume")


def keypad():
    for p in glob.glob("/sys/class/input/event*/device/name"):
        if open(p).read().strip() == "tca8418c":
            return "/dev/input/" + p.split("/")[4]
    raise FileNotFoundError("tca8418c")


def publish():
    out = subprocess.run(["wpctl", "get-volume", SINK], capture_output=True, text=True).stdout
    m = re.search(r"([\d.]+)", out)
    if not m:
        return
    tmp = STATE + ".tmp"
    with open(tmp, "w") as f:
        f.write(f"{round(float(m.group(1)) * 100)} {1 if 'MUTED' in out else 0}\n")
    os.replace(tmp, STATE)


while True:
    try:
        with open(keypad(), "rb") as f:
            while True:
                _, _, kind, code, value = struct.unpack("llHHi", f.read(24))
                if kind == EV_KEY and code in ACT and value in (1, 2):
                    subprocess.run(ACT[code])
                    publish()
    except Exception:
        time.sleep(2)
