# The Movies — Life Support for Modern Operating Systems

> **Part of the KPC Classics Conservation Program.** See
> [Conservation Program & AI Disclosure](#conservation-program--ai-disclosure)
> at the bottom of this README.

A drop-in fix that lets *The Movies* (Lionhead Studios, 2005) run on modern
Windows and on Linux/Wine by replacing every Windows Media Foundation
dependency with **FFmpeg** + **libmpv**. Distributed as an ASI mod loaded
through Ultimate ASI Loader (`dinput8.dll`).

## Why

The Movies was built against Windows Media Foundation circa 2005. Modern
Windows is in the process of removing WMF, Wine never had a complete
WMF implementation, and the in-game movie studio plus all FMVs depend on
it. This mod intercepts WMF / DirectShow at the entry points the game uses
and answers them with a self-contained FFmpeg + libmpv stack.

## Verified scope: launch screens through main menu only

> **Important.** Everything below has been verified up to and including
> the main menu. **Actual in-game play (starting a studio, running the
> simulation, the campaign) has not been tested yet.** The mod replaces
> the WMF dependencies the game touches at launch and on the menu —
> whether further DirectShow / WMF code paths fire during gameplay is
> unknown. Tracked as an open issue.

## What works (verified)

- **All FMV cutscenes** that play before/around the menu: Activision logo,
  frontend intro, frontend background loop. Clean lip-sync (verified
  against the intro's "3-2-1 beep" countdown).
- **Audio**: routed through libmpv → WASAPI. No drift, no underruns.
- **Looping**: only files whose name contains `loop` (i.e. the menu
  background) loop. Intros end naturally and the game progresses.
- **Volume**: the game's `IBasicAudio::put_Volume` is forwarded into
  libmpv's volume property.
- **No `quartz.dll`**: the entire DirectShow filter graph is replaced
  in-process; the game never touches the system DirectShow.
- **Movie export**: the in-game studio's WMF writer path is intercepted
  end-to-end. `CLSID_WMAsfWriter` is replaced by a custom in-process
  filter (`src/asf_writer.c`) that implements `IBaseFilter`,
  `IFileSinkFilter`, `IConfigAsfWriter`, and an input pin, backed by
  **libavformat (ASF muxer)** + **libavcodec (WMV2)**. The game's
  rendered frames go through a hand-rolled BGR→YUV420P converter
  (BT.601 limited range, 2x2 chroma averaging) — `sws_scale` was
  producing garbage in our setup so we bypassed it. Output is a clean
  `.wmv` that VLC and other players read correctly. **No qasf.dll /
  wmvcore.dll / qcap.dll dependency.**

## Known limitations / TODO

- **In-game gameplay untested.** Only menu-level paths are confirmed
  ([#9](https://github.com/NightHammer1000/lifesupport-TheMovies/issues/9)).
- **Movie export improvements** ([#10](https://github.com/NightHammer1000/lifesupport-TheMovies/issues/10),
  [#11](https://github.com/NightHammer1000/lifesupport-TheMovies/issues/11),
  [#12](https://github.com/NightHammer1000/lifesupport-TheMovies/issues/12)):
  switch to MP4+H.264 for universal player compatibility, drive
  encoder bitrate from the captured `IWMProfile`, add audio stream.
- **Widescreen** rendering ([#6](https://github.com/NightHammer1000/lifesupport-TheMovies/issues/6)).
  Original ask, not started.

## Install

You need three files in the game directory next to `MoviesSE.exe`:

| File | Where it comes from |
| --- | --- |
| `dinput8.dll` | [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader) (32-bit build) |
| `movies_fix.asi` | Built from this repo (see below) |
| `libmpv-2.dll` | [shinchiro mpv-winbuild](https://sourceforge.net/projects/mpv-player-windows/files/libmpv/) — i686 dev archive |

Drop them next to `MoviesSE.exe` and start the game.

## Build

Tested on Windows with MSYS2.

Prerequisites:

1. **MSYS2 mingw32 toolchain** at `C:\msys64\mingw32` (provides
   `i686-w64-mingw32-gcc` etc.).
2. **Static FFmpeg 7.1** built into `C:\tmp\ffbuild\install` (only the
   in-game movie-studio reader path uses FFmpeg — playback is libmpv).
   You need `libavformat.a`, `libavcodec.a`, `libswscale.a`,
   `libswresample.a`, `libavutil.a` and the matching headers.
3. **libmpv i686 dev archive** extracted into `mod/lib/libmpv/`. After
   extraction the layout should be:
   ```
   mod/lib/libmpv/libmpv-2.dll
   mod/lib/libmpv/libmpv.dll.a
   mod/lib/libmpv/include/mpv/{client,render,render_gl,stream_cb}.h
   ```

Then:

```bash
cd mod
make            # produces build/movies_fix.asi
make deploy     # copies ASI + libmpv-2.dll into the game directory
make info       # show what DLLs the built ASI imports
make clean
```

(`build.sh` and `deploy.sh` are equivalent thin wrappers if you'd rather not use `make`.)

## Architecture

```
                     ┌──────────────────────────┐
   game ───────────► │ Hooked CoCreateInstance  │
                     │  (src/main.c, MinHook)   │
                     └────┬─────────────────────┘
                          │ FilterGraph → FakeGraph
                          │ WMAsfReader → DSSourceFilter
                          ▼
       ┌──────────────────────────────────────────┐
       │ FakeGraph (src/ds_fakegraph.c)           │
       │ IGraphBuilder / IMediaControl /          │
       │ IMediaSeeking / IMediaPosition /         │
       │ IBasicAudio / IMediaEvent — replaces     │
       │ quartz.dll entirely                      │
       └─┬────────────────────────────────────────┘
         │ AddFilter / Render / Run …
         ▼
       ┌────────────────────┐    ┌──────────────────────┐
       │ DSSourceFilter     │    │ TEXTURERENDERER      │
       │ (src/ds_filter.c)  ├───►│ (game-supplied D3D9  │
       │ libmpv backend     │    │  video sink)         │
       │ (src/mpv_player.c) │    │                      │
       └────────────────────┘    └──────────────────────┘
                │
                └─► WASAPI (libmpv internal audio output)
```

For the **in-game movie studio** path the game also calls
`WMCreateSyncReader` and `WMCreateProfileManager` directly. Those go to
`src/sync_reader.c` and `src/profile_mgr.c` — both FFmpeg-backed. Playback
of the studio assets happens through the same `DSSourceFilter` path as
FMVs, so once a file is built it just plays.

## Source layout

| File | Purpose |
| --- | --- |
| `src/main.c` | ASI entry, MinHook installation, vectored exception logger. |
| `src/log.c/h` | Logger to `movies_fix.log`. |
| `src/ds_types.h`, `src/wm_types.h` | DirectShow / WMF IIDs and `__stdcall` vtables. |
| `src/ds_fakegraph.c/h` | The `quartz.dll` replacement. |
| `src/ds_filter.c/h` | `DSSourceFilter`: COM source filter that wraps libmpv. |
| `src/ds_output_pin.c/h` | The output pin handed to TEXTURERENDERER. BGR24 only. |
| `src/mpv_player.c/h` | Thin C wrapper around libmpv (sw render → BGR24). |
| `src/sync_reader.c/h` | `IWMSyncReader` for the studio path (FFmpeg). |
| `src/profile_mgr.c/h` | `IWMProfileManager` stubs. |
| `src/media_props.c/h` | `IWMOutputMediaProps` for the sync reader. |
| `src/nss_buffer.c/h` | `INSSBuffer` sample carrier. |
| `lib/minhook` | Vendored MinHook. |
| `lib/libmpv` | (Not checked in.) Drop the libmpv dev archive here. |

For the gory reverse-engineering details — Ghidra addresses, the
`IMediaSeeking↔IMediaPosition` swap and the other gotchas — see
[`STATUS.md`](STATUS.md).

## Conservation Program & AI Disclosure

This project is the first entry in the **KPC Classics Conservation Program** —
an effort to keep old and abandonware games from becoming lost media by
patching them back into a runnable state on modern systems.

**The technique.** We pair AI (Claude) with a human driver and reverse-engineering
tooling exposed through MCP — most importantly **Ghidra** (for the static side:
finding vtables, decoding object layouts, identifying the exact COM methods a
game calls) and **Cheat Engine** (for the dynamic side: live memory inspection,
catching the moments where the static analysis missed something). The AI does
the bulk of the code production and a lot of the trace-reading; the human does
the judgement calls, the testing, and pushes back when the AI is about to
solve the wrong problem. Neither half can produce a fix like this on its own
yet — together they can, and at a pace that makes preservation work tractable
for one or two people instead of a studio.

This README, the source comments, the build scripts, and most of the patch
code itself were written collaboratively under that workflow. We disclose it
on the tin so anyone building on the work knows what they're inheriting.

**On using AI for this.** We know AI use is a hot topic in modding and
preservation circles and we don't dismiss the concerns. Our position is
that this particular workflow puts working tools in the hands of people
who otherwise had no realistic path to fix a game themselves — single
contributors who can't drop years into mastering DirectShow, COM,
assembly-level patching, and a specific game's quirks all at once. AI
lowers that bar far enough for one motivated person to rescue a title
that would otherwise stay broken.

The supply of greybeard wizards who can fluently read x86 in Ghidra and
chase pointers through a debugger is *finite*, and it's unreasonable to
expect the handful of them we have to spend their evenings rescuing
forgotten games. AI cannot replace them — but it can absolutely stand
in for them on the 80% of the work that's pattern-matching, vtable
plumbing, and protocol bookkeeping, and free a single human up to do
the parts that actually need a brain. If AI is going to be everywhere
anyway, let it do something useful for once. We'd rather see
preservation happen than see purity tests prevent it.

A walkthrough of the exact setup — Claude Code, the Ghidra MCP, the
Cheat Engine MCP, how to brief an AI on a reverse-engineering job
without it making things up — will live at
**[kpc.bz/preservation-for-the-everyman](https://kpc.bz/preservation-for-the-everyman)**
once it's published. The page will also ship a **mod project
boilerplate** so you don't have to reassemble the workbench from
scratch every time: pre-written agent definitions, project rules,
custom skills, and an MCP config wired up to Ghidra and Cheat Engine
out of the box. Clone, point it at the next abandoned game, get to
work.

**Why bother.** Games are slipping into the void faster than any institution
is preserving them. Studios close, publishers shelve titles, online checks
brick offline-capable software, modern Windows drops APIs the games were built
on. The EU's [*Stop Killing Games*](https://www.stopkillinggames.com/) initiative
has started moving the needle on live-service shutdowns, but everything outside
that scope — the single-player back catalogue, the abandonware, the
"technically still for sale but doesn't run anywhere" — sits in a legal grey
zone where modding is the only thing keeping these works alive.

We think that should be a *right*, not a grey-zone tolerated practice. Call it
**Right to Resurrect**: when the rights holder won't or can't keep a work
running, the public should have an unambiguous, legally clear path to do it
themselves. Until that's law, we do it anyway, in the open, and document the
techniques so the next person has an easier time.

If this work helps you preserve something else, that's the point. Use it,
fork it, copy the workflow.

## License

Mod code: see repo root.
Bundled `libmpv-2.dll` and FFmpeg are LGPL/GPL, redistributed unchanged.
