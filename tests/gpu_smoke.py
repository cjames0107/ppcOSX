#!/usr/bin/env python3
"""Smoke test for the ppcosx-gpu device.

Boots a G4 to the Open Firmware prompt, then uses Forth to poke the device's
registers, submit command batches, and paint the scanout, and uses the QEMU
monitor to capture what the host would present.
"""

import os
import re
import socket
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.realpath(__file__)))
QEMU = os.path.join(ROOT, "build", "qemu-system-ppc")
MON_PORT = 45454
BATCH = 0x01000000          # guest RAM used for command batches
FULL = 0x081801              # GL format: alpha, 24-bit depth, 8-bit stencil


class Guest:
    def __init__(self):
        self.proc = subprocess.Popen(
            [QEMU, "-M", "mac99,via=pmu", "-cpu", "7400_v2.9", "-m", "512",
             "-device", "ppcosx-gpu,id=gpu0", "-nographic",
             "-monitor", f"tcp:127.0.0.1:{MON_PORT},server,nowait"],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, bufsize=0)
        os.set_blocking(self.proc.stdout.fileno(), False)
        self.read_until(r"\d > $", 30)

    def read_until(self, pattern, timeout=10):
        buf, end = "", time.time() + timeout
        while time.time() < end:
            chunk = self.proc.stdout.read()
            if chunk:
                buf += chunk.decode(errors="replace").replace("\r", "")
                if re.search(pattern, buf):
                    return buf
            else:
                time.sleep(0.05)
        raise TimeoutError(f"no {pattern!r} in:\n{buf}")

    def forth(self, line):
        """Run one Forth line; return what it printed as a list of ints."""
        assert len(line) < 78, "Open Firmware truncates long input lines"
        self.proc.stdin.write(line.encode() + b"\r")
        out = self.read_until(r" ok\n\d > $")
        printed = out.split(line, 1)[-1].rsplit(" ok", 1)[0]
        return [int(tok, 16) for tok in printed.split()]

    def monitor(self, command):
        with socket.create_connection(("127.0.0.1", MON_PORT)) as s:
            s.settimeout(3)
            s.sendall(command.encode() + b"\n")
            out, end = b"", time.time() + 3
            while time.time() < end:
                try:
                    data = s.recv(65536)
                except socket.timeout:
                    break
                if not data:
                    break
                out += data
                if out.count(b"(qemu)") >= 2:
                    break
            return out.decode(errors="replace").replace("\r", "")

    def close(self):
        self.proc.kill()
        self.proc.wait()


def check(name, got, want):
    ok = got == want
    print(f"{'PASS' if ok else 'FAIL'}  {name}: got {got}, want {want}")
    return ok


