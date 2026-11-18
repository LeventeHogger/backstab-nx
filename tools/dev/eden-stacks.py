#!/usr/bin/env python3
"""Pillanatkép a futó játék szálairól az Eden GDB-szerverén át (saját, minimális RSP-kliens).

N másodperc futás után megállítja a folyamatot, és szálanként kiírja a PC/LR/SP-t, r0–r3-at és a
veremben talált kódcímeket; a libPirates.so-ba eső címeket a játék dinamikus szimbólumaira fordítja,
a loader saját címeit a backstab.elf szimbólumaira.

Előfeltétel: Eden-beállítás Debugging → use_gdbstub=true (port 6543); az Eden ekkor indításkor vár.
  python3 tools/dev/eden-stacks.py <másodperc> <build/backstab.elf> <libPirates.so> <log.txt> [ismétlés]
"""
import bisect
import re
import socket
import subprocess
import sys
import time

secs, elf, so_path, log_path = int(sys.argv[1]), sys.argv[2], sys.argv[3], sys.argv[4]
repeat = int(sys.argv[5]) if len(sys.argv) > 5 else 1


def symtab(path, dynamic):
    args = ["arm-none-eabi-nm", "--defined-only", "-C"] + (["-D"] if dynamic else []) + [path]
    out = subprocess.check_output(args, text=True, errors="replace")
    t = []
    for line in out.splitlines():
        p = line.split(" ", 2)
        if len(p) == 3 and p[1] in "tTwW":
            t.append((int(p[0], 16) & ~1, p[2]))
    t.sort()
    return t, [x[0] for x in t]


game_syms, game_offs = symtab(so_path, True)
ldr_syms, ldr_offs = symtab(elf, False)


class Rsp:
    def __init__(self, port=6543):
        for _ in range(60):
            try:
                self.s = socket.create_connection(("127.0.0.1", port))
                break
            except OSError:
                time.sleep(1)
        else:
            sys.exit("nem sikerült csatlakozni")
        self.buf = b""

    def _read_packet(self, timeout=30):
        self.s.settimeout(timeout)
        while True:
            i = self.buf.find(b"$")
            j = self.buf.find(b"#", i + 1) if i >= 0 else -1
            if i >= 0 and j >= 0 and len(self.buf) >= j + 3:
                data = self.buf[i + 1:j]
                self.buf = self.buf[j + 3:]
                self.s.sendall(b"+")
                return data.decode(errors="replace")
            chunk = self.s.recv(65536)
            if not chunk:
                raise EOFError
            self.buf += chunk

    def cmd(self, data, wait=True):
        pkt = f"${data}#{sum(data.encode()) & 0xFF:02x}".encode()
        self.s.sendall(pkt)
        return self._read_packet() if wait else None


r = Rsp()
r.cmd("qSupported:swbreak+;hwbreak+")
r.cmd("?")

base = size = None
ldr_base = 0
ldr_main = next(a for a, n in ldr_syms if n == "main")


def load_base():
    global base, size, ldr_base
    try:
        log = open(log_path).read()
    except OSError:
        return
    m = re.search(r"so_file_load: .* @ (0x[0-9a-f]+), (\d+) KB", log)
    if m:
        base, size = int(m.group(1), 16), int(m.group(2)) * 1024
    m = re.search(r"main @ (0x[0-9a-f]+)", log)
    if m:
        ldr_base = int(m.group(1), 16) - ldr_main


def name(addr):
    if base is not None and base <= addr < base + size:
        off = addr - base
        i = bisect.bisect_right(game_offs, off) - 1
        s = f"{game_syms[i][1]}+0x{off - game_syms[i][0]:x}" if i >= 0 else "?"
        return f"0x{addr:08x} [game+0x{off:x}] {s}"
    a = (addr & ~1) - ldr_base
    i = bisect.bisect_right(ldr_offs, a) - 1
    if i >= 0 and a - ldr_syms[i][0] < 0x10000:
        return f"0x{addr:08x} [loader] {ldr_syms[i][1]}+0x{a - ldr_syms[i][0]:x}"
    return f"0x{addr:08x}"


def is_code(v):
    if base is not None and base <= v < base + size:
        return True
    a = (v & ~1) - ldr_base
    i = bisect.bisect_right(ldr_offs, a) - 1
    return i >= 0 and a - ldr_syms[i][0] < 0x4000 and 0 <= a < ldr_offs[-1] + 0x4000


for rep in range(repeat):
    r.cmd("c", wait=False)
    time.sleep(secs)
    r.s.sendall(b"\x03")
    print(f"\n######## pillanatkép {rep + 1} – megállás: {r._read_packet()}")
    load_base()
    tids = []
    resp = r.cmd("qfThreadInfo")
    while resp.startswith("m"):
        tids += resp[1:].split(",")
        resp = r.cmd("qsThreadInfo")
    for tid in tids:
        r.cmd(f"Hg{tid}")
        g = r.cmd("g")
        regs = [int.from_bytes(bytes.fromhex(g[k * 8:k * 8 + 8]), "little") for k in range(16)]
        sp, lr, pc = regs[13], regs[14], regs[15]
        print(f"\n== szál {tid}\n   pc={name(pc)}\n   lr={name(lr)}\n   sp=0x{sp:08x} "
              + " ".join(f"r{k}=0x{regs[k]:08x}" for k in range(4)))
        # a verem tetején túl nincs leképezett memória: 0x100-as darabokban olvasunk, amíg megy
        mem = ""
        k = 0
        while k < 0x2000:
            n = 0x100 - ((sp + k) & 0xFF)  # ne lépjünk át 0x100-as határt egy kérésben
            part = r.cmd(f"m{sp + k:x},{n:x}")
            if not part or part.startswith("E"):
                break
            mem += part
            k += n
        print(f"   (verem: 0x{len(mem) // 2:x} bájt olvasható)")
        hits = []
        if mem:
            raw = bytes.fromhex(mem)
            for k in range(0, len(raw) - 3, 4):
                v = int.from_bytes(raw[k:k + 4], "little")
                if is_code(v):
                    hits.append(f"sp+0x{k:x}: {name(v)}")
        for h in hits[:24]:
            print("   " + h)
r.s.sendall(b"$k#6b")
