#!/usr/bin/env python3
"""Regenerates NotoSansJP-subset.otf, the Japanese font ./wasm/build.sh embeds.

    uv tool install fonttools                  # provides pyftsubset
    python3 wasm/fonts/make_jp_subset.py       # [--font PATH] [--out PATH]

Source: Noto Sans CJK JP Regular 2.004 (SIL OFL 1.1, see OFL.txt), as
either the "Noto Sans CJK JP" face of NotoSansCJK-Regular.ttc
(Debian/Ubuntu fonts-noto-cjk 1:20240730) or upstream's
Sans/OTF/Japanese/NotoSansCJKjp-Regular.otf (github.com/notofonts/noto-cjk).
With fonttools 4.67.0, both give the committed file byte for byte.

Characters:
  - ASCII, Latin-1 Supplement, General Punctuation, plus the euro and TM signs
  - CJK Symbols & Punctuation, Hiragana, Katakana (+ Phonetic Extensions),
    Halfwidth & Fullwidth Forms (full-width ASCII, half-width katakana)
  - JIS X 0208 non-kanji rows 1-8 (symbols, arrows, Greek, Cyrillic, box
    drawing), in both the JIS and the Windows (CP932) mapping, plus CP932's
    row-13 specials (circled numbers, Roman numerals, unit squares, ...)
  - all 2,965 JIS X 0208 level-1 kanji
  - every joyo and jinmeiyo kanji outside level 1 (EXTRA_KANJI, 332: mostly
    level 2, the rest JIS X 0213 or compatibility forms)
The rest of level 2 (~3,000 rare kanji) would double the file; those draw
the renderer's hollow box. Build with --cjk for the whole collection.

Layout: only the features HarfBuzz applies to horizontal text by default
(ccmp calt liga kern mark) survive. Vertical metrics and forms, the
Chinese/Korean locl variants and the width/proportional alternates are
dropped, because the renderer lays out single horizontal lines. Hinting is
kept: the atlas rasterizes hinted at 26 px, like native builds that read
the full font. Every name record stays, including the copyright and
license that OFL section 2 requires each copy to carry.
"""

import argparse
import os
import shutil
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
FAMILY = "Noto Sans CJK JP"
SOURCES = (
    "/usr/share/fonts/opentype/noto/NotoSansCJKjp-Regular.otf",
    "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
    "/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc",
)

BLOCKS = (
    (0x0020, 0x007E),  # ASCII
    (0x00A0, 0x00FF),  # Latin-1 Supplement (punctuation, symbols, letters)
    (0x2000, 0x206F),  # General Punctuation
    (0x20AC, 0x20AC),  # euro sign
    (0x2122, 0x2122),  # trade mark sign
    (0x3000, 0x303F),  # CJK Symbols and Punctuation
    (0x3040, 0x309F),  # Hiragana
    (0x30A0, 0x30FF),  # Katakana
    (0x31F0, 0x31FF),  # Katakana Phonetic Extensions
    (0xFF00, 0xFFEF),  # Halfwidth and Fullwidth Forms
)

