"""Design 7d (2026-11-12): ZOE's stage stubs, all of them, from the disc - one relocatable overlay per version.

Input: games/<game>/extracted/stage_bin/*.bin (tools/m3/zoe_dat.py extract-bins). Each stub is straight-line MIPS
(one setup call, then hash -> function registrations) ending in `jr $ra`, its name strings after the code. They are
laid one after another from CANONICAL (a 4 KB slot each) into one image, extracted/overlays/stages.bin (game
material, not committed), described by overlays/stages/game.json with "relocatable": true - port.py then matches
each stub by its whole code wherever the game loads it (ps2host::RegisterRelocatables) instead of by address.
The old address-bound stub overlays (cut from RAM in played sessions) leave the game's overlay list.
Then analyze + lift (--emit --structure --csv) the overlay, once.

    python -I tools/m3/make_disc_overlays.py games/zoe_us [--no-lift]
"""

import json
import os
import struct
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CANONICAL = 0x01C00000
SLOT = 0x1000
OLD_STUBS = {"title", "init", "fa1", "fa2", "gl1", "vr1", "vr1_00A61B40"}


def code_end(data: bytes) -> int:
    """Just past the stub's last `jr $ra` or `j` (they end with a tail jump into the exe, `j 0x0043BB90` in US)
    and its delay slot, before the stub's name strings."""
    last = -1
    for offset in range(0, len(data) - 3, 4):
        word = struct.unpack_from("<I", data, offset)[0]
        if word == 0x03E00008 or (word >> 26) == 2:
            last = offset
    if last < 0:
        raise SystemExit("no jr $ra / j in a stub")
    return last + 8


def main() -> int:
    game_dir = sys.argv[1]
    lift = "--no-lift" not in sys.argv
    game = os.path.join(ROOT, game_dir)
    source = os.path.join(game, "extracted", "stage_bin")
    names = sorted(n for n in os.listdir(source) if n.endswith(".bin"))
    image = bytearray()
    code = []
    for index, name in enumerate(names):
        with open(os.path.join(source, name), "rb") as handle:
            data = handle.read()
        if len(data) > SLOT:
            raise SystemExit("%s is larger than a slot (%d bytes)" % (name, len(data)))
        at = index * SLOT
        image += b"\0" * (at - len(image)) + data
        end = code_end(data)
        code.append(["0x%08X" % (CANONICAL + at), "0x%08X" % (CANONICAL + at + end)])
        print("%-12s at 0x%08X, code %5d of %5d bytes" % (name, CANONICAL + at, end, len(data)))
    image += b"\0" * (-len(image) % 0x100)
    os.makedirs(os.path.join(game, "extracted", "overlays"), exist_ok=True)
    with open(os.path.join(game, "extracted", "overlays", "stages.bin"), "wb") as handle:
        handle.write(image)
    with open(os.path.join(game, "game.json"), encoding="utf-8") as handle:
        base = json.load(handle)
    os.makedirs(os.path.join(game, "overlays", "stages", "config"), exist_ok=True)
    description = {
        "name": "%s: stage stubs from the disc (%d, relocatable)" % (base["name"], len(names)),
        "title": base.get("title", base["name"]),
        "overlay": {"of": "../..", "image": "../../extracted/overlays/stages.bin",
                    "address": "0x%08X" % CANONICAL, "relocatable": True, "code": code},
    }
    with open(os.path.join(game, "overlays", "stages", "game.json"), "w", encoding="utf-8", newline="\n") as handle:
        json.dump(description, handle, indent=2)
        handle.write("\n")
    kept = [o for o in base.get("overlays", []) if o.split("/")[-1] not in OLD_STUBS]
    if "overlays/stages" not in kept:
        kept.append("overlays/stages")
    base["overlays"] = kept
    with open(os.path.join(game, "game.json"), "w", encoding="utf-8", newline="\n") as handle:
        json.dump(base, handle, indent=2)
        handle.write("\n")
    print("overlays/stages: %d stubs, image %d bytes; game.json overlays now %s" % (len(names), len(image), kept))
    if not lift:
        return 0
    target = "%s/overlays/stages" % game_dir
    for command in (["engine/tools/analyze.py", "--game", target, "-q"],
                    ["engine/tools/lift.py", "--game", target, "--jobs", "2", "--emit", "--structure", "--csv"]):
        result = subprocess.run([sys.executable, "-u"] + command, cwd=ROOT)
        if result.returncode:
            return result.returncode
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
