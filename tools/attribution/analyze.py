#!/usr/bin/env python3
"""Turn a SkyrimEngineTelemetry attribution report into ranked, named findings.

Reads the JSON the in-game sampler writes and produces Markdown with:
  1. a thread table (CPU share, zone coverage, blocked/idle split),
  2. per-zone self-time broken down by direct callee and call site,
  3. top-down call trees of unscoped work per thread, with root-zone candidates,
  4. ready-to-paste hook stubs for the biggest candidates.

Function names come from, in order: a local cache, ghidra-mcp's HTTP server
(BethesdaGhidraScripts project), the Address Library Database `.rename` file,
names mined from CommonLibSSE, and finally sub_<rva>.

Usage:
  analyze.py attribution-YYYYMMDD-HHMMSS.json [-o report.md] [--no-ghidra]
             [--ghidra URL] [--program NAME] [--rename PATH] [--top N]
             [--min-share PCT] [--context]
"""

import argparse
import json
import re
import sys
import urllib.error
import urllib.parse
import urllib.request
from collections import Counter, defaultdict
from pathlib import Path

HERE = Path(__file__).resolve().parent
DEFAULT_RENAME = Path.home() / "source/repos/BethesdaGhidraScripts/extern/AddressLibraryDatabase/skyrimae.rename"
WEAK_NAME = re.compile(r"^(FUN_|sub_|thunk_FUN_|LAB_)|::sub$", re.IGNORECASE)


# --------------------------------------------------------------------------
# Name sources
# --------------------------------------------------------------------------


class Ghidra:
    def __init__(self, url, program):
        self.url = url.rstrip("/")
        self.program = program
        self.image_base = None
        self.available = False

    def connect(self):
        try:
            programs = json.loads(self._get("/list_open_programs", timeout=5))
        except (OSError, ValueError, urllib.error.URLError) as error:
            print(f"ghidra-mcp not reachable at {self.url} ({error}); continuing without it", file=sys.stderr)
            return False

        entries = programs.get("programs", [])
        chosen = None
        for entry in entries:
            if self.program and self.program in (entry.get("name"), entry.get("path")):
                chosen = entry
            elif not self.program and entry.get("is_current"):
                chosen = entry
        if not chosen:
            print(f"ghidra-mcp has no program matching {self.program!r}; open programs: "
                  f"{[e.get('path') for e in entries]}", file=sys.stderr)
            return False

        self.program = chosen["name"]
        self.image_base = int(chosen["image_base"], 16)
        self.available = True
        print(f"Using Ghidra program {chosen['path']} (image base 0x{self.image_base:X})", file=sys.stderr)
        return True

    def _get(self, path, timeout=30, **params):
        if self.program and "program" not in params and path != "/list_open_programs":
            params["program"] = self.program
        query = urllib.parse.urlencode(params)
        with urllib.request.urlopen(f"{self.url}{path}?{query}", timeout=timeout) as response:
            return response.read().decode("utf-8", errors="replace")

    def function(self, rva):
        """Returns (name, signature) for the function containing image_base + rva."""
        text = self._get("/get_function_by_address", address=f"0x{self.image_base + rva:x}")
        name = re.search(r"^Function: (.+?) at ([0-9a-fA-F]+)", text, re.MULTILINE)
        if not name:
            return None
        signature = re.search(r"^Signature: (.+)$", text, re.MULTILINE)
        return name.group(1), signature.group(1) if signature else ""

    def callers(self, rva, limit=12):
        return self._list("/get_function_callers", rva, limit)

    def callees(self, rva, limit=12):
        return self._list("/get_function_callees", rva, limit)

    def decompile(self, rva, max_lines=40):
        text = self._get("/decompile_function", timeout=90, address=f"0x{self.image_base + rva:x}")
        lines = text.strip("\n").splitlines()
        return "\n".join(lines[:max_lines]) + ("\n..." if len(lines) > max_lines else "")

    def _list(self, path, rva, limit):
        text = self._get(path, address=f"0x{self.image_base + rva:x}", limit=limit)
        return [line.strip() for line in text.splitlines() if line.strip() and not line.startswith(("No ", "Error"))]


