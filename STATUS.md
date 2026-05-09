# Engineering Notes — `movies_fix.asi`

For the user-facing overview see [`README.md`](README.md). This file is
the working notebook: architecture, reverse-engineering findings, and the
non-obvious bugs we hit getting it stable.

## Status

**Phase 1 done.** All FMV cutscenes play with clean audio/video sync
(libmpv-paced). The Activision logo, frontend intro and menu loop all
work. The game progresses past intros naturally; the menu file is the
only one that loops.

Open work:
1. Movie *export* (in-game studio's `IWMWriter` path) — not started.
2. Widescreen rendering — not started.
3. Trim FFmpeg dependency: `sync_reader.c` is the only consumer; we
   could carve a much smaller static FFmpeg.

## Architecture

```
                ┌──────────────────────────┐
 game ────────► │ Hooked CoCreateInstance  │   src/main.c
                └────┬─────────────────────┘
                     │  FilterGraph → FakeGraph
                     │  WMAsfReader → DSSourceFilter
                     ▼
       ┌─────────────────────────────────────────────┐
       │ FakeGraph                                   │
       │ — IGraphBuilder / IMediaControl /           │
       │   IMediaSeeking / IMediaPosition /          │
       │   IBasicAudio / IMediaEvent in one alloc    │
       └─┬───────────────────────────────────────────┘
         │ AddFilter / Render / Run …
         ▼
       ┌────────────────────┐    ┌──────────────────────┐
       │ DSSourceFilter     │    │ TEXTURERENDERER      │
       │ (libmpv backend)   ├───►│ (game-supplied D3D9  │
       │                    │    │  video sink)         │
       └─────────┬──────────┘    └──────────────────────┘
                 │
                 ▼
         libmpv (decode + WASAPI audio + own A/V clock)
```

`DSSourceFilter` no longer decodes anything. It owns a single
`mpv_player_t`, sets `vo=libmpv` + `MPV_RENDER_API_TYPE_SW`, and runs one
render thread that:

1. Waits on the mpv update event.
2. Calls `mpv_render_context_render` into a BGR0 software buffer.
3. Converts BGR0 → BGR24 (bottom-up, the layout DirectShow `RGB24`
   expects with `biHeight > 0`) into the renderer's allocator buffer.
4. Calls `IMemInputPin::Receive`.

Audio bypasses DirectShow entirely — libmpv plays through WASAPI. The
game's `IBasicAudio::put_Volume` is forwarded to libmpv's `volume`
property (centibels → linear 0..100).

### Sync reader path (in-game movie studio)

For studio-side reads (random-access frames, `WMCreateSyncReader`) we
keep a separate FFmpeg-backed implementation in `src/sync_reader.c`.
libmpv is a poor fit there — it's stream-oriented and we'd be fighting
its scheduler.

## Source files

| File | Purpose |
| --- | --- |
| `src/main.c` | ASI entry, MinHook hooks (`LoadLibraryA/W`, `GetProcAddress`, `CoCreateInstance`, `FUN_009ed380`), vectored exception logger. |
| `src/log.c/h` | `proxy_log()` → `movies_fix.log`, flushed each line. |
| `src/trace.h` | `TRACE_MSG` for COM-stub spam. |
| `src/ds_types.h`, `src/wm_types.h` | Vtable structs + IIDs/CLSIDs (we don't link the Windows SDK ones). |
| `src/ds_fakegraph.c/h` | The `quartz.dll` replacement. |
| `src/ds_filter.c/h` | `DSSourceFilter` — COM front-end, libmpv backend. |
| `src/ds_output_pin.c/h` | Output pin handed to TEXTURERENDERER. BGR24-only. |
| `src/mpv_player.c/h` | Thin C wrapper around libmpv (sw render thread). |
| `src/sync_reader.c/h` | `IWMSyncReader`/`IWMHeaderInfo3` proxy (FFmpeg). |
| `src/profile_mgr.c/h` | `IWMProfileManager`/`IWMProfile`/`IWMStreamConfig` stubs. |
| `src/media_props.c/h` | `IWMOutputMediaProps` for the sync reader. |
| `src/nss_buffer.c/h` | `INSSBuffer` sample carrier. |

## Reverse-engineering findings

Ghidra base `0x00400000`.

| Address | Meaning |
| --- | --- |
| `0x00d73ea4` | FMV Player C++ vtable. `[2]` Open, `[6]` per-frame play tick, `[10]` IsFinished. |
| `0x009EE690` | Per-frame play tick — calls a callback dispatcher then `FUN_009ed380`. |
| `0x009ED380` | "PlayVideo" — hooked for diagnostics. Operates on the *playback controller*, not the FMV Player. |
| `0x009EDD80` | Callback invoked with the FMV Player as context. Calls vtable[11] then vtable[10] (IsFinished); on finished, tail-jumps vtable[7]. |
| `0x009EDDE0` | `FMVPlayer_IsFinished` → `FUN_009ed5a0(playback_controller)`. |
| `0x009ED5A0` | `IsVideoFinished` — only honours states 1 and 2; other states report finished. |
| `0x009ED520` | Reads position/duration via field_8 of the playback controller. |
| `0x009EE800` | `MoviePlayer_Setup`. Builds the graph: `CoCreateInstance(FilterGraph)`, `CoCreateInstance(WMAsfReader)`, Render, then QIs the graph for `IMediaControl/IMediaSeeking/IMediaPosition/IBasicAudio` in that order, storing them at offsets 4/8/C/10. Sets `field_1c = 3` ("setup done"). |

### Playback controller object (0x24 bytes, allocated in `MoviePlayer_Open`)

| Offset | Field |
| --- | --- |
| 0x00 | `IGraphBuilder*` |
| 0x04 | `IMediaControl*` |
| 0x08 | **stored as `IMediaSeeking`, used as `IMediaPosition`** (see gotcha #1) |
| 0x0C | `IMediaPosition*` |
| 0x10 | `IBasicAudio*` |
| 0x14 | 400-byte sub-object (`FUN_009ed710`) |
| 0x18 | 1 |
| 0x1C | playback state (3 = setup, 1 = playing) |

## Gotchas (with fixes)

### 1. Stack overflow from `GetTimeFormat`

`MS_GetTimeFormat` writes a 16-byte GUID to its `*p` argument. The game
stores `IMediaSeeking` at `field_8` but later calls `IMediaPosition`
methods through that same slot, expecting an 8-byte `double*`. The
16-byte GUID write therefore clobbered the saved return address with
`...89 49 00 A0 04 BB CF AB`, producing the “garbage” crash addresses
`0xA0004989` / `0xABCFBB04`.

**Fix:** `FakeGraph::QI(IID_IMediaSeeking)` returns the *IMediaPosition*
interface pointer instead. The vtable slot order for the methods the
game actually calls through this pointer (`get_Duration`,
`put_CurrentPosition`, `get_CurrentPosition`) matches between the two
interfaces, and `__stdcall` arg cleanup sizes match.

See `src/ds_fakegraph.c::FG_QI`.

### 2. `EC_COMPLETE` arriving as `AddFilter`

TEXTURERENDERER calls `IMediaEventSink::Notify(EC_COMPLETE, 0, 0)`
through the `IFilterGraph*` it stored from `JoinFilterGraph`. Slot 3 of
`IMediaEventSink` (`Notify`) maps to slot 3 of `IFilterGraph`
(`AddFilter`), producing `AddFilter(0x00000001, NULL)` and an AV.

**Fix:** `FG_AddFilter` checks `pFilter < 0x10000` and treats the call
as a `Notify(EC_COMPLETE)`, setting `complete_event`.

### 3. Game cycles `Stop`+`Run` every frame

The game's per-frame callback ends up calling `MC_Stop` then `MC_Run`
each tick. With the FFmpeg-era worker threads this caused continuous
audio cut-outs and visible flicker.

**Fix:** under the libmpv backend it's a non-issue: `BF_Stop` just
calls `mpv_player_set_pause(true)` and `BF_Run` calls
`mpv_player_set_pause(false)`. Idempotent, near-instant, no cost.

### 4. EOF detection for non-loop files

When the user hits ESC during an intro, the game stops driving the
filter but waits for it to finish naturally before transitioning. With
`loop-file=inf` the file never ended, so the game hung.

**Fix:**
- Filename heuristic: only files containing `loop` (i.e.
  `frontend_loop.wmv`) get `loop-file=inf`. Intros run with
  `loop-file=no`.
- mpv player option `keep-open=yes` so the file stays paused on the
  last frame instead of being unloaded.
- `mpv_player_get_position()` checks the `eof-reached` property and
  clamps the returned position to `duration` once true. The game's
  `IsVideoFinished` polls position vs. duration and now fires.

See `src/mpv_player.c::mpv_player_get_position` and
`src/ds_filter.c::FS_Load`.

### 5. Vectored exception handler false positives

`KERNEL32` uses SEH internally (`IsBadReadPtr` etc.). Our VEH was
flagging those as crashes and then suppressing the *real* crash with a
once-only flag.

**Fix:** the VEH skips any exception whose EIP is inside a system
module; only AVs in our ASI or in `MoviesSE.exe` are logged. The
once-only flag is replaced by a counter capped at 5.

See `src/main.c::vectored_handler`.

## libmpv quirks worth remembering

- The render API requires `vo=libmpv` set *before* `mpv_initialize`.
- `MPV_RENDER_PARAM_SW_*` parameters: `SW_SIZE` is `int[2]`, `SW_FORMAT`
  is `char*`, `SW_STRIDE` is `size_t*`, `SW_POINTER` is the buffer
  pointer itself (cast to `void*`).
- Stride should be a multiple of 64 for SIMD; we pad with `align64()`.
- The update callback runs on mpv's render thread — header forbids any
  mpv-API call inside it. Just `SetEvent` and return.
- After `mpv_command("loadfile")`, properties like `width`/`height`
  are not valid until `MPV_EVENT_FILE_LOADED` arrives. We block on it
  for up to 5 s in `mpv_player_load`.

## Build / deploy

`build.sh` and `deploy.sh` are the canonical scripts (the old Makefile
was removed because pkg-config silently picked up the wrong FFmpeg). See
the README for prerequisites and one-liner usage.

The output ASI imports only:
- `KERNEL32.dll`, `msvcrt.dll`, `ole32.dll`, `bcrypt.dll` — system.
- `libmpv-2.dll` — shipped alongside the ASI.

FFmpeg is statically linked.
