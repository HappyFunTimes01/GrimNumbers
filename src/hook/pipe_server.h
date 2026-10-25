// Named-pipe event sink.
//
// Hooks run on the game's own threads, so Publish() must never block and must
// never touch the pipe. It copies the message into a fixed ring buffer and
// returns; a dedicated writer thread owns the pipe and drains the ring. If the
// ring is full (no client, or a slow one), the oldest events are dropped and
// the sequence numbers gap, which the client reports.
#pragma once

#include <stdint.h>
#include <stddef.h>

namespace grimnumbers {

bool PipeStart();
void PipeStop();

// Called on the writer thread each time a client connects, after the HELLO.
// Used to re-announce state a late-joining client would otherwise never see
// (the player identity, which is only published when it changes).
typedef void (*OnConnectFn)();
void PipeSetOnConnect(OnConnectFn cb);

// Clock used to stamp idle heartbeats. It MUST be the same clock that stamps
// damage events (the game's own GetGameTime), because the client measures its
// rolling window as a difference between the two. Defaults to GetTickCount.
typedef uint32_t (*TimeSourceFn)();
void PipeSetTimeSource(TimeSourceFn fn);

// Copies `size` bytes. Fills in hdr.magic/version/size/seq for the caller.
void Publish(void* msg, uint16_t type, uint32_t size);

// Number of events dropped because the ring was full.
uint64_t PipeDropped();

} // namespace grimnumbers
