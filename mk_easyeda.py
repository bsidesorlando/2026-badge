#!/usr/bin/env python3
"""Write an EasyEDA-import-friendly copy of the KiCad board.

EasyEDA Pro's KiCad importer (chameleon/*/js/convert-node-server.js) was
written against an older KiCad file format.  Two things in a modern
KiCad 9/10 board silently break it.  This script patches both, in a copy.

1. Missing net table  ->  netless tracks, vias and pours
--------------------------------------------------------
KiCad 10 no longer emits the top-level `(net N "name")` table; objects carry
the net NAME inline, e.g. `(segment ... (net "GND"))`.  The importer still
builds its lookup from that table:

    parseNet(e){ for(...) this.netObj[e[1]] = ec(e[2])[0] }
    parseTrack: ... h[0]==="net" ? r = this.netObj[h[1]] : ...

With no `(net ...)` elements, netObj stays empty and every track, via and pour
imports with an EMPTY net.  Netless copper pours have nothing to clear around,
so they flood-fill -- which is what buries the routing under near-solid copper.

The importer's parser keeps the quotes on string tokens (hence the
`.replace(/"/g,"")` calls all over it), so a declaration keyed by NAME --
`(net "GND" "GND")` -- lands in netObj under exactly the token objects use.

2. `(fill yes)` vs `(fill solid)`  ->  hollow polygons
------------------------------------------------------
KiCad <= 6 wrote `(fill solid)` / `(fill none)` on gr_poly; KiCad 7+ writes
`(fill yes)` / `(fill no)`.  The importer still tests for the old spelling:

    parsePoly: ... else if(y[0]==="fill") p = (y[1]||"").toLowerCase()=="solid";
               ... return p ? d.convertToFillRegion() : d.convertToLine()

`"yes" != "solid"`, so every filled polygon is imported as an unfilled outline
-- copper polygons show up hollow / "skeleton style".

Only the standalone `(fill yes)` / `(fill no)` forms are rewritten.  Zones use
a multi-line `(fill yes (thermal_gap ...) ...)` that the importer parses by
iterating children, and it must be left alone -- the regexes below anchor on
the closing paren so they cannot match it.

What does NOT need changing: layer numbering
--------------------------------------------
EasyEDA selects its layer table from the file version:

    version.length < 8 ? (version === "3" ? init2() : init1())
                       : (version < "20241129" ? init1() : init3())

init3 already matches KiCad 9/10 numbering (B.Cu=2, Edge.Cuts=25, F.SilkS=5).
Renumbering layers to the legacy init1 scheme BREAKS the import -- id 37 does
not exist in init3, and parseLayers throws
"Cannot read properties of null (reading 'hashId')", surfacing in the UI as
the unhelpful "Convert abnormalities, wrong code:encoding-error".
(That error code is the encode/export half of the pipeline failing, not a
character-encoding problem.)

Usage
-----
    ./mk_easyeda.py [source.kicad_pcb] [dest.kicad_pcb]

Defaults to hardware/bsidesorl-v1.kicad_pcb -> hardware/bsidesorl-v1-easyeda.kicad_pcb.

The output is a ONE-WAY export artifact: re-run after every board change, and
do not open it in KiCad (KiCad will rewrite what it does not recognize).

Verifying without the GUI
-------------------------
EasyEDA ships its converter as a runnable CLI, so an import can be tested
headlessly:

    CONV="/Applications/EasyEDA-Pro.app/Contents/Resources/app/assets/chameleon/<ver>/js/convert-node-server.js"
    node "$CONV" --cmd conversion --params-path params.json

with params.json:

    {"decodingOptions": {"decodingType": "kicad",
                         "decodingFilePath": "<abs path to the -easyeda .kicad_pcb>",
                         "projectName": "bsidesorl-v1", "projectType": "normal"},
     "encodingOptions": {"encodingType": "easyeda-pro",
                         "savingDirPath": "<abs output dir>",
                         "savingFileName": "out", "encoderOptions": {}}}

The resulting .epro is a zip; PCB/*.epcb holds one JSON array per shape:
  track  ["LINE", id, ?, net, layer, x1,y1, x2,y2, width, ?]   layer 2 = Bottom
  filled ["FILL", ...]      hollow ["POLY", ...]
Confirm nets are non-empty and that copper shapes are FILL, not POLY.
"""
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
DEFAULT_SRC = HERE / 'hardware' / 'bsidesorl-v1.kicad_pcb'

