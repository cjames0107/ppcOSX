#!/usr/bin/env python3
"""Sample where a running (possibly hung) 32-bit PowerPC guest is executing.

  guest-stack.py [samples [interval]]

Takes `samples` snapshots (default 5) of the program counter and link
register over QMP, then walks the stack back-chain of the last one using
the Darwin PowerPC ABI (back chain at 0(r1), saved LR at 8(caller r1)).
Addresses only; match them against `kextstat` load addresses or the
kernel's symbols to name them.
"""

import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.realpath(__file__)))
from guestctl import QMP  # noqa: E402


def hmp(q, command):
    return q.cmd("human-monitor-command", **{"command-line": command})


def registers(q):
    text = hmp(q, "info registers")
    regs = {}
    for name in ("NIP", "LR", "MSR"):
        m = re.search(name + r"\s+([0-9a-fA-F]+)", text)
        if m:
            regs[name] = int(m.group(1), 16)
    # Printed 64 bits wide even for 32-bit CPUs; keep the low word.
    gprs = re.findall(r"GPR(\d\d)((?:[ \t]+[0-9a-fA-F]{8,16}){1,8})", text)
    for first, values in gprs:
        for i, v in enumerate(values.split()):
            regs[f"r{int(first) + i}"] = int(v, 16) & 0xffffffff
    return regs


def read_word(q, addr):
    # Stack slots are word-aligned; anything else is not a stack address,
    # and monitor reads of device memory can upset the emulator.
    if addr & 3:
        return None
    out = hmp(q, f"x /1xw 0x{addr:08x}")
    m = re.search(r":\s*0x([0-9a-fA-F]+)", out)
    return int(m.group(1), 16) if m else None


def main():
    samples = int(sys.argv[1]) if len(sys.argv) > 1 else 5
    interval = float(sys.argv[2]) if len(sys.argv) > 2 else 0.5
    q = QMP()
    regs = {}
    for _ in range(samples):
        regs = registers(q)
        print(f"NIP {regs.get('NIP', 0):08x}  LR {regs.get('LR', 0):08x}  "
              f"MSR {regs.get('MSR', 0):08x}  r1 {regs.get('r1', 0):08x}")
        time.sleep(interval)

    print("back chain:")
    sp = regs.get("r1")
    for depth in range(24):
        if not sp:
            break
        caller = read_word(q, sp)
        if not caller or caller <= sp or caller - sp > 0x100000:
            break
        lr = read_word(q, caller + 8)
        print(f"  #{depth} sp {sp:08x} -> return {lr or 0:08x}")
        sp = caller


if __name__ == "__main__":
    main()
