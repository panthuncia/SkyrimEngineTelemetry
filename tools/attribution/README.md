# Attribution tools

Offline half of the unscoped-work discovery loop. The in-game sampler writes JSON reports. `analyze.py` turns a report into a ranked, named Markdown report with zones-file lines (and optional typed hook stubs) you can paste in.

## Workflow

1. Set `bEnable=1` under `[Sampler]` in `SKSE/Plugins/SkyrimEngineTelemetry.ini`, then play the scene you care about. The report file is rewritten every `iReportIntervalSec` seconds and is cumulative for the session:
   `Documents/My Games/Skyrim Special Edition/SKSE/SkyrimEngineTelemetry/attribution-<start time>.json`
2. Open the BethesdaGhidraScripts project in Ghidra, with the ghidra-mcp server listening on `127.0.0.1:8089`.
3. Run the analyzer:
   ```powershell
   python tools/attribution/analyze.py "<path>\attribution-20260922-101500.json" --context
   ```
4. Read the `.md` file written next to the report. Copy the lines from its **Hook candidates** block that you want into `SKSE/Plugins/SkyrimEngineTelemetry.zones`, restart the game, and repeat. No rebuild is needed. Use the typed stubs in `src/Hooks/FrameHooks.cpp` only when a zone needs the function's arguments; fix their signatures first (see the TODOs). Progress shows up as the **Zoned** column rising toward 100% on every busy thread.

Python 3.10+ is required. The tools use only the standard library.

## analyze.py

| Option | Meaning |
| --- | --- |
| `-o PATH` | Markdown output. Defaults to the report path with a `.md` extension. |
| `--ghidra URL` / `--no-ghidra` | ghidra-mcp endpoint. The default is `http://127.0.0.1:8089`. |
| `--program NAME` | Ghidra program name or path. Defaults to the active program. The program must be the same game version as the report. |
| `--rename PATH` | Address Library Database `skyrimae.rename` file (AE ID → name). The default is the BethesdaGhidraScripts checkout under `~/source/repos`. |
| `--top N` | Number of hook candidates to emit (default 15). |
| `--min-share PCT` | Hides tree and table rows below this share of a thread's on-CPU samples (default 1). |
| `--context` | Adds Ghidra callers, callees and a decompile excerpt to each candidate. |

Names are resolved in this order:

1. `.cache/names-<runtime>.json`
2. Ghidra. Names starting with `FUN_` or ending in `::sub` count as weak and fall through to the next sources.
3. The `.rename` file.
4. `commonlib_ids.json`
5. `sub_<rva>`

Delete `.cache/` after renaming functions in Ghidra so the new names are picked up.

### Reading the output

- **Threads**
  - **CPU share** comes from thread cycle counters.
  - **Zoned** and **Unscoped** are shares of *on-CPU* samples.
  - **Blocked** samples caught a thread that had run since its last sample but was inside a syscall.
  - **Idle** samples were skipped because the thread used no CPU. They count neither for nor against coverage.
- **Zone self-time by callee**: for each zone, the samples that no child zone covers are split by the function the hooked function was calling. The call site is given as `ID <hooked fn> + 0x<offset>`. The top rows are the next child zones to add.
  - `self` means the hooked function's own code.
  - `overhead` means the sample landed inside the plugin's instrumentation.
  - `unattributed` means the stack had no plugin thunk frame, usually because it was truncated.
- **Unscoped work by thread**: a top-down tree rooted at the thread's first engine frame. ◆ marks the first fan-out on the heaviest path. That node is a natural root zone for the thread, and its children are the first child zones.
- **Hook candidates**: a block of zones-file lines for every candidate that has an Address Library ID, followed by per-candidate detail with an optional typed stub in the `FrameHooks.cpp` style.
  - ⚠ marks functions with three or more distinct callers. Prefer a call-site hook (`write_call<5>` at the given offset) or a vtable patch over a whole-function detour for these.
  - Typed-stub signatures come from Ghidra and are often wrong or `(void)`. The typed fallback passthrough does **not** preserve float or stack arguments. Zones-file hooks don't have this problem.

## mine_commonlib_ids.py

This script regenerates `commonlib_ids.json`: AE and SE function names, plus AE→SE ID pairs, taken from CommonLibSSE's `RELOCATION_ID` wrappers. `analyze.py` runs it automatically when the file is missing. Re-run it after updating the CommonLibSSE submodule.

## Report schema (version 1)

The top-level fields are:
- `runtime`, `edition`, `imageBase`
- `durationSec`, `intervalUs`, `rounds`, `samples`, `droppedStacks`
- `suspendUs` {`max`, `mean`}

The arrays are listed below. Objects refer to each other by array index.

- `modules[]`: `name`, `base`, `size`, `kind` (`game` | `plugin` | `other`), `loaded`.
- `frames[]`: `module` (index, or -1), `rva`, `function` (containing function's RVA, from `.pdata` with chained entries resolved; `null` for frameless leaves), `id` (Address Library ID of `function`, game module only), and optionally `call` {`kind` (`direct` | `rip-indirect` | `mem-indirect` | `register`), `rva` of the call instruction, `target` (direct calls), `disp`}. A frame is a return address when it is not first in its stack.
- `threads[]`:
  - `tid`, `name`, `description`, `engineName`, `rtti`, `origin` (`existing` | `created` | `discovered`), `createdBy`
  - `start` (frame index), `alive`, `excluded`, `cycles`
  - `samples` {`idle`, `blocked`, `zoned`, `unscoped`, `failed`}
- `zones[]`: `name`, `category`.
- `stacks[]`:
  - `thread`, `count`, `blocked`, `truncated`
  - `zoneDepth`, `zones` (outermost first)
  - `frames` (leaf first)
