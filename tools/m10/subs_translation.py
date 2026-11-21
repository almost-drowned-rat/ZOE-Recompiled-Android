"""The subtitles of our own translations -> the subtitle pack (ZOE-subtitles.pak, a download of its own beside
ZOE.exe - the maintainer 2026-11-21: our translations are not in the exe; how many fetch it shows the demand).

    python -I tools/m10/subs_translation.py [<pack>]   (from the repo root; checks and writes the pack, by default
                                                        games/zoe_us/build/subtitles/ZOE-subtitles.pak)
    python -I tools/m10/subs_translation.py --merge-ja <ja_out>
        (Japanese from the English US subtitles into translation/subtitles_ja_from_en.json, the ids of the Spanish)
    python -I tools/m10/subs_translation.py --merge <out> <es_jp_out> <jp_hashes.json>
        (the translators' chunk files from the Japanese merged into translation/subtitles.json)
    python -I tools/m10/subs_translation.py --merge-es <es_out> <es_unique_ids.json>
        (Spanish from the English US subtitles into translation/subtitles_es_from_en.json)

subtitles.json: {"<scene>#<n>": {"hash": "<FNV-1a 64 of the Japanese line's bytes on the JP disc>",
                                 "en", "fr", "de", "it", "es": ...}} - the Japanese voices' lines translated;
subtitles_es_from_en.json: {"<scene>#<n>": {"hash": "<FNV-1a 64 of the US English line's bytes>", "es": ...}} -
Spanish with the English voices (Spanish had no official text; the maintainer 2026-11-21).
No Japanese or English disc text in the repository: the game finds each line by its hash (dub.cpp). Letters outside
the game's font are reported.
"""

import json
import os
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
GAME = os.path.join(ROOT, "games", "zoe_us")
JSON_PATH = os.path.join(GAME, "translation", "subtitles.json")
ES_JSON_PATH = os.path.join(GAME, "translation", "subtitles_es_from_en.json")
JA_JSON_PATH = os.path.join(GAME, "translation", "subtitles_ja_from_en.json")   # Japanese with the English voices
PACK_PATH = os.path.join(GAME, "build", "subtitles", "ZOE-subtitles.pak")
FONT_PATH = os.path.join(GAME, "port", "fonts", "NotoSansJP-Bold-zoe.otf")
FONT_LICENCE_PATH = os.path.join(GAME, "port", "fonts", "NotoSansJP-OFL.txt")
PACK_VERSION = 1
MAX_JA_LINE = 24                            # full-width letters a Japanese line (the Japanese disc's longest: 28)
DESC_PATH = os.path.join(GAME, "translation", "descriptions_es.json")     # the executable's descriptions
SCX_PATH = os.path.join(GAME, "translation", "scripts_es.json")           # the stage scripts' texts
LANGS = ("en", "fr", "de", "it", "es")      # the US file's channels 7, 8, 9, 10; Spanish (shown in 7)
# Not subtitles: the survey read bytes after a scene's real blocks as lines (times far over an hour).
JUNK = {"bs1g7081#0", "bs1g7131#0", "bs1p7201#0", "ca1g9121#0", "ca1gc261#0", "fa2g1141#0"}
MAX_LINE = 52                               # the disc's longest subtitle line (FR 53, EN / DE 51)

# The game's escapes: & + mark + letter (its French and German subtitles; ñ Ñ ¿ ¡ tried in the game 2026-11-21).
ACCENTS = {
    "á": "&'a", "é": "&'e", "í": "&'i", "ó": "&'o", "ú": "&'u",
    "Á": "&'A", "É": "&'E", "Í": "&'I", "Ó": "&'O", "Ú": "&'U",
    "à": "&`a", "è": "&`e", "ì": "&`i", "ò": "&`o", "ù": "&`u",
    "À": "&`A", "È": "&`E", "Ì": "&`I", "Ò": "&`O", "Ù": "&`U",
    "â": "&^a", "ê": "&^e", "î": "&^i", "ô": "&^o", "û": "&^u", "Ê": "&^E",
    "ä": "&~a", "ö": "&~o", "ü": "&~u", "Ä": "&~A", "Ö": "&~O", "Ü": "&~U", "ß": "&~s",
    "ç": "&~q", "Ç": "&~Q", "ë": "&@e", "ï": "&~i",
    "ñ": "&~n", "Ñ": "&~N", "¿": "&~?", "¡": "&~!",
}
PLAIN = {"…": "...", "’": "'", "‘": "'", "“": '"', "”": '"', "«": '"', "»": '"', "–": "-", "—": "-",
         "œ": "oe", "Œ": "Oe", " ": " ", " ": " "}
