#!/usr/bin/env python3
"""A fake Ten-Tec Orion on a pty, for running AetherSDR without the radio.

    python3 tests/orion/fake_orion.py /tmp/fake-orion

Opens a pty, symlinks its slave to the given path, and answers the CAT
queries the Orion backend polls (frequency, mode, filter, AGC, S-meter,
VFO routing, front end) from a small state machine. Every '*' set command is
applied to that state and printed. Point AetherSDR's "Ten-Tec Orion
(serial)" connection at the same path.

It is a receiver only. A transmit command (*TK, *TU, *T...) or a CW
character (/c) is never acted on; it is printed with a warning so a test
run that should never key makes the mistake visible (fork rule 3).

Keys while running (type, then Enter):
    f <hz>   move dial A as if the front-panel knob turned
    b <hz>   move dial B
    s <raw>  set the main S-meter raw value (0..300; 94 = S9)
    q        quit
"""

from __future__ import annotations

import os
import random
import select
import sys
import termios
import tty


class Orion:
    def __init__(self) -> None:
        self.freq = {"A": 14_025_000, "B": 7_040_000}
        self.mode = {"M": 0, "S": 0}
        self.bw = {"M": 2400, "S": 2400}
        self.pbt = {"M": 0, "S": 0}
        self.agc = {"M": "M", "S": "S"}
        self.rf_gain = 80
        self.atten = 0
        self.preamp = 1
        self.kv = "ABA"
        self.s_main = 94

    def answer(self, q: str) -> str | None:
        if q in ("AF", "BF"):
            return f"@{q}{self.freq[q[0]]}"
        if q == "S":
            jitter = random.randint(-3, 3)
            return f"@SRM{max(0, self.s_main + jitter)}S{random.randint(5, 20)}"
        if q == "KV":
            return f"@KV{self.kv}"
        if len(q) == 3 and q[0] == "R" and q[1] in "MS":
            rx, field = q[1], q[2]
            if field == "M":
                return f"@R{rx}M{self.mode[rx]}"
            if field == "F":
                return f"@R{rx}F{self.bw[rx]}"
            if field == "P":
                return f"@R{rx}P{self.pbt[rx]}"
            if field == "A":
                return f"@R{rx}A{self.agc[rx]}"
            if field == "G":
                return f"@R{rx}G{self.rf_gain}"
            if field == "T":
                return f"@R{rx}T{self.atten}"
            if field == "E":
                return f"@R{rx}E{self.preamp}"
        return None

    def apply(self, cmd: str) -> None:
        body = cmd[1:]
        try:
            if body[:2] in ("AF", "BF"):
                hz = int(body[2:])
                if 100_000 <= hz <= 60_000_000:
                    self.freq[body[0]] = hz
            elif body[0] == "R" and body[1] in "MS":
                rx, field, value = body[1], body[2], body[3:]
                if field == "M":
                    self.mode[rx] = int(value)
                elif field == "F":
                    self.bw[rx] = int(value)
                elif field == "P":
                    self.pbt[rx] = int(value)
                elif field == "A" and value in ("F", "M", "S", "O"):
                    self.agc[rx] = value
                elif field == "G":
                    self.rf_gain = int(value)
                elif field == "T":
                    self.atten = int(value)
                elif field == "E":
                    self.preamp = int(value)
            elif body.startswith("KV") and len(body) == 5:
                self.kv = body[2:]
        except (ValueError, IndexError):
            print(f"  (malformed set ignored: {cmd!r})")


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    link = sys.argv[1]
    master, slave = os.openpty()
    tty.setraw(slave)
    attrs = termios.tcgetattr(slave)
    attrs[3] &= ~termios.ECHO
    termios.tcsetattr(slave, termios.TCSANOW, attrs)
    slave_name = os.ttyname(slave)
    if os.path.islink(link):
        os.unlink(link)
    os.symlink(slave_name, link)
    print(f"fake Orion on {slave_name}, linked at {link}. Ctrl-C or 'q' to quit.")

    radio = Orion()
    buffer = b""
    watch = [master, sys.stdin]
    try:
        while True:
            ready, _, _ = select.select(watch, [], [])
            if sys.stdin in ready:
                text = sys.stdin.readline()
                if text == "":
                    watch = [master]   # stdin closed (run in the background)
                    continue
                line = text.strip().split()
                if not line:
                    pass
                elif line[0] == "q":
                    break
                elif line[0] in ("f", "b") and len(line) == 2:
                    radio.freq["A" if line[0] == "f" else "B"] = int(line[1])
                    print(f"  dial {'A' if line[0] == 'f' else 'B'} -> {line[1]}")
                elif line[0] == "s" and len(line) == 2:
                    radio.s_main = max(0, min(300, int(line[1])))
            if master in ready:
                try:
                    buffer += os.read(master, 4096)
                except OSError:
                    continue
                while b"\r" in buffer:
                    raw, buffer = buffer.split(b"\r", 1)
                    cmd = raw.decode("ascii", "replace").strip()
                    if not cmd:
                        continue
                    if cmd.startswith("*T") or cmd.startswith("/"):
                        print(f"  !!! TRANSMIT/CW COMMAND RECEIVED AND IGNORED: {cmd!r}")
                        continue
                    if cmd.startswith("*"):
                        print(f"  set {cmd}")
                        radio.apply(cmd)
                    elif cmd.startswith("?"):
                        reply = radio.answer(cmd[1:])
                        if reply is not None:
                            os.write(master, reply.encode("ascii") + b"\r")
    except KeyboardInterrupt:
        pass
    finally:
        if os.path.islink(link):
            os.unlink(link)
    return 0


if __name__ == "__main__":
    sys.exit(main())
