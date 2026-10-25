// RADIO DAYS SDR: the rules for feeding the speaker from the receive buffer.  Pure C++, tested on a PC.
//
//   not playing: wait until PREBUF samples are buffered, then play (absorbs Wi-Fi jitter).
//   playing:     whenever the speaker can take another chunk, give it CHUNK samples.
//                If the buffer cannot supply a chunk AND the speaker has nothing left to play, that is an
//                UNDERRUN: stop and re-buffer (no stutter of half-filled chunks).
//   network side: a frame that would push the buffer above HIGH is dropped and counted as an OVERRUN
//                (keeps the delay bounded when the sender's clock runs faster than ours).
#pragma once
#include <stdint.h>
#include <stddef.h>

namespace rdaudio {

struct PlayoutParams {
  uint32_t chunk = 512;       // samples handed to the speaker at once (43 ms at 12 kHz)
  uint32_t prebuf = 3000;     // ~250 ms
  uint32_t high = 9000;       // ~750 ms
  uint32_t capacity = 12000;  // ~1 s (the ring buffer)
};

class Playout {
 public:
  PlayoutParams p;
  bool playing = false;
  uint32_t underruns = 0, overruns = 0, rebuffers = 0;

  void reset() { playing = false; }

  // Network side.  fill = samples now in the ring.  true = store the frame.
  bool admit(uint32_t fill, uint32_t n) {
    if (fill + n > p.capacity || fill > p.high) { overruns++; return false; }
    return true;
  }

  // Speaker side, called often.  slotFree: the speaker queue can take a chunk.  speakerIdle: it has nothing queued.
  // Returns the number of samples to move into a speaker buffer now (0 or chunk).
  uint32_t onFeed(uint32_t fill, bool slotFree, bool speakerIdle) {
    if (!playing) {
      if (fill >= p.prebuf) playing = true;
      else return 0;
    }
    if (!slotFree) return 0;
    if (fill >= p.chunk) return p.chunk;
    if (speakerIdle) {  // starved and silent: re-buffer
      playing = false;
      underruns++;
      rebuffers++;
    }
    return 0;
  }
};

}  // namespace rdaudio
