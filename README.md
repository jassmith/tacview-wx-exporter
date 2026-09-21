# DKS Tacview Weather Exporter

Adds wind, QNH and sea-level temperature to the ACMI recordings a DCS
**dedicated server** produces.

## About this project

This wrapper was developed with the assistance of an AI coding agent under the
oversight of a professional software engineer. Every design decision, the
architecture, and the load-bearing details documented below were directed and
reviewed by a human; the AI did the mechanical work of turning that direction
into code and iterating on it.

I enjoy writing code, but this particular project is the kind that only comes
together through a long grind of measure-adjust-remeasure — patching an import
table, watching a byte count reconcile, chasing why a human trap never resolved,
and doing it again. I did not want to do all of that iteration by hand, so I
drove it with an AI agent and kept a close eye on the result. What is committed
here is code I have read and stand behind, not code I wrote keystroke by
keystroke.

Tacview's DCS recorder only exports weather for a local player's own aircraft,
which a headless server does not have — so host recordings arrive with position
data and nothing else. This exporter puts the sim's authoritative wind into the
recording itself, so every downstream consumer (DKS, Tacview desktop, Lardoon,
anyone who grabs the file) sees it without needing a side channel.

## How it works

```
 DCS server
   ├── Scripts/Hooks/DKSWeatherSampler.lua   samples atmosphere.getWind()
   │        └── writes ──► Mods/tech/Tacview/bin/dks-wx.profile
   └── Scripts/Hooks/TacviewGameGUI.lua      (Tacview's own, unmodified)
            └── require('tacview') ──► Mods/tech/Tacview/bin/tacview.dll  ← ours
                                              └── forwards to tacview_real.dll
```

The proxy exports the single symbol the real recorder exports
(`luaopen_tacview`) and forwards it, so Tacview records exactly as it always
did. It additionally patches the real module's import table for `CreateFileW`,
`WriteFile`, `SetFilePointer` and `DeleteFileW`.

While the mission runs, Tacview flushes the recording as plaintext in 16 KB
blocks. The proxy splits each block at its last line boundary and inserts
weather property lines there. Two details are load-bearing:

- **`SetFilePointer` must be shifted.** Tacview seeks to absolute offsets
  computed from its own byte count, which knows nothing about our insertions.
  Unshifted, its next block seeks back over what we wrote and overwrites it.
  Every `FILE_BEGIN` seek on the recording handle is therefore offset by the
  bytes we have inserted so far. (`FILE_CURRENT`/`FILE_END` need no shift —
  both are relative to state that already reflects our writes.)
- **Tacview's own archive never sees any of it.** The `.zip.acmi` is built from
  Tacview's in-memory model, not from the plaintext file; measured directly,
  injected lines present mid-file in the plaintext were absent from the
  archive, with no truncation. The plaintext is a crash-recovery journal,
  deleted at close.

So the proxy intercepts that deletion and writes the archive itself, from the
enriched journal, using zlib (`bin\zlib1.dll`, already loaded by DCS). The
result is a normal single-entry `.zip.acmi` that any Tacview, Lardoon or DKS
reads without knowing anything happened.

### Holding the tail

Events must land in the frame they actually happened in, and an event can only
be spliced into a frame that has not gone to disk yet. So the outgoing stream
is buffered and only frames older than a five-second window are released;
recent frames stay splice-able. `CloseHandle` is hooked to write the held tail
before Tacview closes the file, and any seek or truncate flushes first, so what
is on disk is never short.

Measured on the server, the pipeline is comfortably inside that window — the
sampler hands an event over 0.05-0.4 s after the engine fires it, which is
3-6 s *before* the stream even reaches the frame it belongs to:

```
queued  eventT=298.54 drainT=298.92 (sampler lag 0.38) streamT=293.56 (handoff -5.36)
placed  eventT=298.54 -> frame 298.55
```

Four consecutive kills placed at 272.20→272.20, 298.54→298.55, 441.89→441.92
and 229.17→229.23 — every one inside a single frame interval of its true time.

### Takeoff and landing, as observed on a live server

`RUNWAY_TAKEOFF`/`RUNWAY_TOUCH` (ids 54/55) are the precise pair — the older
`TAKEOFF`/`LAND` fire late by design ("several seconds after take-off"; only
once a landing aircraft "sufficiently slows down"). Both pairs are subscribed
anyway, because a carrier session showed the runway pair is not sufficient on
its own:

- `RUNWAY_TAKEOFF` fires from a carrier — observed with
  `place=USS Theodore Roosevelt`.
- `RUNWAY_TOUCH` **does** fire for human-flown aircraft, including carrier
  traps, and `LANDING_QUALITY_MARK` fires server-side carrying the full grade
  text (`LSO: GRADE:--- : WX _DRX_ … WIRE# 3 _EGIW_`).

Both were initially invisible for a different reason worth knowing: **DCS
events name the UNIT, while Tacview writes the PLAYER name in `Pilot=` for
human-flown aircraft**. AI resolve because for them the two strings are the
same; humans never matched, so every human trap, grade and ejection was
dropped as "never declared by Tacview". Events therefore carry a second
resolution key — the player name from `getPlayerName()` — and the injector
tries both.

The player-name key has a trap of its own, found the hard way (v1.1.0):
squadron player names routinely contain the pipe character (`Vy14 | 204 |
Hash`) — which is also the field delimiter of the mission→GUI event line.
Only the line's final field can be captured greedily, and there are two
player-name fields, so the initiator's name silently truncated at its first
pipe and every human-initiated event — every trap, LSO grade, takeoff and
shot on a squadron server — failed resolution and dropped. (Events where the
human was the *target* survived, because that name rides in the final greedy
field — which is what gave the bug away.) Names must round-trip exactly to
match what Tacview recorded, so they cannot be scrubbed like the free-text
`note`; instead pipes are transposed to `\1` across the state hop and
restored after the split. Unit names get the same treatment across that hop;
a *unit* name containing a pipe still cannot survive the events-file hop to
the injector (its fields split non-greedily), which is unchanged and has not
been observed in practice.