class Names:
    def __init__(self, report, args):
        self.report = report
        self.edition = report.data.get("edition", "AE")
        self.cache_path = HERE / ".cache" / f"names-{report.data.get('runtime', 'unknown')}.json"
        self.cache = self._load_json(self.cache_path) or {}
        self.rename = self._load_rename(args.rename) if self.edition == "AE" else {}
        self.commonlib = self._load_commonlib()
        self.ghidra = None
        if not args.no_ghidra:
            ghidra = Ghidra(args.ghidra, args.program)
            if ghidra.connect():
                self.ghidra = ghidra

    @staticmethod
    def _load_json(path):
        try:
            return json.loads(Path(path).read_text(encoding="utf-8"))
        except (OSError, ValueError):
            return None

    @staticmethod
    def _load_rename(path):
        names = {}
        try:
            for line in Path(path).read_text(encoding="utf-8", errors="replace").splitlines():
                parts = line.split(" ", 1)
                if len(parts) == 2 and parts[0].isdigit():
                    names[int(parts[0])] = parts[1].strip().removesuffix("_*")
        except OSError:
            print(f"Address Library rename file not found at {path}; skipping it", file=sys.stderr)
        return names

    def _load_commonlib(self):
        path = HERE / "commonlib_ids.json"
        if not path.exists():
            try:
                from mine_commonlib_ids import mine
                ae, se, pairs = mine(HERE.parents[1] / "CommonLibSSE")
                data = {"ae": {str(k): v for k, v in ae.items()}, "se": {str(k): v for k, v in se.items()},
                        "aeToSe": {str(k): v for k, v in pairs.items()}}
                path.write_text(json.dumps(data, indent=1), encoding="utf-8")
            except Exception as error:  # Best effort: the tool works without it.
                print(f"Could not mine CommonLibSSE names ({error})", file=sys.stderr)
                return {"ae": {}, "se": {}, "aeToSe": {}}
        return self._load_json(path) or {"ae": {}, "se": {}, "aeToSe": {}}

    def save(self):
        self.cache_path.parent.mkdir(exist_ok=True)
        self.cache_path.write_text(json.dumps(self.cache, indent=0, sort_keys=True), encoding="utf-8")

    def se_id(self, ae_id):
        if self.edition != "AE" or ae_id is None:
            return None
        return self.commonlib["aeToSe"].get(str(ae_id))

    def game_function(self, function_rva, address_id):
        """Returns {'name', 'signature', 'source'} for a game-module function."""
        key = str(function_rva)
        if key in self.cache:
            return self.cache[key]

        result = None
        ghidra_fallback = None
        if self.ghidra:
            try:
                found = self.ghidra.function(function_rva)
            except (OSError, urllib.error.URLError):
                found = None
            if found:
                name, signature = found
                entry = {"name": name, "signature": signature, "source": "ghidra"}
                if WEAK_NAME.search(name):
                    ghidra_fallback = entry
                else:
                    result = entry

        if not result and address_id is not None:
            table = self.commonlib["ae" if self.edition == "AE" else "se"]
            if address_id in self.rename:
                result = {"name": self.rename[address_id], "signature": "", "source": "rename"}
            elif str(address_id) in table:
                result = {"name": table[str(address_id)], "signature": "", "source": "commonlib"}

        if not result:
            result = ghidra_fallback or {"name": f"sub_{function_rva:X}", "signature": "", "source": "none"}
        if ghidra_fallback and result is not ghidra_fallback and not result["signature"]:
            result["signature"] = ghidra_fallback["signature"]

        if self.ghidra:
            # Only cache complete lookups; an offline run would pin weaker names.
            self.cache[key] = result
        return result


# --------------------------------------------------------------------------
# Report model
# --------------------------------------------------------------------------


