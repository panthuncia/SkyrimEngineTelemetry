# SkyrimEngineTelemetry

An SKSE plugin that instruments Skyrim's engine threads with [BasicTelemetry](https://github.com/panthuncia/BasicTelemetry) zones, which are emitted to both the BasicTelemetry session and Tracy.

The goal is 100% coverage of Skyrim's thread scopes, with enough detail to attribute major frame and worker costs.

## Build

Requires Visual Studio 2026 (or 2022 via the `vs2022-msvc` preset), CMake 3.25+, and `VCPKG_ROOT` pointing at a vcpkg checkout.

```powershell
git submodule update --init --recursive
cmake --preset vs2026-msvc
cmake --build --preset relwithdebinfo-msvc
```

The plugin is written to `build/<preset>/out/<config>/SKSE/Plugins/SkyrimEngineTelemetry.dll`.

`commonlib-shared` is resolved from CommonLibSSE's overlay ports (`vcpkg-configuration.json`), so the vcpkg baseline tracks the CommonLibSSE submodule's.

## Deploy

Put a Skyrim `Data` directory or MO2 mod folder path in `install-prefix.txt` (gitignored), or pass `-DSET_SKYRIM_DIR=...`. With that set, every build auto-installs to `<dir>/SKSE/Plugins`, and a `deploy` target is available.

For the standard local Skyrim/MO2 layout, `tools/run-mo2.ps1` configures and builds the plugin, deploys it to `C:\Modding\MO2\mods\Skyrim Engine Telemetry`, enables the mod in the selected profile, enables the sampler and BasicTelemetry summary capture, and launches SKSE through MO2:

```powershell
.\tools\run-mo2.ps1 -NoWait
```

Omit `-NoWait` to keep the script attached until Skyrim exits and collect the SKSE logs into `build/game-runs/<timestamp>`.

Each run writes BasicTelemetry artifacts directly to `build/game-runs/<timestamp>/basic-telemetry`. Use `-BasicTelemetryMode Trace` when individual scope events and parent relationships are needed, or `-BasicTelemetryMode Off` to disable it. The build also produces `basic-telemetry.exe`, which can query `profile.sqlite`, for example:

```powershell
.\build\vs2026-msvc\BasicTelemetry\RelWithDebInfo\basic-telemetry.exe query .\build\game-runs\<timestamp>\basic-telemetry --metric self --top 25
```

## Profiling

Tracy is built with `TRACY_ON_DEMAND`, so nothing is recorded until a Tracy profiler connects. Launch the game, then connect the Tracy profiler (0.14.0, matching the `ThirdParty/tracy` submodule) to `localhost`.

## Default coverage

Whole-function detours, installed at `kPostLoad` in a single Detours transaction:

| Zone | Category | Address Library ID (SE / AE) |
| --- | --- | --- |
| `Main::Update` (+ frame mark) | Frame | 35565 / 36564 |
| `Main::RenderPlayerView` | Render | 35560 / 36559 |
| `Main::RenderDepth` | Render | 100421 / 107139 |
| `Main::RenderShadowmasks` | Render | 100422 / 107140 |
| `Main::RenderWorld` | Render | 100424 / 107142 |
| `Main::RenderFirstPersonView` | Render | 100411 / 107129 |
| `Main::RenderWaterEffects` | Render | 35561 / 36560 |
| `BSGraphics::Renderer::Begin` | Render | 75460 / 77245 |
| `BSGraphics::Renderer::End (Present)` | Render | 75461 / 77246 |

### Zones file

The quickest way to add a zone is a line in `SKSE/Plugins/SkyrimEngineTelemetry.zones`, which is read at `kPostLoad`:

```
# <id or seId,aeId>  <category>  <name>
100424,107142  Render  Main::RenderWorld
75445,77226    Render  BSGraphics::Renderer::Init
```

Each line detours the function at that Address Library ID through a generic assembly stub (`src/Hooks/ZoneHookStub.asm`). No signature or rebuild is needed, and `tools/attribution/analyze.py` prints lines ready to paste.

How the stub works:
- It is a real frame with unwind data. It calls the original function with the caller's register arguments (`rcx`, `rdx`, `r8`, `r9`, `xmm0`-`xmm5`) and up to 16 stack arguments reproduced exactly, then returns `rax`, `rdx` and `xmm0`-`xmm3`.
- Exceptions and stack walks pass through it normally. If an exception unwinds past a hooked call, the stub's unwind handler closes that call's zone.