# Joyo (2010) and jinmeiyo kanji that are not JIS X 0208 level 1: Unihan
# kJoyoKanji / kJinmeiyoKanji (Unicode 18.0) minus rows 16-47. Written as
# codepoints, not characters, because the compatibility ideographs
# (U+F9xx/FAxx) would silently become their unified forms in any
# NFC-normalizing editor.
EXTRA_KANJI = (
    # joyo (34): 丼傲刹剝哺喩嗅嘲塡彙恣惧慄憬拉摯曖楷毀璧瘍箋籠緻羞訃諧貪踪辣錮頰鬱𠮟
    """
    4E3C 50B2 5239 525D 54FA 55A9 55C5 5632 5861 5F59 6063 60E7
    6144 61AC 62C9 646F 66D6 6977 6BC0 74A7 760D 7B8B 7C60 7DFB
    7F9E 8A03 8AE7 8CAA 8E2A 8FA3 932E 9830 9B31 20B9F
    """
    # jinmeiyo (96): 侑俐凛凰巫昊昴晏晨栞椰橙檜澪煌燎珀琥瑶皓稟笙絆綸綺翔苺
    # 茉莉菫蕾迪遙颯驍麒黎 ...
    """
    4F91 4FD0 4FE0 4FF1 51DB 51F0 52C1 52D2 541E 582F 594E 5D1A
    5DD6 5DEB 5DF2 5F57 5FA0 60FA 6451 660A 6634 6644 664F 665F
    6668 6689 681E 689B 6930 69C7 6A59 6A9C 6AC2 6BEC 6D35 6D38
    6E3E 6EC9 6F31 6FAA 7130 714C 7199 71CE 71FF 73C0 73C8 7425
    7476 7693 7738 797F 79B1 7A1F 7A70 7A79 7B19 7C1E 7D46 7DB8
    7DBA 7E61 7E6B 7F9A 7FD4 8129 82FA 8309 8389 83EB 840A 8420
    8523 857E 87EC 881F 8A62 8AC4 8D73 8FEA 901E 9059 91AC 91C9
    980C 985A 98AF 9A4D 9DD7 9E92 9ECE FA16 FA46 FA4A FA4F FA53
    """
    # traditional and compatibility forms the jinmeiyo list allows (202):
    # 亞佛來國圓壽廣德榮櫻澁眞禮與萬藝齊 ...
    """
    4E58 4E9E 4F5B 4F86 50B3 50DE 50F9 5109 5152 51C9 51DC 5269
    528D 52F3 5377 537D 55AE 56B4 5708 570B 5713 5718 589E 58D8
    58DE 58EF 58FD 5967 596C 5B43 5BE2 5BE6 5BEC 5C07 5C08 5CFD
    5DE2 5E36 5EE3 5EF3 5F48 5F4C 5F9E 5FB5 5FB7 6046 60E0 60E1
    613C 61C9 61F7 6230 6232 62C2 62D4 62DC 63ED 6416 641C 64CA
    651D 6536 654D 665A 665D 66C6 66C9 689D 69AE 6A02 6A23 6A6B
    6AA2 6AFB 6B65 6B77 6BCF 6C23 6D89 6DDA 6DE8 6E34 6EAB 6EEF
    6F81 6FD5 7028 71D2 722D 7232 72C0 72F9 7378 758A 7626 76DC
    76E1 771E 788E 7955 79AA 79AE 7A3B 7A57 7CB9 7DA0 7DD6 7DE3
    7E23 7E31 7E96 807D 81DF 8207 838A 842C 85B0 85CF 85DD 85E5
    865B 885E 88DD 89BD 8B20 8B93 8CE3 8CF4 8F49 90DE 9189 91C0
    9304 934A 93AD 9444 9677 96AA 96DC 975C 986F 98DC 9A37 9A57
    9AEE 9DC4 9EC3 9ED1 9ED8 9F4A F91D F928 F929 F936 F9D0 FA19
    FA1A FA1B FA22 FA26 FA30 FA31 FA33 FA34 FA35 FA37 FA38 FA3A
    FA3B FA3D FA3F FA40 FA41 FA43 FA44 FA45 FA47 FA48 FA4B FA4C
    FA4D FA4E FA50 FA51 FA52 FA54 FA55 FA56 FA57 FA59 FA5A FA5B
    FA5C FA5F FA61 FA62 FA63 FA64 FA65 FA67 FA68 FA69
    """
)

# Only what HarfBuzz turns on for horizontal text by default, and that
# this font has.
LAYOUT_FEATURES = "ccmp,calt,liga,kern,mark"
DROP_TABLES = "vhea,vmtx,VORG,BASE"


def jis0208(rows):
    """Characters of JIS X 0208 rows (ku) `rows`, as EUC-JP maps them."""
    out = []
    for row in rows:
        for cell in range(1, 95):
            try:
                out.append(bytes((0xA0 + row, 0xA0 + cell)).decode("euc_jp"))
            except UnicodeDecodeError:
                pass  # unassigned cell
    return out