class Report:
    def __init__(self, path):
        self.data = json.loads(Path(path).read_text(encoding="utf-8"))
        if self.data.get("schema") != 1:
            print(f"Warning: report schema {self.data.get('schema')} (tool expects 1)", file=sys.stderr)
        self.modules = self.data["modules"]
        self.frames = self.data["frames"]
        self.threads = self.data["threads"]
        self.zones = self.data["zones"]
        self.stacks = self.data["stacks"]

    def module(self, frame_index):
        index = self.frames[frame_index]["module"]
        return self.modules[index] if index >= 0 else None

    def kind(self, frame_index):
        module = self.module(frame_index)
        return module["kind"] if module else "unknown"

    def function_key(self, frame_index):
        """Groups frames by containing function (or by address when unknown)."""
        frame = self.frames[frame_index]
        return frame["module"], frame["function"] if frame["function"] is not None else frame["rva"]


class Labeler:
    def __init__(self, report, names):
        self.report = report
        self.names = names

    def function(self, frame_index):
        frame = self.report.frames[frame_index]
        module = self.report.module(frame_index)
        if module is None:
            return f"0x{frame['rva']:X}"
        if module["kind"] == "game" and frame["function"] is not None:
            info = self.names.game_function(frame["function"], frame["id"])
            suffix = f" [ID {frame['id']}]" if frame["id"] is not None else f" [+0x{frame['function']:X}]"
            return f"{info['name']}{suffix}"
        if module["kind"] == "plugin":
            return f"(SkyrimEngineTelemetry+0x{frame['rva']:X})"
        rva = frame["function"] if frame["function"] is not None else frame["rva"]
        return f"{module['name']}+0x{rva:X}"

    def call_site(self, frame_index):
        """Describes where a return-address frame's call happens, relative to its function."""
        frame = self.report.frames[frame_index]
        call = frame.get("call")
        if not call or frame["function"] is None:
            return "?"
        offset = call["rva"] - frame["function"]
        owner = f"ID {frame['id']}" if frame["id"] is not None else f"{self.report.module(frame_index)['name']}+0x{frame['function']:X}"
        detail = call["kind"]
        if call["kind"] == "mem-indirect" and call["disp"] >= 0 and call["disp"] % 8 == 0:
            detail += f", vtable slot {call['disp'] // 8}?"
        return f"{owner} + 0x{offset:X} ({detail})"


# --------------------------------------------------------------------------
# Analysis
# --------------------------------------------------------------------------


def on_cpu(thread):
    samples = thread["samples"]
    return samples["zoned"] + samples["unscoped"]


def attribute_zone_sample(report, stack):
    """For a zoned sample, returns (kind, callee_frame, callsite_frame).

    Walks from the leaf to the innermost zone's thunk (the first plugin frame).
    The frame just below it is the hooked engine function H; the one below H
    is the callee responsible for H's self time.
    """
    frames = stack["frames"]
    if frames and report.kind(frames[0]) == "plugin":
        return "overhead", None, None
    for index, frame in enumerate(frames):
        if report.kind(frame) == "plugin":
            if index == 0:
                return "overhead", None, None
            if index == 1:
                return "self", frames[0], None
            return "callee", frames[index - 2], frames[index - 1]
    return "unattributed", None, None


def build_tree(report, stacks):
    """Top-down trie of stacks, rooted at the first game frame."""
    root = {"count": 0, "children": {}, "frame": None}
    for stack in stacks:
        frames = list(reversed(stack["frames"]))
        start = 0
        while start < len(frames) and report.kind(frames[start]) != "game":
            start += 1
        if start == len(frames):
            start = 0
        node = root
        node["count"] += stack["count"]
        for frame in frames[start:]:
            key = report.function_key(frame)
            child = node["children"].get(key)
            if child is None:
                child = {"count": 0, "children": {}, "frame": frame}
                node["children"][key] = child
            child["count"] += stack["count"]
            node = child
    return root


def find_fanout(node, minimum):
    """First node (walking the heaviest path) whose samples split across children."""
    while node["children"]:
        heaviest = max(node["children"].values(), key=lambda child: child["count"])
        if node["frame"] is not None and heaviest["count"] < 0.8 * node["count"] and node["count"] >= minimum:
            return node
        node = heaviest
    return node if node["frame"] is not None else None


