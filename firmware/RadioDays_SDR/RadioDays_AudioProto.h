// RADIO DAYS SDR: audio stream from the Mac to the Cardputer speaker.  Pure C++ (no Arduino), tested on a PC.
//
//   TCP, port 8767.  client -> "RDSDR-AUDIO 1 <token>\n", server -> "OK\n" (or "BUSY ...\n"), then frames:
//   'R' 'D' | u16 nsamples | u32 seq | u16 rate_hz | nsamples * int16     (all little endian, 16-bit mono PCM, unscaled)
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>

namespace rdaudio {

static constexpr size_t HDR_SIZE = 10;
static constexpr size_t MAX_SAMPLES = 1024;  // the Mac sends frames of up to 1024 samples (it splits the KiwiSDR's 2048-sample blocks); anything larger is an error

struct FrameHeader {
  uint16_t nsamples;
  uint32_t seq;
  uint16_t rate;
};

inline bool parseHeader(const uint8_t* b, FrameHeader& h) {
  if (b[0] != 'R' || b[1] != 'D') return false;
  h.nsamples = (uint16_t)(b[2] | (b[3] << 8));
  h.seq = (uint32_t)b[4] | ((uint32_t)b[5] << 8) | ((uint32_t)b[6] << 16) | ((uint32_t)b[7] << 24);
  h.rate = (uint16_t)(b[8] | (b[9] << 8));
  return h.nsamples > 0 && h.nsamples <= MAX_SAMPLES && h.rate >= 4000 && h.rate <= 48000;
}

// Feed it the bytes as they arrive from the socket, in chunks of any size (TCP gives no frame boundaries).
// onFrame(header, const int16_t* pcm) is called once per complete frame.  feed() returns false on a protocol error
// (wrong magic / absurd header): the caller must close the connection and reconnect.
class FrameAssembler {
 public:
  void reset() { have_ = 0; need_ = HDR_SIZE; inPayload_ = false; }
  FrameAssembler() { reset(); }

  template <class F>
  bool feed(const uint8_t* d, size_t n, F&& onFrame) {
    while (n) {
      size_t take = need_ - have_;
      if (take > n) take = n;
      memcpy(buf_ + have_, d, take);
      have_ += take; d += take; n -= take;
      if (have_ < need_) break;
      if (!inPayload_) {
        if (!parseHeader(buf_, hdr_)) return false;
        inPayload_ = true;
        have_ = 0;
        need_ = (size_t)hdr_.nsamples * 2;
      } else {
        for (size_t i = 0; i < hdr_.nsamples; i++) pcm_[i] = (int16_t)(buf_[2 * i] | (buf_[2 * i + 1] << 8));
        onFrame(hdr_, (const int16_t*)pcm_);
        inPayload_ = false;
        have_ = 0;
        need_ = HDR_SIZE;
      }
    }
    return true;
  }

 private:
  uint8_t buf_[MAX_SAMPLES * 2];
  int16_t pcm_[MAX_SAMPLES];
  size_t have_, need_;
  bool inPayload_;
  FrameHeader hdr_;
};

// What the diagnostics screen shows about the network side: is a drop-out the Wi-Fi's fault?
struct NetStats {
  uint32_t frames = 0;
  uint32_t gaps = 0;         // sequence jumps (the Mac dropped frames because we read too slowly)
  uint32_t lostFrames = 0;
  uint32_t maxGapMs = 0;     // longest silence between two frames (> ~150 ms = the network)
  uint32_t reconnects = 0;
  uint32_t lastSeq = 0;
  uint32_t lastMs = 0;
  bool first = true;
  void onFrame(uint32_t seq, uint32_t nowMs) {
    if (!first) {
      if (seq != lastSeq + 1) { gaps++; lostFrames += seq - lastSeq - 1; }
      uint32_t d = nowMs - lastMs;
      if (d > maxGapMs) maxGapMs = d;
    }
    first = false;
    lastSeq = seq;
    lastMs = nowMs;
    frames++;
  }
  void onReconnect() { reconnects++; first = true; }
};

}  // namespace rdaudio