# EasyEDA picks init3 (KiCad 9/10 layer numbering) at or above this file version.
INIT3_MIN_VERSION = "20241129"


def build(src_path: Path, dst_path: Path) -> int:
    src = src_path.read_text()

    m = re.search(r'\(version (\d+)\)', src)
    if not m:
        sys.exit(f"{src_path}: no (version ...) token -- is this a .kicad_pcb?")
    ver = m.group(1)
    if ver < INIT3_MIN_VERSION:
        sys.exit(f"{src_path}: version {ver} predates KiCad 9 layer numbering, so "
                 f"EasyEDA uses its init1 table; this script assumes init3.")

    # Top-level (net ...) declarations, either KiCad's numeric form or the
    # name-keyed one this script writes. Either way there is nothing to add,
    # and re-running on our own output would duplicate every entry.
    existing = len(re.findall(r'\n\t\(net (?:\d+|")', src))
    if existing:
        sys.exit(f"{src_path}: already has {existing} top-level (net ...) "
                 f"declarations -- nothing to inject. Run this on the source "
                 f"board, not on a generated -easyeda copy.")

    nets = sorted(set(re.findall(r'\(net "([^"]*)"\)', src)))
    if not nets:
        sys.exit(f"{src_path}: found no (net \"name\") references.")
    decl = "".join(f'\t(net "{n}" "{n}")\n' for n in nets)

    # Insert straight after the (setup ...) block, so nets are parsed before any
    # footprint / segment / zone that references them.
    setup = re.search(r'\n\t\(setup\n', src)
    if not setup:
        sys.exit(f"{src_path}: no (setup ...) block to anchor the insertion to.")
    depth = 0
    end = None
    for i in range(setup.start() + 1, len(src)):
        if src[i] == '(':
            depth += 1
        elif src[i] == ')':
            depth -= 1
            if depth == 0:
                end = i + 1
                break
    if end is None:
        sys.exit(f"{src_path}: unbalanced parentheses in (setup ...).")

    out = src[:end] + "\n" + decl.rstrip('\n') + src[end:]

    if out.count('(') != out.count(')'):
        sys.exit("refusing to write: parentheses unbalanced after net injection")
    if not (src[:end] == out[:end] and src[end:] == out[len(out) - len(src[end:]):]):
        sys.exit("refusing to write: bytes outside the insertion point changed")

    # Old-style fill spelling. Anchored on the closing paren so the multi-line
    # zone form `(fill yes\n\t(thermal_gap ...)` can never match.
    zones_before = len(re.findall(r'\(fill yes\n', out))
    n_solid = len(re.findall(r'\(fill yes\)', out))
    n_none = len(re.findall(r'\(fill no\)', out))
    out = out.replace('(fill yes)', '(fill solid)').replace('(fill no)', '(fill none)')
    if len(re.findall(r'\(fill yes\n', out)) != zones_before:
        sys.exit("refusing to write: zone (fill yes ...) blocks were altered")

    dst_path.write_text(out)

    print(f"source     {src_path}")
    print(f"version    {ver}  -> EasyEDA uses init3; layer numbering left alone")
    print(f"injected   {len(nets)} net declarations after (setup ...)")
    print(f"           {', '.join(nets[:8])}{' ...' if len(nets) > 8 else ''}")
    print(f"fill fix   {n_solid} (fill yes)->(fill solid), {n_none} (fill no)->(fill none)")
    print(f"           {zones_before} zone fill blocks left untouched")
    print(f"wrote      {dst_path}")
    return 0


if __name__ == '__main__':
    src = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else DEFAULT_SRC
    if len(sys.argv) > 2:
        dst = Path(sys.argv[2]).resolve()
    else:
        dst = src.with_name(src.stem + '-easyeda' + src.suffix)
    if not src.exists():
        sys.exit(f"no such file: {src}")
    sys.exit(build(src, dst))
