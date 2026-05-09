# Engineering Notes — `movies_fix.asi`

For the user-facing overview see [`README.md`](README.md). This file is
the working notebook: architecture, reverse-engineering findings, and the
non-obvious bugs we hit getting it stable.

## Status

**Verified scope: launch through main menu only.** All FMV cutscenes
that play around the launch and menu surface work — Activision logo,
frontend intro, frontend background loop — with clean audio/video sync
(libmpv-paced). The game progresses past intros naturally; the menu
file is the only one that loops.

**Movie export** writes a real `.wmv` (libavformat ASF muxer +
libavcodec WMV2). Codec parameter tuning is ongoing — current output
has visible artifacts but the data path is end-to-end functional. The
writer is `src/asf_writer.c`, intercepted at `CLSID_WMAsfWriter`.

**In-game play has not been exercised yet** (starting a studio,
running the simulation, the campaign). Whether additional WMF /
DirectShow paths fire during gameplay is unknown.

Open work:
1. **In-game gameplay verification** — never run past the menu yet;
   file new issues for whatever surfaces.
2. **Movie export image quality** — codec params (bitrate, qmin/qmax,
   sws flags, profile-driven settings) still need tuning to clear the
   pink/cyan striping artifact seen in early frames.
3. Widescreen rendering — not started (#6).
4. Trim FFmpeg dependency: `sync_reader.c` and `asf_writer.c` are the
   only consumers; we could carve a smaller static FFmpeg (#7).

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

### Writer (movie export) call surface

**Headline finding:** the game does **not** call `WMCreateWriter`.
`MoviesSE.exe`'s only delay-loaded `wmvcore.dll` exports are
`WMCreateSyncReader` and `WMCreateProfileManager` — confirmed by
`list_imports`. The exporter is built as a **stock DirectShow capture
graph** with `CLSID_WMAsfWriter` (which internally uses WMF) for the
WMV path and a stock `CLSID_AVIMux` for the AVI path.

The export class (likely `CAviSyst` based on RTTI string
`.?AVCAviSyst@MV@@`) lives in a 0x888-byte allocation. Constructed by
`FUN_00a2cb40`, vtable at `0x00d76cd8`. Global instance pointer:
`DAT_010bab18`. Format flag at object offset `0x83c` (1 = WMV, 0 = AVI),
audio-on flag at `0x828`. Output filename at offset `0x8` is built via
`%s%s%s.{wmv|avi}` based on whether the profile name (offset `0x418`)
contains the substring "Windows Media Format". A temp WAV file path
`<savepath>\TMP%04d.wav` is stored at offset `0x210` — audio is staged
to disk and re-read by an `AsyncReader` filter rather than fed live.

**Vtable** (concrete C++ class, not COM): 7 slots at `0x00d76cd8`.
| Slot | Address | Purpose |
| --- | --- | --- |
| 0 | `0x00a2f9b0` | scalar deleting destructor |
| 1 | `0x00a2ffa0` | **start export** — builds graph + Run |
| 2 | `0x00a2e560` | (unverified — encoder helper?) |
| 3 | `0x00a2e590` | (unverified) |
| 4 | `0x00a2e5b0` | (unverified) |
| 5 | `0x00a2cce0` | (unverified) |
| 6 | `0x00a2ecd0` | (unverified) |

**Object field layout (relevant offsets):**
| Offset | Field |
| --- | --- |
| `0x000` | vtable |
| `0x008` | output filename (WCHAR\*) |
| `0x210` | temp WAV path (WCHAR\*) |
| `0x418` | profile name (WCHAR\*) |
| `0x820` | profile suffix flag (`_NA` if false, "" if true) |
| `0x828` | audio-on flag (BOOL) |
| `0x83c` | format flag (1 = WMV, 0 = AVI) |
| `0x840` | codec config (AVI mode) |
| `0x858` | `IGraphBuilder*` (CLSID_FilterGraph) |
| `0x85c` | `ICaptureGraphBuilder2*` (CLSID_CaptureGraphBuilder2) |
| `0x860` | `IMediaControl*` (graph QI) |
| `0x864` | `IMediaEvent*` (graph QI) |
| `0x868` | (video source filter slot — added to graph) |
| `0x86c` | video compressor filter |
| `0x874` | audio compressor filter |
| `0x878` | audio source — `CLSID_AsyncReader` (the WAV file reader) |
| `0x87c` | `CLSID_WaveParser` |
| `0x880` | mux/writer — **WMV: `CLSID_WMAsfWriter`** / **AVI: AVI MUX (found via `FindInterface`)** |
| `0x884` | `IFileSinkFilter*` (QI from writer, WMV mode only) |

**Setup / start sequence (in `FUN_00a2ffa0`, vtable[1]):**

1. **`FUN_00a2c110`** — base graph creation:
   - `CoCreateInstance(CLSID_FilterGraph,           IID_IGraphBuilder,           &obj_858)`
   - `CoCreateInstance(CLSID_CaptureGraphBuilder2,  IID_ICaptureGraphBuilder2,   &obj_85c)`
   - `obj_85c->SetFiltergraph(obj_858)`
   - `obj_858->QI(IID_IMediaControl, &obj_860)`
   - `obj_858->QI(IID_IMediaEvent,   &obj_864)`
2. **`FUN_00a2f9d0`** — codec / sink setup:
   - **AVI:** call helper `FUN_00a2f8c0`, then
     `obj_85c->FindInterface(MEDIATYPE_Stream, &outname, &obj_880, 0)`
     to obtain the AVI mux.
   - **WMV:** `CoCreateInstance(CLSID_WMAsfWriter, IID_IBaseFilter, &obj_880)`,
     `obj_880->QI(IID_IFileSinkFilter, &obj_884)`,
     `obj_884->SetFileName(<filename>, NULL)`.
   - **Audio (both modes):**
     `CoCreateInstance(CLSID_AsyncReader,  IID_IBaseFilter, &obj_878)` (TMP wav),
     `CoCreateInstance(CLSID_WaveParser,   IID_IBaseFilter, &obj_87c)`.
3. **`FUN_00a2fc30` (WMV) / `FUN_00a2f550` (AVI)** — graph build:
   - `IGraphBuilder::AddFilter` × N for: video source, video compressor,
     (if audio) async reader + wave parser + audio compressor, mux/writer.
   - **WMV:** load `Data\Video\WMVProfile_<quality>{_NA}.prx`
     (`FUN_00a2f770` builds the path, `FUN_00a2e980` loads the XML via
     `WMCreateProfileManager` → `CreateEmptyProfile(WMT_VER_7_0)` →
     `LoadProfileByData(xml, ...)`), then push profile into the writer
     (likely via `IConfigAsfWriter` QI'd from `obj_880` — the QI call is
     the unrecovered-args call at the top of `FUN_00a2fc30`).
   - **`FUN_00a2e4b0` + `FUN_00a2e610`** — pin connection helpers
     called repeatedly to wire `pin → pin` for each filter→filter link:
     V-source→V-comp→Mux/Writer, then Reader→Parser→A-comp→Mux/Writer.
4. **`obj_860->Run()`** (`IMediaControl::Run`, vtable slot 7) starts the
   pipeline.

**Profile naming.** Built by `FUN_00a2f770` from a quality enum:
`Data\Video\WMVProfile_{Upload|Small|Medium|High|Best}{_NA?}.prx`. The
`_NA` suffix is appended when object offset `0x820` is false (so
`_NA` = "no audio"). Quality is read via `FUN_00acd42c` (5 cases).

**Profile contents** (extracted from the `.pak` archives via Reshoot;
see *Data extraction* below). Standard Microsoft WMV-profile XML,
**UTF-16 LE with BOM**. Two `<streamconfig>` elements per profile (one
for `_NA` variants — video only). Resolutions / bitrates:

| Quality | Resolution | FPS | Video kbps (WMV3) | Audio kbps (WMA2) | Audio Hz |
| --- | --- | --- | --- | --- | --- |
| Upload | 384×216 | 20 | 114 | 20 | 22050 |
| Small | 256×144 | 20 | 300 | 48 | 44100 |
| Medium | 384×216 | 20 | 300 | 96 | 44100 |
| High | 512×288 | 20 | 300 | 96 | 44100 |
| Best | 768×432 | 30 | 8192 | 192 | 44100 |
| (_NA variants) | as above | as above | as above | — | — |
| Audio (standalone) | — | — | — | 96 | 44100 |

Codecs: video is **WMV3** (`bicompression="WMV3"`, subtype
`{33564D57-0000-0010-8000-00AA00389B71}` = `MEDIASUBTYPE_WMV3`); audio
is **WMA Standard** (`wFormatTag="353"` = `0x161`, subtype
`{00000161-0000-0010-8000-00AA00389B71}` = `MEDIASUBTYPE_WMAUDIO2`).
Profile version field is `589824` = `0x90000` = `WMT_VER_9_0`.

Sample profile structure (`wmvprofile_upload.prx`, decoded):

```xml
<profile version="589824" storageformat="1" name="MoviesOnline">
  <streamconfig majortype="{73647561-…}" streamnumber="1"
                streamname="Audio Stream" bitrate="20008" …>
    <wmmediatype subtype="{00000161-…}" …>
      <waveformatex wFormatTag="353" nChannels="2"
                    nSamplesPerSec="22050" nAvgBytesPerSec="2501"
                    nBlockAlign="813" wBitsPerSample="16"
                    codecdata="0044000017 00B50C0000"/>
    </wmmediatype>
  </streamconfig>
  <streamconfig majortype="{73646976-…}" streamnumber="2"
                streamname="Video Stream" bitrate="114000" …>
    <videomediaprops maxkeyframespacing="30000000" quality="40"/>
    <wmmediatype subtype="{33564D57-…}" …>
      <videoinfoheader dwbitrate="114000" avgtimeperframe="500000">
        <rcsource left="0" top="0" right="384" bottom="216"/>
        <rctarget left="0" top="0" right="384" bottom="216"/>
        <bitmapinfoheader biwidth="384" biheight="216"
                          bibitcount="24" bicompression="WMV3" …/>
      </videoinfoheader>
    </wmmediatype>
  </streamconfig>
</profile>
```

The XML in this exact form is what `IWMProfileManager::LoadProfileByData`
receives (the bytes the game reads from the `.prx` file in the PAK,
passed through directly).

**Implications for #3 and #2:**
- Parser only needs to recognise: `bicompression`, `biwidth`, `biheight`,
  `dwbitrate`, `avgtimeperframe`, `nSamplesPerSec`, `nChannels`,
  `nAvgBytesPerSec`, `wFormatTag`. The rest is decoration.
- All five preset videos use the **same codec pair** (WMV3 + WMA2).
  Encoder side (#2) only needs FFmpeg's `wmv2`/`wmv3` + `wmav2`
  encoders + ASF muxer. Lots of FFmpeg can be cut (#7).
- The XML BOM + UTF-16 encoding means the parser must handle wide
  strings — `wcsstr` + a tiny attribute-value extractor is enough,
  no full XML library needed.

### Data extraction (for project records)

The `.prx` profiles ship inside `Data\pak\*.pak` archives, not as loose
files on disk. Extracted using **Reshoot**, the community CLI tool from
[themovies3d.com](https://www.themovies3d.com/en/downloads/category/22-tools-a-utilities):

```bash
"/c/Program Files (x86)/The Movies Editor/reshoot.exe" -g -l "WMVProfile*"   # list
"/c/Program Files (x86)/The Movies Editor/reshoot.exe" -g -e "WMVProfile_*.prx"  # extract
```

Extracted samples for development reference are under
`mod/research/profiles/` (gitignored — game data).

**Identified CLSIDs / IIDs in the writer path:**
| Symbol | GUID |
| --- | --- |
| `CLSID_FilterGraph` | `E436EBB3-524F-11CE-9F53-0020AF0BA770` |
| `CLSID_CaptureGraphBuilder2` | `BF87B6E1-8C27-11D0-B3F0-00AA003761C5` |
| `CLSID_WMAsfWriter` | `7C23220E-55BB-11D3-8B16-00C04FB6BD3D` |
| `CLSID_AsyncReader` | `E436EBB5-524F-11CE-9F53-0020AF0BA770` |
| `CLSID_WaveParser` | `D51BD5A1-7548-11CF-A520-0080C77EF58A` |
| `IID_IGraphBuilder` | `56A868A9-0AD4-11CE-B03A-0020AF0BA770` |
| `IID_ICaptureGraphBuilder2` | `93E5A4E0-2D50-11D2-ABFA-00A0C9C6E38D` |
| `IID_IBaseFilter` | `56A86895-0AD4-11CE-B03A-0020AF0BA770` |
| `IID_IMediaControl` | `56A868B1-0AD4-11CE-B03A-0020AF0BA770` |
| `IID_IMediaEvent` | `56A868B6-0AD4-11CE-B03A-0020AF0BA770` |
| `IID_IFileSinkFilter` | `A2104830-7C70-11CF-8BCE-00AA00A3F1A6` |

**Implications for the shim (#2):**
- We already hook `CoCreateInstance(CLSID_FilterGraph)` → returns our
  `FakeGraph`. That covers the playback path. **For the writer path the
  game also creates `CLSID_CaptureGraphBuilder2` and `CLSID_WMAsfWriter`
  — neither is currently intercepted.**
- Either:
  - **(a)** intercept all three new CLSIDs and reimplement
    `ICaptureGraphBuilder2::SetFiltergraph/RenderStream/FindInterface`
    plus an `IFileSinkFilter` + `IConfigAsfWriter`-bearing writer
    filter that ultimately calls FFmpeg's ASF muxer; OR
  - **(b)** **simpler**: detect "writer setup in progress" (e.g. in
    `FakeGraph::AddFilter` when the added filter has CLSID
    `CLSID_WMAsfWriter` or is the AVI mux), tear down the placeholder
    graph, and replace the *whole* export with a direct FFmpeg encode
    that reads the rendered video frames from the existing
    TEXTURERENDERER source and the audio from the staged TMP WAV file.
    The game only inspects state via `IMediaControl::Run` /
    `IMediaEvent::GetEvent` and a duration polling loop — easy to fake.

The (b) path is significantly less surface area and avoids
re-implementing DirectShow's graph builder. Recommended for #2.

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