# The US text's own signs (EUC bytes, read as Latin-1: the PS2 buttons' and brackets' glyphs), kept as bytes.
RAW_PAIRS = ("¢¢", "¢¤", "¡û", "¡ß", "¡È", "¡É", "¥Î")


def game_text(s, where, problems):
    out = bytearray()
    i = 0
    while i < len(s):
        pair = s[i:i + 2]
        if pair in RAW_PAIRS:
            out += pair.encode("latin-1")
            i += 2
            continue
        ch = s[i]
        i += 1
        if ch in ACCENTS:
            out += ACCENTS[ch].encode()
        elif ch in PLAIN:
            out += PLAIN[ch].encode()
        elif ch == "\n" or 32 <= ord(ch) < 127:
            if ch == "&":
                problems.append(f"{where}: '&' (the game's escape sign)")
            out += ch.encode()
        else:
            problems.append(f"{where}: letter {ch!r} not in the game's font")
    for line in s.split("\n"):
        if len(line) > MAX_LINE:
            problems.append(f"{where}: line of {len(line)} letters: {line!r}")
    if s.count("\n") > 2:
        problems.append(f"{where}: {s.count(chr(10)) + 1} lines")
    return bytes(out)


def c_string(b):
    out = ['"']
    for x in b:
        ch = chr(x)
        if ch == "\n":
            out.append("\\n")
        elif ch in '"\\':
            out.append("\\" + ch)
        elif 32 <= x < 127:
            out.append(ch)
        else:
            out.append(f"\\{x:03o}")
    out.append('"')
    return "".join(out)


def load_dir(folder):
    out = {}
    for name in sorted(os.listdir(folder)):
        if name.endswith(".json"):
            out.update(json.load(open(os.path.join(folder, name), encoding="utf-8")))
    return out


