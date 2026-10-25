# Grim Numbers — a DPS meter for Grim Dawn x64

**Grim Numbers** by HappyFunTimes, copyright (c) 2026 — written with Claude.

- derives its hooked-symbol set and its `CombatAttributeType` name table from
  **GDDPSMeter** by Lumak, copyright (c) 2023
- vendors **MinHook** by Tsuda Kageyu, copyright (c) 2009-2017, which itself
  contains the Hacker Disassembler Engine by Vyacheslav Patkov, copyright (c)
  2008-2009
- hooks **Grim Dawn** by Crate Entertainment, which is neither part of this
  work nor redistributed by it

The parts of this work that can be licensed are MIT. See `LICENSE_*.txt` for
licences, [What this is built on](#what-this-is-built-on) for the debts,
[Credits](#credits) for who did which work, and [Disclaimer](#disclaimer) for
what this repository does not own.

*So, your what hurts?*

A proxy `dinput8.dll` that hooks Grim Dawn's damage pipeline with MinHook and
streams damage events over a named pipe, plus a desktop app, a click-through
in-game overlay on **F9**, and a console client. **No injector** — the DLL is
loaded by the game's own import table.

The name is the long game: the game does a lot of arithmetic it never shows
you, and a DPS readout is only the first of it.

## Layout

```
include/grimnumbers/protocol.h   wire format, shared by both sides
src/hook/                  the proxy dinput8.dll
  dllmain.cpp              DllMain, real-DLL loading, deferred init
  gd_symbols.{h,cpp}       resolves game symbols by mangled name
  hooks.cpp                MinHook install + damage accounting
  pipe_server.cpp          ring buffer + named-pipe writer thread
src/client/              the out-of-process side, no game code
  stats.{h,cpp}          damage accounting; hands out a Snapshot under a lock
  pipe_client.{h,cpp}    reader thread: connect, reframe, reconnect
  fields.{h,cpp}         {token} values, number formatting, element labels
  graph.{h,cpp}          the per-actor damage-over-time plot
  layout.{h,cpp}         overlay.ini parser: panels, anchors, rows
  overlay.{h,cpp}        layered click-through window, Direct2D + DirectWrite
  appwindow.{h,cpp}      the desktop window, hotkeys, config reload
  app_main.cpp           grimnumbers_app wiring
  main.cpp               grimnumbers_client, the console readout
config/overlay.ini         overlay layout, copied next to the exe
config/elements.ini        damage-type id -> name, filled in by observation
config/skills.ini          skill name/colour overrides (names come from the game)
tools/gen_proxy_exports.py generates the full pass-through export table
tools/pipe_smoke.cpp       synthetic feeder, exercises the pipe without the game
docs/protocol.md           the pipe protocol, specified in full
                           (kept as public_docs/ in the working tree)
third_party/minhook/       vendored MinHook sources
reference/                 the two cloned repos (gitignored, not part of the build)
LICENSE_*.txt              this project's licence and those it builds on
make_upload.cmd            stages the publishable tree into upload\
```

## Build

Needs CMake and MSVC x64 (VS 2022 Build Tools is enough).

```bash
cmake -S . -B build -A x64 -DGD_X64_DIR="C:/Program Files (x86)/Steam/steamapps/common/Grim Dawn/x64"
cmake --build build --config Release
```

Produces `build/Release/dinput8.dll`, `grimnumbers_app.exe`,
`grimnumbers_client.exe` and `grimnumbers_pipe_smoke.exe`. The proxy links only
`KERNEL32.dll` (static CRT), so it drags no extra runtime into the game. The
app links `d2d1` and `dwrite`, both part of Windows — there is no third-party
UI or rendering library anywhere in the build.

## Install

Copy the proxy next to `Grim Dawn.exe` in the game's **x64** folder:

```bash
cmake --build build --config Release --target install_proxy
```

The loader prefers the application directory over `System32` for `dinput8.dll`
(it is not a `KnownDLL`), so the game picks up the proxy with no injector and
no launcher. To uninstall, delete `dinput8.dll` from that folder — nothing
else is touched, since a default install has no file by that name.

Then, with the game running:

```bash
build\Release\grimnumbers_app.exe
```

| Key | |
|---|---|
| **F9** | show / hide the in-game overlay |
| **F10** | re-read `overlay.ini`, `elements.ini` and `skills.ini` |

Both are global hotkeys, because Grim Dawn has focus when they are pressed.
`--window SECONDS` sets the rolling window (default 5) and `--overlay` starts
with the overlay already showing.

The console readout is still there, and is the tool for identifying
damage-type ids:

```bash
build\Release\grimnumbers_client.exe --window 5 --raw
```

Options: `--window SECONDS`, `--top N` (busiest targets, abilities and
actors; default 5), `--raw` (one line per event, with its damage-type split).

## The overlay

A topmost layered window drawn with Direct2D, positioned against the **game
window's** corners rather than the desktop's. It is `WS_EX_TRANSPARENT`, so
hit testing skips it entirely: every click, drag and hover goes to the game,
and `WS_EX_NOACTIVATE` keeps it from ever taking focus. There is nothing to
click and nothing to interrupt.

Because clicks pass through, the overlay cannot be dragged — its layout lives
in `overlay.ini` next to the exe. Panels, their corner, their rows and the
`{tokens}` in each row are all config, re-read on F10, so arranging the
display needs no rebuild. The file documents every token and panel key. Two
panels ship enabled (the DPS block, the element breakdown and the ability
breakdown, anchored lower right); a target list and an all-actor list ship
disabled, because both get long in a busy fight and the window is the better
place to read them.

One limitation: a layered window cannot composite over an
exclusive-fullscreen swapchain. Run Grim Dawn windowed or borderless. Drawing
inside the game instead would mean hooking its swapchain and running UI code
on its render thread, which is a far larger risk to the game process than this
is worth so far.

### The element breakdown

Every component of a hit carries the game's own `CombatAttributeType` value
over the wire, so the split is exact, and the overlay names them: fire, cold,
aether, bleeding, and so on.

The names are not recoverable from the binary — the enum has no
`EnumConverter` specialisation in the export table
(it has no `EnumConverter` specialisation). The built-in table
is GDDPSMeter's, which rendered per-type damage with it against a live game,
so it is observed rather than inferred — but observed against an older build.
If an id ever reads wrong, `elements.ini` overrides the built-in name and
colour without a rebuild, and `grimnumbers_client --raw` prints each hit's split
so an id can be confirmed against a skill of known type. An id in neither
place still shows as `type N`.

## How it works

The game ships its **full MSVC C++ export table** — 25,113 names in `Game.dll`
and 6,283 in `Engine.dll`, mangled, with complete signatures. So every address
is resolved with `GetProcAddress` on a mangled name; no signature scanning and
no hardcoded offsets. The analysis notes hold the details.

Damage is accumulated and committed like this:

| Hook | Role |
|---|---|
| `Character::AttackTarget` | opens an outgoing attack — attacker, defender, skill id |
| `CombatManager::ApplyDamage` | per-damage-type component; `attrType == 0` is a miss |
| `Player::RegisterCombatTextHit` | commit, **non-crit** |
| `Player::RegisterCombatTextCrit` | commit, **crit** |
| `CombatManager::TakeAttack` | brackets one **incoming** attack |
| `Player::UpdateSelf` | learns the local player |
| `Player::PostPetSpawn` | pet ids, so pet damage is attributed |
| `Character::CharacterIsDying` | death events |

Crits carry no boolean anywhere in the damage path — crit is signalled
*structurally*, by which of the two combat-text functions the game calls.

Incoming damage needs a different commit model, because that combat-text pair
is outgoing-only and nothing announces that an incoming hit is final. A normal
monster hit is aggregated across the `TakeAttack` scope and emitted once on
exit. DoT and tick damage reaches `ApplyDamage` with no enclosing attack at
all, so those components are emitted individually and flagged
`aggregated = 0`; the client counts them as *ticks*, not *hits*, so a burning
character doesn't inflate the hit count. Incoming events always report
`isCrit = 0` — the game gives no crit signal on that path. Outgoing and
incoming damage use separate rolling windows, so neither perturbs the other's
figure.

Hooks run on the game's own threads, so they never touch the pipe: an event is
copied into a fixed 16,384-slot ring and a dedicated writer thread owns the
pipe. If no client is attached the ring overwrites its oldest entries, the
sequence numbers gap, and the client reports the drop count rather than
silently under-reporting.

## Testing

`grimnumbers_pipe_smoke` drives the real pipe server with synthetic events, so the
wire format, the ring, the reconnect path and the client's DPS math can be
checked without launching the game:

```bash
build\Release\grimnumbers_pipe_smoke.exe --dps 20000 --dtps 6000 --seconds 12
build\Release\grimnumbers_client.exe --window 5 --top 3      # in another console
```

It emits known averages (20% of outgoing hits crit for double; incoming split
half as aggregated hits and half as DoT ticks), so the client's rolling DPS,
crit rate, biggest hit, damage-taken total and hit-vs-tick split are all
verifiable against arithmetic. The last run matched exactly: 22 hits × 1500 +
87 ticks × 375 = 65,625 taken, crit rate 20.0%, and outgoing DPS unchanged by
the incoming traffic.

### What has and has not been verified

**Verified in the live game.** The proxy loads into the real process, all
eight hooks install, and a play session produced real numbers: 394 hits,
1,229,107 total damage, rolling DPS 22k-32k, 28 crits (7.1%), biggest hit
23,317, three distinct skill ids, component counts up to 9, and no dropped
events. The game ran 10+ minutes with no warnings or errors in the log.

Every hook's runtime address matched its documented RVA exactly (`Game.dll`
base + `0x67460` for `AttackTarget`, and so on for all eight), which confirms
that resolving by mangled name holds up against the shipped module.

Also verified:

- All 60 of GDDPSMeter's hooked symbols resolve to x64 exports, and the 17
  symbols this project resolves were checked name-by-name and
  module-by-module against the export table.
- The proxy builds warning-free, is x64, and its export table matches the
  system `dinput8.dll` exactly — same names, same ordinals.
- Pipe, wire format, ring buffer, reconnect, late-join, DPS/crit maths and the
  damage-taken hit-vs-tick split, end-to-end through `grimnumbers_pipe_smoke`.
- Checked by disassembly rather than inference: that `Character::ApplyDamage`
  forwards to `CombatManager::ApplyDamage` (and that `CombatManager` sits at
  `Character+0x530`); that the shared `Character::RegisterCombatText*` address
  is a folded constant stub; that `GameEngine::RegisterDamage` is called *by*
  `ApplyDamage`; that `TakeAttack` does bracket a normal incoming hit while
  the DoT and tick paths do not; and that `AttackTarget` dispatches virtually
  on its target, so it always runs before the defender-side hooks.

**Still unverified:** damage *taken* has not been exercised against the live
game — the outgoing path is what the play session covered. Crit attribution
is believed right (the crit combat-text hook is the only signal the game
offers) but has not been cross-checked against the numbers the game draws on
screen. Skill ids are reported raw, so nothing confirms which id is which
skill.

Bugs the live run found that static analysis had not:

| Symptom | Cause |
|---|---|
| Game died in a 15s relaunch loop | proxy exported 3 winmm functions; `steamclient64.dll` needed others |
| Pipe closed right after HELLO | overlapped `WriteFile`'s byte count is undefined; must come from `GetOverlappedResult` |
| Client could not reconnect while idle | server only noticed a dead client when it wrote; fixed with a heartbeat |
| `bad magic` mid-stream | `SetNamedPipeHandleState` needs `FILE_WRITE_ATTRIBUTES`; without it the pipe silently stayed in byte mode and split messages |
| Every target named `records/creatures/enemie` | 51-char DBR paths truncated from the end into a 48-byte field, destroying the file stem |
| Teardown double-delete | `HooksInstall` calls `HooksRemove` on failure, and detach called it again |

The DLL logs to `%TEMP%\grimnumbers.log` by default; override with
`GRIMNUMBERS_LOGFILE`, or watch it in DebugView.

```bash
set GRIMNUMBERS_LOGFILE=C:\Temp\grimnumbers.log
```

## Known gaps

- **Crit is player-only.** The game signals a crit purely by which of
  `Player::RegisterCombatTextHit` / `...Crit` it calls, and neither fires for
  anyone else. So `isCrit = 0` on another actor's damage means "unknown", not
  "not a crit", and no crit rate can be shown for anything but the player.
- **Skill and actor ids are per-session.** Both are Object ids, assigned in
  creation order at load, so nothing may persist one across runs — see
  "Naming skills" in the unpublished notes. Names are resolved at runtime and
  sent over the wire for exactly this reason.
- **Pet damage is not attributed to a named skill.** Pets own their own
  `SkillManager` and the name sweep only walks the player's, so a pet's
  abilities show as raw ids.
- **Damage taken by pets is not counted as incoming** — only the player's own,
  so a dying pet does not inflate a damage-taken readout. It does appear in
  the all-actor table, which is the place for it.
- **The event stream is unfiltered.** Every actor's damage is published and
  the client decides what to show. That is a deliberate starting point rather
  than a finished design: a game-side filter would cut the volume, but the
  cost is modest enough that measuring came first.
- **The actor list sorts only when the cast changes.** Rows are ordered by
  damage dealt when an actor first appears, and then left alone. Re-sorting
  continuously would reorder rows under the pointer four times a second and
  make a row hard to click; the cost is that the order goes mildly stale
  during a long fight.
- **The graph has no CSV export and no point selection yet.** It plots dealt
  and taken per second for the selected actor over the last three minutes;
  picking a point to see the ability and element split behind it, and writing
  the series out, are still to come.

## What this is built on

**[GDDPSMeter](https://github.com/Lumak/GDDPSMeter) by Lumak (MIT, 2023) is
the prior art this project stands on**, and the debt is larger than a link.
Lumak did the original hard part: working out *which* of Grim Dawn's tens of
thousands of functions carry the combat numbers, and that the game can be
read by resolving its exported mangled C++ names rather than by scanning for
byte signatures. Both of those arrived here already solved.

Taken from GDDPSMeter, directly:

- the set of 60 hooked symbols, recorded as x86→x64 pairs in the
  unpublished notes;
- the `CombatAttributeType` name table in
  [`src/client/fields.cpp`](src/client/fields.cpp), transcribed from its
  `DetailDamage.cpp` and shortened for display — the enum has no
  `EnumConverter` in the export table, so these names are not recoverable
  from the binary and would otherwise have had to be rediscovered by
  observation;
- the damage-accounting shape: that an attack opens at `AttackTarget`,
  accumulates per type through `ApplyDamage`, and is committed by the
  combat-text call.

Not taken: no GDDPSMeter source file is compiled into this build or
redistributed by this repository. The clone under `reference/` is
`.gitignore`d. What is different here is the x64 port of all 60 manglings,
the full 31,396-row demangled index, the import-table proxy in place of
GDDPSMeter's `CreateRemoteThread` loader, MinHook in place of Detours, a
named pipe and an out-of-process client in place of in-process imgui, and the
incoming/DoT accounting, which GDDPSMeter does not do.

**[MinHook](https://github.com/TsudaKageyu/minhook) by Tsuda Kageyu**
(BSD-2-Clause) is vendored under `third_party/minhook/` and compiled in. It
contains the Hacker Disassembler Engine by Vyacheslav Patkov.

**Grim Dawn** is by [Crate Entertainment](https://www.crateentertainment.com/).
Nothing of theirs is in this repository except observations about their
binaries.

Tools that did real work: `dumpbin -exports` and `undname`, both from the
MSVC toolchain, built the symbol index; the structural claims in the
analysis notes were checked against raw disassembly of the shipped modules
rather than inferred; [DebugView] reads the hook log
live as an alternative to the log file.

[DebugView]: https://learn.microsoft.com/en-us/sysinternals/downloads/debugview

## Credits

Plainly, because the division of labour here is unusual enough to be worth
stating rather than leaving to be guessed at.

**HappyFunTimes** ([@HappyFunTimes01](https://github.com/HappyFunTimes01)) —
owner and director of the work. Set the goal and the scope, supplied the
target and the environment, made the judgement calls as they came up, and
licensed the result.

Every run against the live game was theirs. Claude cannot
launch Grim Dawn and play it. The ten-minute session recorded under
[Testing](#what-has-and-has-not-been-verified) — 394 hits, 1,229,107 damage,
no dropped events — was a person playing the game with the proxy loaded, and
it is the only reason this is a working tool rather than a plausible one. It
also produced all six of the bugs in that table, none of which static
analysis had found: the relaunch loop, the overlapped-write byte count, the
missing heartbeat, the `FILE_WRITE_ATTRIBUTES` pipe-mode failure, the
truncated DBR paths, and the teardown double-delete.

**Claude** (Anthropic — Claude Opus 5, via Claude Code) — wrote the code and
the prose. The C++ under `src/`, `include/` and `tools/`; the analysis
notes, including the x86→x64 mangling port and the disassembly checks; the
CMake build and the generated proxy export table; this README; and the
GrimMath→GrimNumbers rename.

## Disclaimer

Grim Dawn is the property of Crate Entertainment. This project is not
affiliated with, endorsed by, or supported by Crate Entertainment, and it
ships none of their code or assets.

The analysis notes kept alongside this project — the symbol index, the hook
mapping and the findings — are an *index of observations* about shipped
binaries: symbol names, RVAs, class offsets and call relationships read out
of `Game.dll` and `Engine.dll` with `dumpbin`, `undname` and a disassembler.
They are not published in this repository. Those facts describe Crate's
software; recording them here claims no ownership of it, and the MIT licence
on this repository does not extend to them.

This is single-player tooling. It reads combat numbers the game already shows
the player, changes no game behaviour, and writes nothing into the game's
memory beyond MinHook's trampolines. Multiplayer use is out of scope and
untested. Hooking a running game is inherently at your own risk; a crash
costs you a Grim Dawn session, and that is on you, not on Crate.

## Licences

The parts of this work that can be licensed are **MIT**.

| File | Covers |
|---|---|
| [`LICENSE_GrimNumbers.txt`](LICENSE_GrimNumbers.txt) | this project — MIT, HappyFunTimes, 2026 |
| [`LICENSE_GDDPSMeter.txt`](LICENSE_GDDPSMeter.txt) | GDDPSMeter — MIT, Lumak, 2023; the symbol set and the element-name table derive from it |
| [`LICENSE_MinHook.txt`](LICENSE_MinHook.txt) | MinHook — BSD-2-Clause, Tsuda Kageyu; also at `third_party/minhook/LICENSE.txt`, where it travels with the vendored source |
