#!/usr/bin/env python3
"""Extract Address Library ID -> function name pairs from CommonLibSSE sources.

CommonLibSSE wraps most engine functions like this:

    void Main::RenderWorld(bool a_unk)
    {
        using func_t = decltype(&Main::RenderWorld);
        static REL::Relocation<func_t> func{ RELOCATION_ID(100424, 107142) };
        ...

so each RELOCATION_ID(se, ae) that sits inside a function body and relocates a
function (not data) names that function for both runtimes. The output is used
by analyze.py as a fallback name table and to pair AE IDs with their SE IDs.

Usage: mine_commonlib_ids.py [--commonlib PATH] [--out commonlib_ids.json]
"""

import argparse
import json
import re
import sys
from pathlib import Path

DEFINITION = re.compile(
    r"^\s*(?:[\w:<>,\*&\s]+?\s+)?[\*&]?\s*(?P<name>(?:\w+::)+~?\w+)\s*\([^;]*$"
)
RELOCATION = re.compile(r"RELOCATION_ID\(\s*(?P<se>\d+)\s*,\s*(?P<ae>\d+)\s*\)")
DECLTYPE = re.compile(r"decltype\(&(?P<name>(?:\w+::)+~?\w+)\)")
FUNCTION_HINT = re.compile(r"func_t|decltype\(&|Relocation<\s*\w+\s*\(|Relocation<func|func\{|func\(")


def mine(root: Path):
    ae_names, se_names, pairs = {}, {}, {}
    files = sorted(list(root.glob("src/**/*.cpp")) + list(root.glob("include/**/*.h")))
    for path in files:
        if path.name.startswith("Offsets_"):
            continue
        definition = None  # Most recent Class::Method( definition line.
        recent = []        # Last few lines, to find `using func_t = decltype(&X::Y)`.
        for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
            stripped = line.strip()
            match = DEFINITION.match(line)
            if match and not stripped.startswith(("return", "using", "static", "if", "else", "//")):
                definition = match.group("name")

            recent = (recent + [line])[-4:]
            relocation = RELOCATION.search(line)
            if not relocation or not FUNCTION_HINT.search(line):
                continue

            name = None
            for previous in reversed(recent):
                typed = DECLTYPE.search(previous)
                if typed:
                    name = typed.group("name")
                    break
            name = name or definition
            if not name:
                continue

            se, ae = int(relocation.group("se")), int(relocation.group("ae"))
            se_names.setdefault(se, name)
            ae_names.setdefault(ae, name)
            pairs.setdefault(ae, se)

    return ae_names, se_names, pairs


def main():
    here = Path(__file__).resolve().parent
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--commonlib", type=Path, default=here.parents[1] / "CommonLibSSE")
    parser.add_argument("--out", type=Path, default=here / "commonlib_ids.json")
    args = parser.parse_args()

    if not (args.commonlib / "include" / "RE").is_dir():
        sys.exit(f"{args.commonlib} does not look like a CommonLibSSE checkout")

    ae_names, se_names, pairs = mine(args.commonlib)
    data = {
        "source": str(args.commonlib),
        "ae": {str(k): v for k, v in sorted(ae_names.items())},
        "se": {str(k): v for k, v in sorted(se_names.items())},
        "aeToSe": {str(k): v for k, v in sorted(pairs.items())},
    }
    args.out.write_text(json.dumps(data, indent=1), encoding="utf-8")
    print(f"Wrote {len(ae_names)} AE / {len(se_names)} SE function names to {args.out}")


if __name__ == "__main__":
    main()