One departure can still arrive twice, because the classic pair and the
runway pair are both subscribed and overlap. Takeoff, landing and LSO events
collapse repeats of the same kind and subject within 20 s. Hits and shots are
exempt, since they legitimately repeat.

Events naming a unit Tacview never declared are dropped after 30 s and logged
(`DROPPED ... never declared by Tacview`). Tacview does not record every DCS
unit, so on a large mission a fraction of ground kills have no object to attach
to; the log makes that count visible rather than silent.

Injected lines use Tacview's own native property names on the objects Tacview
itself declared, plus one global of our own:

```
0,QNH=1013.25
0,Temperature=31.4
5901,WindDirection=129.7,WindPitch=0.0,WindSpeed=4.07
```

`Temperature` is the sea-level temperature in °C — the mission's ISA
departure. Tacview has no native property for it and ignores the line; it is
there because a recording's true airspeed can only be turned back into the
calibrated airspeed the pilot saw if the speed of sound is known, and that is
set by temperature. A Persian Gulf afternoon at ISA+25 reads about 4 % lower
on the gauge than a standard day for the same TAS, which is what every
consumer assumed before this line existed. It is sampled from
`atmosphere.getTemperatureAndPressure` at MSL (the atmosphere the sim is
applying), falling back to the mission table's season temperature.

Three properties of this design are load-bearing:

- **Splices land only at line boundaries**, so a line is never broken and the
  result is one clean sequential stream.
- **Weather is built at the moment it is written**, from current state, so every
  object it names is alive at that point. Weather built earlier and flushed
  later can name an object Tacview has since removed, which downstream parsers
  read as a brand new object. (Events are different: they are deliberately held
  and placed by timestamp, which is what the hold-back window is for.)
- **Property-only lines, never a synthetic moving object.** Protected
  recordings (`PlaybackDelay > 0`) encrypt `Lng/Lat/U/V` of dynamic objects with
  a stream cipher that advances per encrypted digit. Property lines carry none
  of those fields and cannot desync it; a synthetic object with a position
  would.

Wind is read from `atmosphere.getWind()` — the field the sim is actually
applying — not the mission's static weather table, so it stays correct under
dynamic weather and under mods that rewrite conditions at runtime.

## Building

```
build.bat
```

Needs VS 2022 Build Tools with the C++ workload; the script finds `vcvars64`
itself. Output is `build\tacview.dll`.

## Installing (dedicated server)

1. Install Tacview's DCS module on the server as normal.
2. In `Saved Games/<DCS>/Mods/tech/Tacview/bin/`:
   rename `tacview.dll` to `tacview_real.dll`, then copy our `tacview.dll` in
   beside it. Repeat for `bin-mt\` if the server runs the MT executable.
3. Copy `lua/DKSWeatherSampler.lua` to `Saved Games/<DCS>/Scripts/Hooks/`.

Tacview's own `TacviewGameGUI.lua` is left untouched — it loads the proxy
without knowing anything changed.

> **A Tacview update restores its own DLL and silently disables the exporter.**
> Re-run the install after updating Tacview. The recording is unaffected either
> way; it just stops gaining weather.

## Verifying

`test/harness.cpp` replays a real recording through the injector with
deliberately awkward buffer sizes — buffers that end mid-line, mid-number and
mid-property — so the stream logic is exercised outside DCS:

```
g++ -O2 -std=c++17 -o harness test/harness.cpp src/acmi_inject.cpp -Isrc
./harness in.txt.acmi out.txt.acmi
```

The check that matters is that everything *other* than weather is untouched:
parse both files and compare object count, duration, sample count and kills.
Against a 50 MB host recording the enriched file adds 14,670 wind lines (+1.6%)
and parses identically — 1,688 objects, 834,740 position samples, 64 kills.

## Verified on a live server

Against a dedicated server running a two-F-16 Caucasus mission with the wind
cranked to 20/30/40 m/s:

- Byte accounting reconciles exactly — Tacview wrote 101,674 bytes across 7
  blocks, we inserted 638, journal 102,312.
- The rebuilt archive passes `unzip -t`, and DKS parses it identically to
  Tacview's own output for every non-weather field.
- Fed through `computeTelemetry`, the recording's reported airspeed changes by
  up to **81 kt** (TAS 439.3 → 357.9 where the aircraft flew directly downwind),
  while ground speed, vertical speed and G-force change by exactly zero.
- `|ΔTAS|` never exceeds the wind magnitude at that sample — the physical bound.

One thing this exposed: at 500 m the sim's actual wind was 42.1 m/s where linear
interpolation of the mission table's 20 (ground) / 30 (2000 m) predicts ~22.
Sampling `atmosphere.getWind` is not a convenience over reading the `.miz` — the
`.miz` would have been wrong by nearly 2×.

## Layout

| Path | |
|---|---|
| `src/acmi_inject.{h,cpp}` | Stream logic and wind interpolation. No Windows dependencies, so it is testable natively. |
| `src/proxy.cpp` | DLL entry point, IAT patching, the `WriteFile` split. |
| `src/proxy.def` | Export list — one symbol, matching the real recorder. |
| `lua/DKSWeatherSampler.lua` | Wind sampler. Deliberately Lua: this is the half that talks to the sim and should be readable by anyone deciding whether to trust it. |
| `test/harness.cpp` | Offline replay harness. |