Checks and limits:
- Every ID is checked against the Address Library and must be the entry point of a function in the game executable.
- Each hook installs in its own transaction, so one bad line can't block the others.
- Functions that take more than 20 arguments, or that read their own return address, are not supported.
- Every call pays for two calls into the plugin plus a 128-byte copy. Don't zone functions that run millions of times per frame.

### Typed hooks

To add a whole-function zone that needs the function's arguments (for example, to annotate the zone), add a hook struct (`name`, `id`, `thunk`, `original`) in `src/Hooks/FrameHooks.cpp` and add an `AttachFunctionDetour<Hook>` call for it in `Install()`. Open every zone with `SET_ZONE(name, category)` rather than `BT_ZONE_*`. `SET_ZONE` also records the zone on a per-thread stack that the sampler reads, and a zone opened any other way counts as unscoped.

## Threads

The plugin patches the game executable's `CreateThread`, `_beginthread` and `_beginthreadex` imports in `SKSEPluginLoad`, before the engine creates most of its threads. Each new thread goes through a start trampoline that records the thread's real start routine. If the start parameter is an engine object (for example a `BSThread` subclass), the trampoline also records its RTTI class name and uses it to name the thread. Threads created elsewhere (drivers, audio, other plugins) are picked up by Toolhelp rescans and named after their start routine (`module+0xRVA`). Names the engine sends with the MSVC thread-naming exception take precedence over both. Tracy shows all of these names. At `kDataLoaded`, the thread table is logged to the SKSE log.

## Finding unscoped work

`SkyrimEngineTelemetry.ini` enables a development-only stack sampler, which is off by default:

| Key | Default | Meaning |
| --- | --- | --- |
| `[Threads] bNameThreads` | 1 | Name unnamed threads. |
| `[Threads] bLogThreads` | 1 | Log the thread table at `kDataLoaded`. |
| `[Sampler] bEnable` | 0 | Run the sampler. |
| `[Sampler] iIntervalUs` | 1000 | Time between sampling rounds. Each round samples every thread that used CPU since its last sample. |
| `[Sampler] iReportIntervalSec` | 30 | How often the cumulative report is rewritten. |
| `[BasicTelemetry] bCapture` | 0 | Start the in-process timing/statistics session. The runner enables it. |
| `[BasicTelemetry] sMode` | Summary | `Summary` aggregates distributions; `Trace` also retains individual events. |
| `[BasicTelemetry] sOutputDirectory` | next to plugin | Artifact output directory. |
| `[BasicTelemetry] iSnapshotIntervalSec` | 10 | Periodically rewrite artifacts so forced exits still retain data. |
| `[BasicTelemetry] iMaximumTraceEvents` | 1000000 | Maximum retained events in Trace mode. |
| `[BasicTelemetry] bWriteSqlite` | 1 | Write the queryable `profile.sqlite` database. |
| `[BasicTelemetry] bWriteMarkdown` | 1 | Write the human-readable `summary.md`. |
| `[BasicTelemetry] bMeasureThreadCpuTime` | 0 | Measure per-scope thread CPU time; off by default due to hot-path overhead. |

For each thread, the sampler:

1. suspends the thread briefly;
2. copies its `SET_ZONE` stack;
3. unwinds its call stack using the modules' `.pdata`, with a lock-free lookup so a suspended thread holding a loader or heap lock cannot deadlock the sampler;
4. resumes it.

Each sample is classed as zoned, unscoped, blocked (inside a syscall) or idle. Every frame is resolved to module, function start, Address Library ID and the decoded call instruction. The resulting report goes to `Documents/My Games/Skyrim Special Edition/SKSE/SkyrimEngineTelemetry/attribution-<start>.json`.

`tools/attribution/analyze.py` turns a report into Markdown that answers two questions: which threads are busy but unscoped, and which callees account for each zone's self time. The output also includes ready-to-paste hook stubs, with names from Ghidra (via ghidra-mcp), the Address Library Database and CommonLibSSE. See [tools/attribution/README.md](tools/attribution/README.md).

## Roadmap

- Split the rest of `Main::Update`: script VM, AI/process lists, Havok, animation, audio, UI, and the job-list dispatch.
- Zone the engine's worker threads (the job system, IO/`BSResource` streaming, the AI linear task threads, Havok, and audio), guided by the sampler's unscoped-work trees.
- Call-site zone hooks (`write_call`) for hot shared helpers, so a zone can cover one caller's use of a function.
- Add per-pass render detail: shadow maps via the `BSShadowLight` vfuncs, the `BSShaderAccumulator`/`BSBatchRenderer` passes, and image-space effects.
- Add capture controls for excluding startup/menu frames and delimiting repeatable benchmark windows.
