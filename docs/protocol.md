# The Grim Numbers pipe protocol

Everything the hook DLL knows reaches the outside world through one named
pipe. This document specifies that pipe completely enough to write another
client against it — in any language — without reading the C++.

The authoritative definition is
[`include/grimnumbers/protocol.h`](../include/grimnumbers/protocol.h); this
document describes the same thing in prose, with the offsets spelled out.

> The analysis notes on Grim Dawn's internals that the hook side is built from
> are not published in this repository yet. Nothing here depends on them: the
> protocol is the boundary, and it is self-contained.

## Transport

| | |
|---|---|
| Name | `\\.\pipe\GrimNumbers` |
| Direction | **server → client only**; the client never writes |
| Server | the hook DLL, inside the game process |
| Mode | message mode (`PIPE_TYPE_MESSAGE`) |
| Instances | 1 — a single client at a time |
| Server outbound buffer | 64 KiB |
| Byte order | little-endian |
| Struct packing | 4 bytes (`#pragma pack(4)`) |

The server creates the pipe with `PIPE_ACCESS_OUTBOUND | FILE_FLAG_OVERLAPPED`
and waits. It is created when the DLL initialises and lives for the life of
the game process, so a client may connect, disconnect and reconnect freely.

### Connecting

```c
HANDLE h = CreateFileW(L"\\\\.\\pipe\\GrimNumbers",
                       GENERIC_READ | FILE_WRITE_ATTRIBUTES,
                       0, NULL, OPEN_EXISTING, 0, NULL);

DWORD mode = PIPE_READMODE_MESSAGE;
SetNamedPipeHandleState(h, &mode, NULL, NULL);
```

`FILE_WRITE_ATTRIBUTES` is **required** even though the client only reads.
Without it `SetNamedPipeHandleState` fails, the handle silently stays in byte
mode, and reads then split messages at arbitrary boundaries — which surfaces
later as a bogus magic number rather than as an error at connect time.

`ERROR_FILE_NOT_FOUND` means the game is not running, or the DLL is not
loaded. `ERROR_PIPE_BUSY` means another client already holds the single
instance. Both are ordinary; retry.

## Framing

Every message begins with a 16-byte header whose `size` field covers the
header and the body together. Although the pipe is in message mode, **one
read can return several whole records back to back**: the server batches
whatever the ring holds into a single write, up to 60 KiB. A reader must walk
the buffer by `size` rather than assuming one record per read:

```
off = 0
while off + 16 <= bytes_read:
    size = u32 at off+8
    if size < 16 or off + size > bytes_read:  # malformed; drop the connection
        fail
    dispatch(buffer[off : off+size])
    off += size
```

Records are never split across reads. A 64 KiB read buffer is enough for any
batch the server will send.

## Header

`GmHeader`, 16 bytes, at the start of every message.

| Offset | Size | Field | Notes |
|---|---|---|---|
| 0 | 4 | `magic` | `0x4D554E47` — `'GNUM'` in memory order |
| 4 | 2 | `version` | currently **4** |
| 6 | 2 | `type` | message type, below |
| 8 | 4 | `size` | total bytes, header included |
| 12 | 4 | `seq` | monotonic per connection |

A client must check `magic` and `version` on **every** record and drop the
connection if either is wrong. There is no negotiation: the two sides are
built together, and a version mismatch means one of them is stale.

### Message types

| Value | Name | Struct | Size |
|---|---|---|---|
| 1 | `HELLO` | `GmHello` | 56 |
| 2 | `DAMAGE` | `GmDamage` | 156 |
| 3 | `DEATH` | `GmDeath` | 72 |
| 4 | `PLAYER` | `GmPlayer` | 72 |
| 5 | `HEARTBEAT` | `GmHeartbeat` | 48 |

A client should tolerate a record whose `size` is larger than it expects —
read the prefix it understands and skip the rest by `size`. That is the only
forward-compatibility the protocol offers.

## Sequence numbers and dropped events

`seq` increments by one for every message that passes through the server's
ring buffer. The ring holds 16,384 slots. If it fills — no client attached, or
a client too slow to drain it — the **oldest** entries are overwritten, which
shows up at the client as a jump in `seq`.

```
if expected and seq > expected:
    dropped += seq - expected
expected = seq + 1
```

**Heartbeats are the exception**: they are built on the writer thread and
bypass the ring, so they carry a *repeat* of the last used sequence number
rather than a fresh one. Counting them would fabricate gaps. Skip
`HEARTBEAT` when tracking sequence continuity.

Sequence numbers restart at each connection, so reset the expectation on
connect.

## Timebase

Every message carries `gameTimeMs`, the game's own clock, not wall time. All
rate calculations should use differences between these values. The clock is
shared by damage events and heartbeats precisely so that a rolling window can
be measured across both — an idle period is still time passing, and a DPS
figure that only advances when a hit lands never decays.

## Messages

### HELLO — type 1, 56 bytes

Sent once, immediately on connect, before anything else.

| Offset | Size | Field |
|---|---|---|
| 0 | 16 | `hdr` |
| 16 | 4 | `pid` — the game process id |
| 20 | 4 | `gameTimeMs` — clock at connect |
| 24 | 32 | `gameBuild` — `Game.dll` timestamp, NUL-padded |