def render_tree(node, labeler, total, minimum, depth, lines, fanout, max_depth=14):
    children = sorted(node["children"].values(), key=lambda child: -child["count"])
    for child in children:
        if child["count"] < minimum:
            continue
        marker = " ◆ root-zone candidate" if child is fanout else ""
        lines.append(f"{'  ' * depth}{100 * child['count'] / total:5.1f}%  {labeler.function(child['frame'])}{marker}")
        if depth < max_depth:
            render_tree(child, labeler, total, minimum, depth + 1, lines, fanout, max_depth)


# --------------------------------------------------------------------------
# Hook stub generation
# --------------------------------------------------------------------------

GHIDRA_TYPES = {
    "void": "void", "bool": "bool", "char": "char", "byte": "std::uint8_t", "undefined": "void",
    "undefined1": "std::uint8_t", "undefined2": "std::uint16_t", "undefined4": "std::uint32_t",
    "undefined8": "std::uint64_t", "short": "std::int16_t", "ushort": "std::uint16_t", "int": "std::int32_t",
    "uint": "std::uint32_t", "longlong": "std::int64_t", "ulonglong": "std::uint64_t", "float": "float",
    "double": "double",
}


def map_signature(signature):
    """Best-effort Ghidra signature -> (return type, [param types]); None when unsure."""
    match = re.match(r"^\s*(.+?)\s+(?:__\w+\s+)?[\w:~<>]+\s*\((.*)\)\s*$", signature or "")
    if not match:
        return None

    def to_cpp(text):
        text = text.strip()
        if "*" in text:
            return "void*"
        return GHIDRA_TYPES.get(text.split()[-1] if text else "", None)

    returned = to_cpp(match.group(1))
    params = []
    body = match.group(2).strip()
    if body and body != "void":
        for part in body.split(","):
            part = re.sub(r"\s+\w+$", "", part.strip())  # Drop the parameter name.
            cpp = to_cpp(part)
            if cpp is None:
                return None
            params.append(cpp)
    if returned is None:
        return None
    return returned, params


def stub(name, ae_id, se_id, signature, category):
    ident = re.sub(r"\W+", "_", name).strip("_") or "Unnamed"
    mapped = map_signature(signature)
    notes = []
    if mapped and signature and "(void)" not in signature:
        returned, params = mapped
    else:
        returned, params = "std::uint64_t", ["void*", "void*", "void*", "void*"]
        notes.append("// TODO: signature unknown. This passthrough forwards rcx/rdx/r8/r9 only; float/double")
        notes.append("//       arguments (xmm0-3) and stack arguments are NOT preserved. Fix before shipping.")
    if signature:
        notes.append(f"// Ghidra: {signature}")

    args = ", ".join(f"{t} a_{i}" for i, t in enumerate(params))
    forward = ", ".join(f"a_{i}" for i in range(len(params)))
    if se_id is not None:
        rel = f"RELOCATION_ID({se_id}, {ae_id});"
    else:
        rel = f"REL::ID({ae_id});  // AE ID; SE ID unknown"
    call = f"original({forward});" if returned == "void" else f"return original({forward});"
    lines = notes + [
        f"struct {ident}",
        "{",
        f"\tstatic constexpr auto    name = \"{name}\"sv;",
        f"\tstatic constexpr REL::ID id = {rel}",
        "",
        f"\tstatic {returned} thunk({args})",
        "\t{",
        f"\t\tSET_ZONE(\"{name}\", {category});",
        f"\t\t{call}",
        "\t}",
        "",
        "\tstatic inline decltype(&thunk) original{ nullptr };",
        "};",
        f"// Install(): AttachFunctionDetour<{ident}>(transaction);",
    ]
    return "\n".join(lines)


