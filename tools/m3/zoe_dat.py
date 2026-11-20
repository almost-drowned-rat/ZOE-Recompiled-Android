"""ZOE.DAT - the game's one data archive - listed, and its stage code (*.bin) extracted, straight from the ISO.

Rule 2026-11-12 (design 7d): code a game loads at run time is found on the disc before the build, not in play
tests. ZOE keeps everything in ZOE.DAT (ISO file, LBA 300 in both versions). Its table: 32-byte entries
`name[16], a, b, flags, 0` (little endian):
  * a folder (flags 0x0102 in the byte order shown by a hex dump: `02 01 00 00`): a = number of entries,
    b = sector (from the start of ZOE.DAT) of the folder's own table;
  * a file (flags 0): a = sector of the data, b = size in bytes.
The root table is at sector 0 (entry 0 = the root itself: a = 2 entries following). Stage folders (fa1, gl1, ...)
hold movies (.pss), .pak, .mts, .sdx, .scx and one stage stub <stage>.bin - straight-line MIPS that registers the
stage's callbacks (ZOE-Recompiled docs/notes/02_zoe_internals.md, GPL-3: 22 files; format facts only).

    python -I tools/m3/zoe_dat.py <iso> list [--all]
    python -I tools/m3/zoe_dat.py <iso> extract-bins <out folder>

Extracted files are game material: they stay in the game's extracted/ folder, never committed.
"""

import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ENGINE = os.path.join(os.path.dirname(os.path.dirname(HERE)), "engine")
sys.path.insert(0, ENGINE)
from ps2decomp.iso9660 import Iso9660  # noqa: E402

SECTOR = 2048
FOLDER = 0x0102


def entries(iso, base, sector, count):
    data = iso.read_sectors(base + sector, (count * 32 + SECTOR - 1) // SECTOR)
    for i in range(count):
        raw = data[i * 32:(i + 1) * 32]
        name = raw[:16].split(b"\0")[0].decode("latin-1")
        a, b, flags, _ = struct.unpack_from("<4I", raw, 16)
        if (flags & 0xFFFF) == FOLDER and a == 0 and b == 0 and len(name) <= 8:
            # The root's own entry and its "stage" folder keep count and offset in bytes 8-15 (inside the
            # name field): `stage\0\0\0 17000000 60000000`.
            a, b = struct.unpack_from("<2I", raw, 8)
        yield name, a, b, flags & 0xFFFF


def walk(iso, base, sector, count, path="", skip=0):
    """(path, sector, size) for every file below the table at `sector` (its entries from index `skip` on)."""
    listed = list(entries(iso, base, sector, skip + count))[skip:]
    for name, a, b, flags in listed:
        if not name:
            continue
        if flags == FOLDER:
            if b < 0x1000 and b % 32 == 0:
                # The root's "stage" folder: b = 0x60 is a BYTE offset into this same table - its a entries
                # follow there (bs1, ca1, ... at entries 3..). Elsewhere b is the sector of the folder's table.
                yield from walk(iso, base, sector, a, path + name + "/", b // 32)
            else:
                yield from walk(iso, base, b, a, path + name + "/")
        else:
            yield path + name, a, b


def main():
    iso_path, command = sys.argv[1], sys.argv[2]
    iso = Iso9660.open(iso_path)
    record = iso.find("ZOE.DAT")
    base = record.offset // SECTOR if record.offset >= SECTOR else record.offset
    # The root's own entry keeps its count in what is the name field elsewhere (bytes 8-11): `0 0 2 0x20`.
    head = iso.read_sectors(base, 1)
    root_count = struct.unpack_from("<I", head, 8)[0]
    files = list(walk(iso, base, 0, root_count + 1))
    if command == "list":
        everything = "--all" in sys.argv
        for path, sector, size in files:
            if everything or path.lower().endswith(".bin"):
                print("%-40s sector %-8X size %d" % (path, sector, size))
        print("%d files, %d stage .bin" % (len(files), sum(p.lower().endswith(".bin") for p, _, _ in files)))
        return 0
    if command == "extract-bins":
        out = sys.argv[3]
        os.makedirs(out, exist_ok=True)
        n = 0
        for path, sector, size in files:
            if not path.lower().endswith(".bin"):
                continue
            data = iso.read_sectors(base + sector, (size + SECTOR - 1) // SECTOR)[:size]
            name = os.path.basename(path)
            with open(os.path.join(out, name), "wb") as handle:
                handle.write(data)
            print("%-28s %6d bytes  (%s)" % (name, size, path))
            n += 1
        print("%d stage .bin extracted to %s" % (n, out))
        return 0
    raise SystemExit("unknown command %s" % command)


if __name__ == "__main__":
    sys.exit(main())