`gameBuild` is a sanity check: a client can warn when it is talking to a build
it has never been exercised against.

A `PLAYER` message normally follows immediately, replaying identity the client
would otherwise not learn until it next changed.

### DAMAGE — type 2, 156 bytes

One committed hit. *Committed* means the game finished folding together the
per-damage-type components and drew combat text for it, so `totalDamage` is
the number the player actually saw.

| Offset | Size | Field | Notes |
|---|---|---|---|
| 0 | 16 | `hdr` | |
| 16 | 4 | `gameTimeMs` | |
| 20 | 4 | `attackerId` | game object id |
| 24 | 4 | `defenderId` | game object id |
| 28 | 4 | `skillId` | 0 when unattributed |
| 32 | 4 | `totalDamage` | `float`; sum over components |
| 36 | 1 | `source` | see below |
| 37 | 1 | `isCrit` | 1 = the crit path fired |
| 38 | 1 | `componentCount` | components folded into this hit |
| 39 | 1 | `aggregated` | 1 = one visible hit, 0 = a lone tick |
| 40 | 48 | `otherName` | NUL-padded, may be empty |
| 88 | 1 | `elemCount` | entries used in `elems` |
| 89 | 1 | `elemDropped` | distinct types past the 8 that fit |
| 90 | 2 | — | padding |
| 92 | 64 | `elems[8]` | 8 × `GmElem` |

`GmElem` is 8 bytes: `type` (`uint32`) then `amount` (`float`).

**`source`** — 0 unknown, 1 player, 2 pet, 3 incoming (damage taken by the
player). Outgoing damage, the kind DPS is computed from, is sources 1 and 2.

**`otherName`** is whichever party is not the player: the victim for outgoing
damage, the attacker for incoming.

**`aggregated`** distinguishes a hit from a tick. A normal hit is accumulated
across the game's attack scope and emitted once, with `aggregated = 1`.
Damage-over-time and tick damage arrives with no enclosing attack, so each
component is emitted on its own with `aggregated = 0`. Counting those as hits
would make a burning character look like it was being hit several times a
second, so a client should count them separately.

Incoming damage always reports `isCrit = 0`; the game offers no crit signal on
that path.

**`elems`** is the per-damage-type split of `totalDamage`. Components sharing
a type are summed into one entry. `type` is the game's own raw damage-type id
— deliberately *not* translated to a name on the wire, because the ids are not
recoverable from the game binary and the mapping is observational. A client
should render an unknown id as something neutral rather than guess. If a hit
carries more than 8 distinct types, the overflow is counted in `elemDropped`
and remains included in `totalDamage`, so the split may be short of the total
but never exceeds it.

### DEATH — type 3, 72 bytes

| Offset | Size | Field |
|---|---|---|
| 0 | 16 | `hdr` |
| 16 | 4 | `gameTimeMs` |
| 20 | 4 | `characterId` |
| 24 | 48 | `name` — NUL-padded |

### PLAYER — type 4, 72 bytes

Sent when the local player becomes known or changes, and replayed on connect.

| Offset | Size | Field |
|---|---|---|
| 0 | 16 | `hdr` |
| 16 | 4 | `gameTimeMs` |
| 20 | 4 | `playerId` |
| 24 | 48 | `name` — NUL-padded, may be empty |

### HEARTBEAT — type 5, 48 bytes

Sent on a 250 ms timer — about four a second — whether or not the game is
idle.

| Offset | Size | Field |
|---|---|---|
| 0 | 16 | `hdr` |
| 16 | 4 | `gameTimeMs` |
| 20 | 4 | — padding |
| 24 | 8 | `dropped` — `uint64`, since this client connected |
| 32 | 8 | `dropped5s` — in the last 5 seconds |
| 40 | 8 | `dropped60s` — in the last 60 seconds |

It exists for three reasons:

- the server only discovers a client has gone away when a write fails, so
  without traffic a departed client would hold the single pipe instance
  indefinitely;
- a client that redraws only on arrival would freeze its rolling figures at
  the last value instead of letting them decay through an idle stretch;
- the windowed drop counts say whether the link is losing events *now*. A
  session total is close to useless on its own, because one burst while
  nothing was reading taints every reading after it.

All three counters are zeroed when a client connects.

## Disconnecting

The client closes its handle; the server's next write fails and it loops back
to waiting for a new connection. On the client, `ERROR_BROKEN_PIPE`,
`ERROR_PIPE_NOT_CONNECTED` and `ERROR_NO_DATA` from `ReadFile` all mean the
game went away, and are normal at game exit.

Because the pipe allows a single instance, a client that exits without closing
cleanly blocks the next one until the server notices — which the heartbeat
guarantees happens within a few hundred milliseconds.

## Writing a client

`tools/pipe_smoke.cpp` drives the real server with synthetic events and no
game, which makes it the fastest way to develop a client:

```bash
build\Release\grimnumbers_pipe_smoke.exe --dps 20000 --dtps 6000 --seconds 30
```

It emits known averages, so a new client's arithmetic can be checked against
a figure you can compute by hand.