# --------------------------------------------------------------------------
# Main
# --------------------------------------------------------------------------


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("report", type=Path)
    parser.add_argument("-o", "--out", type=Path, help="Markdown output (default: next to the report)")
    parser.add_argument("--ghidra", default="http://127.0.0.1:8089")
    parser.add_argument("--program", help="Ghidra program name or path (default: the current program)")
    parser.add_argument("--no-ghidra", action="store_true")
    parser.add_argument("--rename", type=Path, default=DEFAULT_RENAME, help="Address Library Database .rename file")
    parser.add_argument("--top", type=int, default=15, help="Number of hook candidates to emit")
    parser.add_argument("--min-share", type=float, default=1.0,
                        help="Hide rows below this percentage of the thread's on-CPU samples")
    parser.add_argument("--context", action="store_true",
                        help="Add Ghidra callers/callees and a decompile excerpt for each candidate")
    args = parser.parse_args()

    report = Report(args.report)
    names = Names(report, args)
    labeler = Labeler(report, names)
    out = []
    candidates = []  # (samples, thread_index, frame_index, reason, category, callsite_frame)

    data = report.data
    out.append(f"# Attribution report: {args.report.name}\n")
    out.append(f"- Runtime {data['runtime']} ({data['edition']}), {data['plugin']}")
    out.append(f"- {data['durationSec']:.0f} s, {data['samples']:,} on-CPU samples over {data['rounds']:,} rounds "
               f"at {data['intervalUs']} µs; suspend mean {data['suspendUs']['mean']:.1f} µs, "
               f"max {data['suspendUs']['max']:.0f} µs")
    if data["droppedStacks"]:
        out.append(f"- **{data['droppedStacks']:,} samples dropped** (unique-stack limit reached)")
    names_note = "ghidra-mcp" if names.ghidra else "no Ghidra"
    out.append(f"- Names: {names_note}, {len(names.rename):,} rename entries, "
               f"{len(names.commonlib['ae']):,} CommonLibSSE entries\n")

    # ---- Threads ----
    stacks_by_thread = defaultdict(list)
    for stack in report.stacks:
        stacks_by_thread[stack["thread"]].append(stack)

    live_cycles = sum(t["cycles"] for t in report.threads if not t["excluded"]) or 1
    order = sorted(range(len(report.threads)), key=lambda i: (-on_cpu(report.threads[i]), -report.threads[i]["cycles"]))

    out.append("## Threads\n")
    out.append("| Thread | TID | Origin | Start | CPU share | On-CPU samples | Zoned | Unscoped | Blocked | Idle |")
    out.append("|---|---|---|---|---:|---:|---:|---:|---:|---:|")
    for index in order:
        thread = report.threads[index]
        if thread["excluded"]:
            continue
        samples = thread["samples"]
        running = on_cpu(thread)
        if running == 0 and thread["cycles"] == 0:
            continue
        start = labeler.function(thread["start"]) if thread["start"] is not None else "?"
        origin = thread["origin"] + (f" ({thread['createdBy']})" if thread["createdBy"] else "")
        name = thread["name"] + ("" if thread["alive"] else " †")
        zoned = f"{100 * samples['zoned'] / running:.0f}%" if running else "–"
        unscoped = f"{100 * samples['unscoped'] / running:.0f}%" if running else "–"
        out.append(f"| {name} | {thread['tid']} | {origin} | {start} | {100 * thread['cycles'] / live_cycles:.1f}% | "
                   f"{running:,} | {zoned} | {unscoped} | {samples['blocked']:,} | {samples['idle']:,} |")
    out.append("\n† exited. Zoned/Unscoped are shares of on-CPU samples; the goal is 100% zoned on every busy thread.\n")

    # ---- Zone self-time ----
    out.append("## Zone self-time by callee\n")
    out.append("Time inside a zone that no child zone covers, split by the function the hooked function was "
               "calling. The top rows are the next child zones to add.\n")
    zone_totals = Counter()
    zone_breakdown = defaultdict(Counter)
    zone_thread = defaultdict(Counter)
    callers_of = defaultdict(set)  # function key -> distinct caller function keys (fan-in)

    for stack in report.stacks:
        frames = stack["frames"]
        for lower, upper in zip(frames, frames[1:]):
            callers_of[report.function_key(lower)].add(report.function_key(upper))
        if stack["blocked"] or not stack["zones"]:
            continue
        zone = stack["zones"][-1]
        zone_totals[zone] += stack["count"]
        zone_thread[zone][stack["thread"]] += stack["count"]
        kind, callee, callsite = attribute_zone_sample(report, stack)
        if kind == "callee":
            zone_breakdown[zone][("callee", report.function_key(callee), callsite)] += stack["count"]
        elif kind == "self":
            zone_breakdown[zone][("self", report.function_key(callee), None)] += stack["count"]
        else:
            zone_breakdown[zone][(kind, None, None)] += stack["count"]

    representative = {}
    for stack in report.stacks:
        for frame in stack["frames"]:
            representative.setdefault(report.function_key(frame), frame)

    for zone, total in zone_totals.most_common():
        info = report.zones[zone]
        thread_index = zone_thread[zone].most_common(1)[0][0]
        thread_running = on_cpu(report.threads[thread_index]) or 1
        out.append(f"### {info['name']} ({info['category']}): {total:,} self samples, "
                   f"{100 * total / thread_running:.1f}% of {report.threads[thread_index]['name']}\n")
        out.append("| Share of zone self | Callee | Call site | Samples |")
        out.append("|---:|---|---|---:|")
        for (kind, key, callsite), count in zone_breakdown[zone].most_common():
            share = 100 * count / total
            if share < args.min_share and kind == "callee":
                continue
            if kind == "callee":
                frame = representative[key]
                fan_in = len(callers_of[key])
                note = f" ⚠ {fan_in} callers" if fan_in >= 3 else ""
                out.append(f"| {share:.1f}% | {labeler.function(frame)}{note} | {labeler.call_site(callsite)} | {count:,} |")
                candidates.append((count, thread_index, frame, f"child of `{info['name']}`", info["category"],
                                   callsite if fan_in >= 3 else None))
            elif kind == "self":
                out.append(f"| {share:.1f}% | *(self: code in {labeler.function(representative[key])})* | | {count:,} |")
            else:
                out.append(f"| {share:.1f}% | *({kind})* | | {count:,} |")
        out.append("")

    # ---- Unscoped trees ----
    out.append("## Unscoped work by thread\n")
    out.append("Top-down call trees of on-CPU samples outside every zone, rooted at the thread's first engine frame. "
               "◆ marks where the heaviest path first fans out: a natural root zone for the thread, with its "
               "children as the first child zones.\n")
    for index in order:
        thread = report.threads[index]
        unscoped_stacks = [s for s in stacks_by_thread.get(index, []) if not s["blocked"] and not s["zones"]]
        total = sum(s["count"] for s in unscoped_stacks)
        running = on_cpu(thread)
        if not total or 100 * total / (running or 1) < args.min_share:
            continue
        minimum = max(1, int(total * args.min_share / 100))
        tree = build_tree(report, unscoped_stacks)
        fanout = find_fanout(tree, minimum)
        truncated = sum(s["count"] for s in unscoped_stacks if s["truncated"])
        out.append(f"### {thread['name']} (tid {thread['tid']}): {total:,} unscoped samples"
                   + (f", {100 * truncated / total:.0f}% truncated stacks" if truncated else "") + "\n")
        lines = []
        render_tree(tree, labeler, total, minimum, 0, lines, fanout)
        out.append("```\n" + "\n".join(lines) + "\n```\n")
        if fanout:
            candidates.append((fanout["count"], index, fanout["frame"], f"root zone for `{thread['name']}`", None, None))
            for child in sorted(fanout["children"].values(), key=lambda c: -c["count"]):
                if child["count"] >= max(minimum, total * 0.05) and report.kind(child["frame"]) == "game":
                    candidates.append((child["count"], index, child["frame"],
                                       f"child zone under root candidate on `{thread['name']}`", None, None))

    # ---- Blocking call sites ----
    blocked = Counter()
    for stack in report.stacks:
        if not stack["blocked"]:
            continue
        for frame in stack["frames"]:
            if report.kind(frame) == "game":
                blocked[(stack["thread"], report.function_key(frame))] += stack["count"]
                break
    if blocked:
        out.append("## Where running threads block\n")
        out.append("Innermost engine frame of samples that caught a thread inside a syscall (waits, I/O). "
                   "Useful for naming sync points; not counted in coverage.\n")
        out.append("| Thread | Engine frame | Samples |")
        out.append("|---|---|---:|")
        for (thread_index, key), count in blocked.most_common(20):
            out.append(f"| {report.threads[thread_index]['name']} | {labeler.function(representative[key])} | {count:,} |")
        out.append("")

    # ---- Hook candidates ----
    out.append("## Hook candidates\n")
    zone_lines_at = len(out)
    zone_lines = []
    out.append("Per-candidate detail follows, ranked by samples. The typed stubs (in the `src/Hooks/FrameHooks.cpp` "
               "style) are only needed when a zone should read the function's arguments; verify their signatures in "
               "Ghidra first. ⚠ marks shared helpers where a call-site hook beats a whole-function detour.\n")
    seen = set()
    emitted = 0
    for count, thread_index, frame, reason, category, callsite in sorted(candidates, key=lambda c: -c[0]):
        info = report.frames[frame]
        if report.kind(frame) != "game" or info["function"] is None or info["function"] in seen:
            continue
        seen.add(info["function"])
        emitted += 1
        if emitted > args.top:
            break

        name_info = names.game_function(info["function"], info["id"])
        name = name_info["name"]
        thread = report.threads[thread_index]
        share = 100 * count / (on_cpu(thread) or 1)
        out.append(f"### {emitted}. {name}: {share:.1f}% of {thread['name']}\n")
        out.append(f"- {reason}; function RVA 0x{info['function']:X}"
                   + (f", Address Library ID {info['id']}" if info["id"] is not None else ", **no Address Library ID**"))
        if callsite is not None:
            out.append(f"- ⚠ Shared helper. Prefer hooking the call at {labeler.call_site(callsite)} with "
                       f"`write_call<5>` (direct) or a vtable patch.")

        if args.context and names.ghidra:
            try:
                callers = names.ghidra.callers(info["function"])
                callees = names.ghidra.callees(info["function"])
                out.append(f"- Callers: {', '.join(callers[:8]) or '?'}")
                out.append(f"- Callees: {', '.join(callees[:8]) or '?'}")
                out.append("\n<details><summary>Decompile excerpt</summary>\n\n```c\n"
                           + names.ghidra.decompile(info["function"]) + "\n```\n</details>")
            except (OSError, urllib.error.URLError) as error:
                out.append(f"- (Ghidra context unavailable: {error})")

        if info["id"] is not None:
            se_id = names.se_id(info["id"])
            id_text = f"{se_id},{info['id']}" if se_id is not None else str(info["id"])
            zone_lines.append(f"{id_text:<16} {category or 'Engine':<10} {name}"
                              + ("    # shared helper, see note" if callsite is not None else ""))
            zone_category = f"\"{category or 'Engine'}\""
            code = stub(name, info["id"], se_id, name_info.get("signature", ""), zone_category)
            out.append("\n<details><summary>Typed stub</summary>\n\n```cpp\n" + code + "\n```\n</details>\n")
        else:
            out.append("\nNo Address Library ID: hook by pattern or by a call site in a function that has one.\n")

    if zone_lines:
        out[zone_lines_at:zone_lines_at] = [
            "Append these lines to `SKSE/Plugins/SkyrimEngineTelemetry.zones` to zone the candidates without "
            "recompiling. Zone hooks need no signature: every argument and return register is preserved.\n",
            "```\n# <id or seId,aeId>  <category>  <name>\n" + "\n".join(zone_lines) + "\n```\n",
        ]

    names.save()
    target = args.out or args.report.with_suffix(".md")
    target.write_text("\n".join(out) + "\n", encoding="utf-8")
    print(f"Wrote {target}")


if __name__ == "__main__":
    main()