def main():
    g = Guest()
    ok = True
    try:
        pci = g.monitor("info pci")
        m = re.search(r"device\s+(\d+), function 0:\n\s+Display controller: "
                      r"PCI device 1234:7078.*?BAR0: .*? at (0x[0-9a-f]+)"
                      r".*?BAR2: .*? at (0x[0-9a-f]+)", pci, re.S)
        if not m:
            sys.exit("ppcosx-gpu not found in 'info pci':\n" + pci)
        slot, vram, mmio = int(m[1]), int(m[2], 16), int(m[3], 16)
        print(f"device at slot {slot}, vram {vram:#x}, mmio {mmio:#x}")

        def reg(off):
            return f"{mmio + off:x}"

        def submit(words):
            for i in range(0, len(words), 3):
                g.forth(" ".join(f"{w:x} {BATCH + 4 * (i + j):x} l!"
                                 for j, w in enumerate(words[i:i + 3])))
            g.forth(f"{BATCH:x} {reg(0x10)} l! 0 {reg(0x14)} l!")
            g.forth(f"{len(words) * 4:x} {reg(0x18)} l! 1 {reg(0x1c)} l!")
            return g.forth(f"{reg(0x20)} l@ u. {reg(0x2c)} l@ u.")

        g.forth("hex")
        ok &= check("magic, version, features",
                    g.forth(f"{reg(0)} l@ u. {reg(4)} l@ u. {reg(8)} l@ u."),
                    [0x50584750, 8, 3])

        scanout = [1, 0x18, 640, 480, 640 * 4, 0]
        ok &= check("batch without bus mastering -> BAD_BATCH",
                    submit(scanout), [1, 0])

        # Enable memory space + bus master through the UniNorth config
        # mechanism (little-endian address/data ports).
        addr = ((1 << slot) | 4).to_bytes(4, "little").hex()
        g.forth(f"{addr} f2800000 l! 0600 f2c00004 w!")

        ok &= check("SET_SCANOUT 640x480 -> OK, 1 done",
                    submit(scanout), [0, 1])
        ok &= check("irq status set, then cleared by write-1",
                    g.forth(f"{reg(0x24)} l@ u. 1 {reg(0x24)} l! "
                            f"{reg(0x24)} l@ u."), [1, 0])

        g.forth(f"{vram:x} {640 * 480 * 4:x} ff fill")
        g.forth(f"00ff0000 {vram:x} l!")            # top-left pixel red
        with tempfile.TemporaryDirectory() as d:
            shot = os.path.join(d, "shot.ppm")
            g.monitor(f"screendump {shot} gpu0")
            data = open(shot, "rb").read()
        magic, dims, _, pixels = data.split(b"\n", 3)
        ok &= check("scanout capture size", dims, b"640 480")
        ok &= check("first pixel is red (big-endian xRGB)",
                    pixels[:3], b"\xff\x00\x00")
        ok &= check("remaining pixels white",
                    set(pixels[3:]), {0xff})

        # GL: clear a 64x48 context to green on the host GPU and present it
        # at (10, 20) on the scanout.
        one = 0x3f800000
        ok &= check("GL call with no bound context -> BAD_ARG",
                    submit([0x1001, 0xc, 0x4000]), [2, 1])
        ok &= check("GL create + bind + clear + present -> OK",
                    submit([0x10, 0x18, 1, 64, 48, FULL,
                            0x12, 0xc, 1,
                            0x1000, 0x18, 0, one, 0, one,
                            0x1001, 0xc, 0x4000,
                            0x13, 0x10, 10, 20]), [0, 6])
        ok &= check("binding does not outlive the batch",
                    submit([0x1001, 0xc, 0x4000]), [2, 6])
        with tempfile.TemporaryDirectory() as d:
            shot = os.path.join(d, "shot.ppm")
            g.monitor(f"screendump {shot} gpu0")
            pixels = open(shot, "rb").read().split(b"\n", 3)[3]

        def pixel(x, y):
            return pixels[(y * 640 + x) * 3:(y * 640 + x) * 3 + 3]

        green, white = b"\x00\xff\x00", b"\xff\xff\xff"
        ok &= check("GL image corners are green",
                    [pixel(10, 20), pixel(73, 67)], [green, green])
        ok &= check("pixels just outside are untouched",
                    [pixel(9, 20), pixel(74, 67), pixel(10, 68)],
                    [white, white, white])
        # Resize to 8x4, clear to blue with alpha 0, read back into VRAM at
        # 32 MB: the alpha survives only if the format has an alpha channel.
        rb = 0x2000000
        ok &= check("GL resize + clear + readback -> OK",
                    submit([0x14, 0x18, 1, 8, 4, FULL,
                            0x12, 0xc, 1,
                            0x1000, 0x18, 0, 0, one, 0,
                            0x1001, 0xc, 0x4000,
                            0x15, 0xc, rb]), [0, 11])
        ok &= check("with alpha: readback is ARGB blue, alpha 0",
                    g.forth(f"{vram + rb:x} l@ u. {vram + rb + 31 * 4:x} l@ u."),
                    [0x000000ff, 0x000000ff])
        ok &= check("without alpha: same clear reads back opaque",
                    submit([0x14, 0x18, 1, 8, 4, 0,
                            0x12, 0xc, 1,
                            0x1000, 0x18, 0, 0, one, 0,
                            0x1001, 0xc, 0x4000,
                            0x15, 0xc, rb]) +
                    g.forth(f"{vram + rb:x} l@ u."), [0, 16, 0xff0000ff])
        # Overlay: a 16x8 green frame for context 1 placed at (100, 50),
        # with the key colour painted only into its left 8x8 half.
        ok &= check("overlay frame + place -> OK",
                    submit([0x14, 0x18, 1, 16, 8, FULL,
                            0x12, 0xc, 1,
                            0x1000, 0x18, 0, one, 0, one,
                            0x1001, 0xc, 0x4000,
                            0x17, 0x8,
                            0x16, 0x20, 1, 100, 50, 16, 8, 1]), [0, 22])
        base = vram + (50 * 640 + 100) * 4
        g.forth(f"8 0 do 8 0 do ff01ff {base:x} j a00 * + i 4 * + l! loop loop")

        def shot_pixels():
            with tempfile.TemporaryDirectory() as d:
                path = os.path.join(d, "shot.ppm")
                g.monitor(f"screendump {path} gpu0")
                time.sleep(0.2)
                return open(path, "rb").read().split(b"\n", 3)[3]

        time.sleep(0.5)                     # let the display refresh
        pixels = shot_pixels()
        ok &= check("overlay shows only where the key is",
                    [pixel(100, 50), pixel(107, 57), pixel(108, 50),
                     pixel(99, 50), pixel(100, 58)],
                    [green, green, white, white, white])
        ok &= check("hiding the overlay reveals the key colour",
                    submit([0x16, 0x20, 1, 100, 50, 16, 8, 0]), [0, 23])
        time.sleep(0.5)
        pixels = shot_pixels()
        ok &= check("key pixels show as themselves again",
                    [pixel(100, 50), pixel(108, 50)],
                    [b"\xff\x01\xff", white])
        # glReadPixels from the 16x8 green context: bytes, a depth float,
        # and 16-bit components (which must arrive big-endian).
        ok &= check("READPIXELS colour, depth, 16-bit -> OK",
                    submit([0x12, 0xc, 1,
                            0x1001, 0xc, 0x4100,
                            0x18, 0x24, rb, 0, 0, 2, 1, 0x1908, 0x1401,
                            0x18, 0x24, rb + 16, 0, 0, 1, 1, 0x1902, 0x1406,
                            0x18, 0x24, rb + 32, 0, 0, 1, 1, 0x1908, 0x1403]),
                    [0, 28])
        ok &= check("RGBA bytes, depth 1.0, RGBA shorts",
                    g.forth(f"{vram + rb:x} l@ u. {vram + rb + 4:x} l@ u. "
                            f"{vram + rb + 16:x} l@ u.") +
                    g.forth(f"{vram + rb + 32:x} l@ u. "
                            f"{vram + rb + 36:x} l@ u."),
                    [0x00ff00ff, 0x00ff00ff, 0x3f800000,
                     0x0000ffff, 0x0000ffff])
        ok &= check("READPIXELS bigger than VRAM -> BAD_ARG",
                    submit([0x12, 0xc, 1,
                            0x18, 0x24, 0x3fffff0, 0, 0, 16, 8, 0x1908,
                            0x1401]), [2, 29])
        ok &= check("readback past end of vram -> BAD_ARG",
                    submit([0x12, 0xc, 1, 0x15, 0xc, 0x3fffff0]), [2, 30])
        ok &= check("GL destroy, then bind fails",
                    submit([0x11, 0xc, 1]) + submit([0x12, 0xc, 1]),
                    [0, 31, 2, 31])

        ok &= check("NOP + NOP -> OK, 3 done",
                    submit([0, 8, 0, 8]), [0, 33])
        ok &= check("unknown opcode -> UNSUPPORTED",
                    submit([0x2000, 8]), [3, 33])
        ok &= check("scanout past end of vram -> BAD_ARG",
                    submit([1, 0x18, 640, 480, 640 * 4, 0xfffffffc]), [2, 33])
        ok &= check("command length overruns batch -> BAD_BATCH",
                    submit([0, 0x40]), [1, 33])
    finally:
        g.close()
    print("ALL PASS" if ok else "FAILURES")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
