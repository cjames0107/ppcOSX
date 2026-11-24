#!/usr/bin/env python3
"""Drive a running guest through QEMU's QMP socket.

  guestctl.py shot OUT.png            capture the guest screen
  guestctl.py click X Y [right]       click at screen pixel X,Y
  guestctl.py drag X1 Y1 X2 Y2 [N]    drag with the left button in N steps
  guestctl.py move X Y                move the pointer
  guestctl.py key ret | meta_l-q ...  press a key chord (QEMU qcode names)
  guestctl.py type "text"             type ASCII text
  guestctl.py hmp "info status"       run a monitor command

Pointer commands need the guest started with `-device usb-tablet`.
"""

import json
import os
import socket
import sys
import time

PORT = int(os.environ.get("PPCOSX_QMP_PORT", "45456"))
WIDTH = int(os.environ.get("PPCOSX_WIDTH", "1024"))
HEIGHT = int(os.environ.get("PPCOSX_HEIGHT", "768"))

SHIFTED = {
    "!": "1", "@": "2", "#": "3", "$": "4", "%": "5", "^": "6", "&": "7",
    "*": "8", "(": "9", ")": "0", "_": "minus", "+": "equal", ":": "semicolon",
    '"': "apostrophe", "<": "comma", ">": "dot", "?": "slash", "~": "grave_accent",
    "{": "bracket_left", "}": "bracket_right", "|": "backslash",
}
PLAIN = {
    " ": "spc", "-": "minus", "=": "equal", ";": "semicolon", "'": "apostrophe",
    ",": "comma", ".": "dot", "/": "slash", "`": "grave_accent",
    "[": "bracket_left", "]": "bracket_right", "\\": "backslash", "\n": "ret",
    "\t": "tab",
}


class QMP:
    def __init__(self):
        self.sock = socket.create_connection(("127.0.0.1", PORT))
        self.file = self.sock.makefile("rw")
        self.file.readline()                    # greeting
        self.cmd("qmp_capabilities")

    def cmd(self, execute, **arguments):
        msg = {"execute": execute}
        if arguments:
            msg["arguments"] = arguments
        self.file.write(json.dumps(msg) + "\n")
        self.file.flush()
        while True:
            reply = json.loads(self.file.readline())
            if "return" in reply:
                return reply["return"]
            if "error" in reply:
                sys.exit(f"QMP error: {reply['error']['desc']}")

    def events(self, events):
        self.cmd("input-send-event", events=events)

    def move(self, x, y):
        self.events([
            {"type": "abs", "data": {"axis": "x",
                                     "value": x * 0x7fff // (WIDTH - 1)}},
            {"type": "abs", "data": {"axis": "y",
                                     "value": y * 0x7fff // (HEIGHT - 1)}},
        ])

    def button(self, name, down):
        self.events([{"type": "btn", "data": {"button": name, "down": down}}])

    def chord(self, qcodes):
        keys = [{"type": "qcode", "data": q} for q in qcodes]
        self.cmd("send-key", keys=keys, **{"hold-time": 80})
        time.sleep(0.15)


def main(argv):
    if len(argv) < 2:
        sys.exit(__doc__)
    q, op, args = QMP(), argv[1], argv[2:]
    if op == "shot":
        q.cmd("screendump", filename=os.path.abspath(args[0]), format="png")
    elif op == "move":
        q.move(int(args[0]), int(args[1]))
    elif op == "click":
        name = args[2] if len(args) > 2 else "left"
        q.move(int(args[0]), int(args[1]))
        time.sleep(0.2)
        q.button(name, True)
        time.sleep(0.1)
        q.button(name, False)
    elif op == "drag":
        # drag X1 Y1 X2 Y2 [STEPS]: press at (X1, Y1), move, release
        x1, y1, x2, y2 = (int(a) for a in args[:4])
        steps = int(args[4]) if len(args) > 4 else 20
        q.move(x1, y1)
        time.sleep(0.3)
        q.button("left", True)
        time.sleep(0.3)
        for i in range(1, steps + 1):
            q.move(x1 + (x2 - x1) * i // steps, y1 + (y2 - y1) * i // steps)
            time.sleep(0.08)
        time.sleep(0.3)
        q.button("left", False)
    elif op == "key":
        for chord in args:
            q.chord(chord.split("-"))
    elif op == "type":
        for ch in args[0]:
            if ch in SHIFTED:
                q.chord(["shift", SHIFTED[ch]])
            elif ch in PLAIN:
                q.chord([PLAIN[ch]])
            elif ch.isupper():
                q.chord(["shift", ch.lower()])
            else:
                q.chord([ch])
    elif op == "hmp":
        print(q.cmd("human-monitor-command", **{"command-line": args[0]}))
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main(sys.argv)
