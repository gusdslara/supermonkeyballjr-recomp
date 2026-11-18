"""ROM helpers shared by recomp.py and learn.py: LZ77 (GBA BIOS format) and RAM code sources."""
import os

ROM_BASE = 0x08000000


def lz77(rom, off, limit=0x40000):
    """Decodes a BIOS LZ77UnComp stream (header 0x10, 24-bit size). None if it is not one."""
    if off + 4 > len(rom) or rom[off] != 0x10:
        return None
    size = rom[off + 1] | rom[off + 2] << 8 | rom[off + 3] << 16
    if size == 0 or size > limit:
        return None
    out = bytearray()
    i = off + 4
    try:
        while len(out) < size:
            flags = rom[i]
            i += 1
            for b in range(8):
                if len(out) >= size:
                    break
                if flags & (0x80 >> b):
                    x = rom[i] << 8 | rom[i + 1]
                    i += 2
                    n, disp = (x >> 12) + 3, (x & 0xFFF) + 1
                    if disp > len(out):
                        return None
                    for _ in range(n):
                        out.append(out[-disp])
                else:
                    out.append(rom[i])
                    i += 1
    except IndexError:
        return None
    return bytes(out[:size])


def lz77_blobs(rom, min_size=64):
    """Every plausible LZ77 stream in the ROM: [(rom_address, data)]."""
    out = []
    for off in range(0, len(rom) - 4, 4):
        if rom[off] == 0x10:
            d = lz77(rom, off)
            if d and len(d) >= min_size:
                out.append((ROM_BASE + off, d))
    return out


def parse_ramcode(spec, rom):
    """'BASE raw ROMADDR LEN' or 'BASE lz77 ROMADDR' -> (base, data) or None."""
    f = spec.split()
    base, kind, src = int(f[0], 16), f[1], int(f[2], 16) - ROM_BASE
    if kind == "raw":
        return base, rom[src: src + int(f[3], 16)]
    if kind == "lz77":
        d = lz77(rom, src)
        return (base, d) if d else None
    raise ValueError("ramcode: unknown kind %r" % kind)


def read_ramcode(path, rom, extra=()):
    """ramcode.txt -> [(base, data, spec)]."""
    specs = list(extra)
    if path and os.path.exists(path):
        for l in open(path, encoding="utf-8"):
            l = l.split("#")[0].strip()
            if l:
                specs.append(l)
    out = []
    for s in specs:
        r = parse_ramcode(s, rom)
        if r:
            out.append((r[0], r[1], s))
    return out
