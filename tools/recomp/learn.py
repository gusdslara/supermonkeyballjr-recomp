#!/usr/bin/env python3
"""Feeds a runtime miss list back into the recompiler's inputs.

  smbjr rom.gba --test 3600 --misses build/misses.txt      (or play normally, then quit)
  python tools/recomp/learn.py --rom rom.gba --misses build/misses.txt

Adds every address that ran on the interpreter to entries.txt. For code that runs from RAM it also
finds where the game loads it from (a raw copy or an LZ77 stream in the ROM) using the RAM snapshot
written next to the miss list, and records that in ramcode.txt. Both files only hold addresses, so
they can be committed; the code itself is always read from the player's ROM at build time.
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gbarom import ROM_BASE, lz77_blobs, read_ramcode  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
REGIONS = {0x02: ("ewram", 0x40000), 0x03: ("iwram", 0x8000)}
WINDOW = 32


_blobs = []


def blobs_cache(rom):
    if not _blobs:
        _blobs.extend(lz77_blobs(rom))
    return _blobs


def find_source(rom, dump, off, addr, probe, blobs):
    # 1) a raw copy: grow the identical run around the address in both directions.
    i = rom.find(probe)
    if i >= 0:
        lo, hi = 0, len(probe)
        while off - lo - 1 >= 0 and i - lo - 1 >= 0 and dump[off - lo - 1] == rom[i - lo - 1]:
            lo += 1
        while off + hi < len(dump) and i + hi < len(rom) and dump[off + hi] == rom[i + hi]:
            hi += 1
        return "%08X raw %08X %X" % (addr - lo, ROM_BASE + i - lo, lo + hi)
    # 2) an LZ77 stream (BIOS LZ77UnComp) that unpacks to these bytes.
    for src, data in blobs():
        p = data.find(probe)
        if p >= 0:
            return "%08X lz77 %08X" % (addr - p, src)
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rom", required=True)
    ap.add_argument("--misses", required=True)
    ap.add_argument("--entries", default=os.path.join(HERE, "entries.txt"))
    ap.add_argument("--ramcode", default=os.path.join(HERE, "ramcode.txt"))
    args = ap.parse_args()

    rom = open(args.rom, "rb").read()
    dumps = {r: [] for r in REGIONS}  # RAM snapshots written by the runtime: misses.txt.iwram.N
    for r, (name, size) in REGIONS.items():
        for n in range(64):
            p = "%s.%s.%d" % (args.misses, name, n)
            if os.path.exists(p):
                dumps[r].append(open(p, "rb").read())

    known = set()
    lines = open(args.entries, encoding="utf-8").read().splitlines() if os.path.exists(args.entries) else []
    for l in lines:
        f = l.split("#")[0].split()
        if f:
            known.add((f[0], int(f[1], 16)))
    ramcode = read_ramcode(args.ramcode, rom)  # list of (base, data, spec)

    def covered(addr, probe):
        for b, d, _ in ramcode:
            if b <= addr and addr + len(probe) <= b + len(d) and d[addr - b: addr - b + len(probe)] == probe:
                return True
        return False

    new_entries, new_maps, unresolved = [], [], []
    for l in open(args.misses, encoding="utf-8"):
        f = l.split("#")[0].split()
        if not f:
            continue
        mode, addr = f[0], int(f[1], 16)
        if (mode, addr) not in known:
            known.add((mode, addr))
            new_entries.append((mode, addr))
        r = addr >> 24
        if r not in REGIONS:
            continue
        resolved = False
        for dump in dumps[r]:
            off = addr & (REGIONS[r][1] - 1)
            probe = dump[off: off + WINDOW]
            if len(probe) < 8 or probe.count(0) == len(probe):
                continue
            if covered(addr, probe):
                resolved = True
                continue
            spec = find_source(rom, dump, off, addr, probe, lambda: blobs_cache(rom))
            if spec:
                new_maps.append(spec)
                ramcode = read_ramcode(None, rom, extra=[spec]) + ramcode
                resolved = True
        if not resolved:
            unresolved.append(addr)

    if new_entries:
        with open(args.entries, "a", encoding="utf-8") as f:
            for mode, addr in sorted(new_entries, key=lambda e: e[1]):
                f.write("%s %08X\n" % (mode, addr))
    if new_maps:
        with open(args.ramcode, "a", encoding="utf-8") as f:
            for m in new_maps:
                f.write(m + "\n")
    print("learn: %d new entries, %d new RAM code sources, %d RAM addresses without a ROM source"
          % (len(new_entries), len(new_maps), len(unresolved)))
    for a in unresolved[:20]:
        print("  unresolved %08X" % a)


if __name__ == "__main__":
    main()