def cp932(rows):
    """The same rows as Windows (CP932) maps them: a few symbols land on
    other codepoints (e.g. U+FF5E/U+2225/U+FF0D instead of
    U+301C/U+2016/U+2212), and row 13 holds the NEC specials."""
    out = []
    for row in rows:
        for cell in range(1, 95):
            j1, j2 = row + 0x20, cell + 0x20  # JIS -> Shift_JIS
            s1 = ((j1 + 1) >> 1) + (0x70 if j1 <= 0x5E else 0xB0)
            s2 = j2 + (0x1F if j1 & 1 else 0x7E)
            if j1 & 1 and s2 >= 0x7F:
                s2 += 1
            try:
                out.append(bytes((s1, s2)).decode("cp932"))
            except UnicodeDecodeError:
                pass
    return out


def codepoints():
    level1 = jis0208(range(16, 48))
    assert len(level1) == 2965 and level1 == cp932(range(16, 48))
    cps = set()
    for lo, hi in BLOCKS:
        cps.update(range(lo, hi + 1))
    cps.update(map(ord, jis0208(range(1, 9)) + cp932(range(1, 9)) + cp932([13])))
    cps.update(map(ord, level1))
    cps.update(int(h, 16) for h in EXTRA_KANJI.split())
    return sorted(cps)


def family_names(path):
    """Family name (name ID 1, Windows English) of each face in an
    OpenType font or collection, in face order."""
    with open(path, "rb") as f:
        def read(offset, size):
            f.seek(offset)
            return f.read(size)

        if read(0, 4) == b"ttcf":
            count = struct.unpack(">I", read(8, 4))[0]
            faces = struct.unpack(">%dI" % count, read(12, 4 * count))
        else:
            faces = (0,)
        names = []
        for base in faces:
            num_tables = struct.unpack(">H", read(base + 4, 2))[0]
            name = None
            for i in range(num_tables):
                tag, _, offset, _ = struct.unpack(">4sIII", read(base + 12 + 16 * i, 16))
                if tag == b"name":
                    _, count, strings = struct.unpack(">HHH", read(offset, 6))
                    for j in range(count):
                        pid, eid, lid, nid, length, at = struct.unpack(
                            ">6H", read(offset + 6 + 12 * j, 12))
                        if (pid, eid, lid, nid) == (3, 1, 0x409, 1):
                            name = read(offset + strings + at, length).decode("utf-16-be")
                            break
            names.append(name)
        return names


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--font", help="source .otf/.ttc (default: first of %s)" % ", ".join(SOURCES))
    ap.add_argument("--out", default=os.path.join(HERE, "NotoSansJP-subset.otf"))
    args = ap.parse_args()

    src = args.font or next((p for p in SOURCES if os.path.exists(p)), None)
    if src is None:
        sys.exit("no Noto Sans CJK JP found; pass --font (NotoSansCJKjp-Regular.otf "
                 "from github.com/notofonts/noto-cjk releases)")
    names = family_names(src)
    if FAMILY not in names:
        sys.exit("%s has no '%s' face (faces: %s)" % (src, FAMILY, names))
    pyftsubset = shutil.which("pyftsubset")
    if pyftsubset is None:
        sys.exit("pyftsubset not found: uv tool install fonttools (or pip install fonttools)")

    cps = codepoints()
    with tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False) as listing:
        listing.write("\n".join("U+%04X" % cp for cp in cps) + "\n")
    try:
        subprocess.run([
            pyftsubset, src,
            "--font-number=%d" % names.index(FAMILY),
            "--unicodes-file=" + listing.name,
            "--layout-features=" + LAYOUT_FEATURES,
            "--drop-tables+=" + DROP_TABLES,
            "--name-IDs=*", "--name-languages=*", "--name-legacy",
            # Inlined subroutines: 2% larger as a file, 5% smaller after
            # gzip, which is how Pages serves the .wasm.
            "--desubroutinize",
            "--output-file=" + args.out,
        ], check=True)
    finally:
        os.unlink(listing.name)
    print("%s: %d bytes from %s (face '%s', %d codepoints requested)"
          % (args.out, os.path.getsize(args.out), src, FAMILY, len(cps)))


if __name__ == "__main__":
    main()