def save(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        json.dump(dict(sorted(data.items())), f, ensure_ascii=False, indent=1)
        f.write("\n")


def merge(out_dir, es_dir, hashes_path):
    hashes = json.load(open(hashes_path, encoding="utf-8"))
    four, es = load_dir(out_dir), load_dir(es_dir)
    merged = {}
    for k, v in four.items():
        if k in JUNK or k not in es:
            continue
        merged[k] = {"hash": hashes[k], **{lang: v[lang] for lang in ("en", "fr", "de", "it")}, "es": es[k]}
    missing = sorted(set(hashes) - set(merged) - JUNK)
    save(JSON_PATH, merged)
    print(f"{len(merged)} lines merged, {len(missing)} missing" + (f": {missing[:10]}" if missing else ""))


def fnv(b):
    h = 0xcbf29ce484222325
    for x in b:
        h = ((h ^ x) * 0x100000001b3) & 0xFFFFFFFFFFFFFFFF
    return h


def merge_es(es_dir, ids_path):
    ids = json.load(open(ids_path, encoding="utf-8"))          # English text -> id
    es = load_dir(es_dir)
    merged = {}
    for text, key in ids.items():
        if key in es:
            merged[key] = {"hash": f"{fnv(text.encode('latin-1')):016x}", "es": es[key]}
    save(ES_JSON_PATH, merged)
    print(f"{len(merged)} Spanish lines (from the English) merged, {len(ids) - len(merged)} missing")


def japanese_text(s, where, problems, font_chars):
    """A Japanese line (UTF-8, drawn with Noto Sans JP by the port): its letters in the font's subset, its length."""
    for ch in s:
        if ch != "\n" and font_chars is not None and ord(ch) not in font_chars:
            problems.append(f"{where}: letter {ch!r} not in the Noto Sans JP subset")
    for line in s.split("\n"):
        if len(line) > MAX_JA_LINE:
            problems.append(f"{where}: Japanese line of {len(line)} letters: {line!r}")
    if s.count("\n") > 2:
        problems.append(f"{where}: {s.count(chr(10)) + 1} lines")
    return s.encode("utf-8")


def font_characters(path):
    """The code points the font has (its cmap), or None without fontTools."""
    try:
        from fontTools.ttLib import TTFont
    except ImportError:
        return None
    return set(TTFont(path).getBestCmap())


# The pack: "ZOESUBS" 0x01, then sections - a 4-letter tag, its size (u32 LE), its bytes. Numbers little-endian;
# a text is its size (u16) and its bytes (the game's letters and escapes; Japanese in UTF-8).
#   VERS  the pack's version (u32)
#   JPTR  the Japanese voices' lines: count, then each: hash (u64), EN FR DE IT ES texts
#   ESEN  Spanish with the English voices: count, then hash (u64) + text
#   JAEN  Japanese with the English voices: count, then hash (u64) + text (UTF-8)
#   DESC  Spanish descriptions in the executable: count, then at (u32), group (u32), hash (u64), text
#   SCXE  Spanish texts of the stage scripts: count, then hash (u64) + text
#   FONT  Noto Sans JP (the subset, SIL OFL 1.1) for the Japanese subtitles;  OFL_  its licence
def section(tag, body):
    return tag.encode() + struct.pack("<I", len(body)) + body


def text_bytes(b):
    if len(b) > 0xFFFF:
        raise ValueError("text too long")
    return struct.pack("<H", len(b)) + b


def build(pack_path=PACK_PATH):
    data = json.load(open(JSON_PATH, encoding="utf-8"))
    es_en = json.load(open(ES_JSON_PATH, encoding="utf-8")) if os.path.exists(ES_JSON_PATH) else {}
    ja_en = json.load(open(JA_JSON_PATH, encoding="utf-8")) if os.path.exists(JA_JSON_PATH) else {}
    desc = json.load(open(DESC_PATH, encoding="utf-8")) if os.path.exists(DESC_PATH) else {}
    scx = json.load(open(SCX_PATH, encoding="utf-8")) if os.path.exists(SCX_PATH) else {}
    font_chars = font_characters(FONT_PATH)
    problems = []
    rows = {int(v["hash"], 16): [game_text(v[lang], f"{key} {lang}", problems) for lang in LANGS]
            for key, v in data.items()}
    es_rows = {int(v["hash"], 16): game_text(v["es"], f"{key} es (from en)", problems) for key, v in es_en.items()}
    ja_rows = {int(v["hash"], 16): japanese_text(v["ja"], f"{key} ja (from en)", problems, font_chars)
               for key, v in ja_en.items()}
    jptr = struct.pack("<I", len(rows)) + b"".join(
        struct.pack("<Q", h) + b"".join(text_bytes(t) for t in rows[h]) for h in sorted(rows))
    esen = struct.pack("<I", len(es_rows)) + b"".join(
        struct.pack("<Q", h) + text_bytes(es_rows[h]) for h in sorted(es_rows))
    jaen = struct.pack("<I", len(ja_rows)) + b"".join(
        struct.pack("<Q", h) + text_bytes(ja_rows[h]) for h in sorted(ja_rows))
    # The descriptions (the executable's tables: the EN group's first pointer's address, its size, the English
    # lines' hash - joined with "\n" - and the Spanish lines joined so) and the scripts' texts.
    descs = sorted(desc.items(), key=lambda x: int(x[0], 16))
    desc_b = struct.pack("<I", len(descs)) + b"".join(
        struct.pack("<IIQ", int(at, 16), v["group"], int(v["hash"], 16))
        + text_bytes(game_text("\n".join(v["es"]), f"description {at}", problems)) for at, v in descs)
    scx_b = struct.pack("<I", len(scx)) + b"".join(
        struct.pack("<Q", int(h, 16)) + text_bytes(game_text(v, f"script {h}", problems)) for h, v in sorted(scx.items()))
    out = b"ZOESUBS\x01" + section("VERS", struct.pack("<I", PACK_VERSION)) + section("JPTR", jptr) \
        + section("ESEN", esen) + section("JAEN", jaen) + section("DESC", desc_b) + section("SCXE", scx_b) \
        + section("FONT", open(FONT_PATH, "rb").read()) + section("OFL_", open(FONT_LICENCE_PATH, "rb").read())
    os.makedirs(os.path.dirname(pack_path), exist_ok=True)
    with open(pack_path, "wb") as f:
        f.write(out)
    print(f"{len(rows)} lines from the Japanese, {len(es_rows)} Spanish and {len(ja_rows)} Japanese ones from the "
          f"English, {len(descs)} descriptions, {len(scx)} script texts: {pack_path} ({len(out)} bytes)")
    if font_chars is None:
        print("  (fontTools missing: the Japanese letters not checked against the font)")
    for p in problems:
        print("  " + p)
    return 1 if any("not in the game's font" in p or "Noto Sans JP subset" in p for p in problems) else 0


def merge_ja(ja_dir):
    """The translators' Japanese (from the English) by the Spanish one's ids - the same English lines, their hashes."""
    es = json.load(open(ES_JSON_PATH, encoding="utf-8"))
    ja = load_dir(ja_dir)
    # The US text's button / bracket signs (its EUC bytes read as Latin-1) as the letters they are - drawn with
    # Noto Sans JP, not the game's font; "¥Î" (ノ) stands where the US line lost a "…".
    signs = {p: ("…" if p == "¥Î" else p.encode("latin-1").decode("euc_jp")) for p in RAW_PAIRS}
    for k, v in ja.items():
        for p, ch in signs.items():
            v = v.replace(p, ch)
        ja[k] = v
    merged = {k: {"hash": es[k]["hash"], "ja": v} for k, v in ja.items() if k in es}
    save(JA_JSON_PATH, merged)
    print(f"{len(merged)} Japanese lines (from the English) merged, {len(set(es) - set(merged))} missing, "
          f"{len(set(ja) - set(es))} unknown ids")


def apply_fixes(fix_dir, from_en=None):
    """A review's corrections: {id: {lang: text}} (from the Japanese) or {id: text} (from_en "es" / "ja": Spanish or
    Japanese from the English). An empty text: no subtitle there (a noise - the maintainer 2026-11-21: "keuch" and
    the like not shown, "das ist natürlicher")."""
    path = {"es": ES_JSON_PATH, "ja": JA_JSON_PATH}.get(from_en, JSON_PATH)
    data = json.load(open(path, encoding="utf-8"))
    n = 0
    for k, v in load_dir(fix_dir).items():
        if k not in data:
            print(f"  fix for an unknown id {k}")
            continue
        for lang, text in ({from_en: v} if from_en else v).items():
            if lang in data[k] and data[k][lang] != text:
                data[k][lang] = text
                n += 1
    save(path, data)
    print(f"{n} corrections applied to {os.path.basename(path)}")


def merge_desc(entries_path, es_path, scx_src_path):
    """Descriptions (tools/m10/desc_tables.py's entries + their Spanish) and the scripts' texts ({English: Spanish})
    into translation/descriptions_es.json / scripts_es.json, by the English text's hash."""
    entries = json.load(open(entries_path, encoding="utf-8"))["entries"]
    es = json.load(open(es_path, encoding="utf-8"))
    desc = {}
    for e in entries:
        if e["at"] in es:
            h = fnv("\n".join(e["lines"]).encode("latin-1"))
            desc[e["at"]] = {"hash": f"{h:016x}", "group": e["group"], "es": es[e["at"]]}
    save(DESC_PATH, desc)
    scx = {f"{fnv(en.encode('latin-1')):016x}": v for en, v in json.load(open(scx_src_path, encoding="utf-8")).items()}
    save(SCX_PATH, scx)
    print(f"{len(desc)} descriptions and {len(scx)} script texts in Spanish")


if __name__ == "__main__":
    if len(sys.argv) >= 5 and sys.argv[1] == "--merge-desc":
        merge_desc(sys.argv[2], sys.argv[3], sys.argv[4])
    elif len(sys.argv) >= 3 and sys.argv[1] in ("--fix", "--fix-es", "--fix-ja"):
        apply_fixes(sys.argv[2], {"--fix-es": "es", "--fix-ja": "ja"}.get(sys.argv[1]))
    elif len(sys.argv) >= 5 and sys.argv[1] == "--merge":
        merge(sys.argv[2], sys.argv[3], sys.argv[4])
    elif len(sys.argv) >= 4 and sys.argv[1] == "--merge-es":
        merge_es(sys.argv[2], sys.argv[3])
    elif len(sys.argv) >= 3 and sys.argv[1] == "--merge-ja":
        merge_ja(sys.argv[2])
    else:
        sys.exit(build(*sys.argv[1:2]))
