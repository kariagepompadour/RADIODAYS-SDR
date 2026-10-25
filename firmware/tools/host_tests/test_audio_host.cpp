// Host tests for RadioDays_AudioProto.h and RadioDays_Playout.h (no Arduino needed).
//   usage: test_audio_host stream.bin   (a capture of the Mac server's stream: everything after "OK\n")
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <random>
#include <cmath>
#include "../../RadioDays_SDR/RadioDays_AudioProto.h"
#include "../../RadioDays_SDR/RadioDays_Playout.h"
using namespace rdaudio;
static int fails = 0;
static void check(bool ok, const char* m) { printf("%s  %s\n", ok ? "OK  " : "FAIL", m); if (!ok) fails++; }

static void testStream(const char* path) {
  FILE* f = fopen(path, "rb"); if (!f) { puts("cannot open stream"); fails++; return; }
  std::vector<uint8_t> all; uint8_t b[4096]; size_t n; while ((n = fread(b, 1, sizeof b, f)) > 0) all.insert(all.end(), b, b + n); fclose(f);
  std::mt19937 rng(7);
  for (int mode = 0; mode < 3; mode++) {   // 0: random chunks 1..64, 1: random 1..3000, 2: one byte at a time
    FrameAssembler a; NetStats st; uint32_t frames = 0, samples = 0, t = 0; double sumsq = 0; bool ok = true; uint16_t rate = 0;
    size_t pos = 0;
    while (pos < all.size()) {
      size_t c = mode == 2 ? 1 : (mode == 0 ? 1 + rng() % 64 : 1 + rng() % 3000);
      if (pos + c > all.size()) c = all.size() - pos;
      ok &= a.feed(all.data() + pos, c, [&](const FrameHeader& h, const int16_t* pcm) {
        st.onFrame(h.seq, t += 43); frames++; samples += h.nsamples; rate = h.rate; for (uint16_t i = 0; i < h.nsamples; i++) sumsq += (double)pcm[i] * pcm[i]; });
      pos += c;
    }
    char m[160];
    snprintf(m, sizeof m, "stream parsed with chunk mode %d: %u frames, %u samples, rate %u, gaps %u, rms %.0f, no protocol error", mode, frames, samples, rate, st.gaps, std::sqrt(sumsq / (samples ? samples : 1)));
    check(ok && frames > 50 && st.gaps == 0 && rate >= 11990 && rate <= 12010 && samples % frames == 0 && sumsq > 0, m);
    if (mode == 0) printf("      (fake tone: rms ~11585; a real receiver: whatever the band carries)\n");
  }
  // a corrupted stream is detected
  { FrameAssembler a; std::vector<uint8_t> bad(all.begin(), all.begin() + 30); bad[0] = 'X'; bool ok = a.feed(bad.data(), bad.size(), [&](const FrameHeader&, const int16_t*) {}); check(!ok, "wrong magic is a protocol error"); }
  { FrameAssembler a; uint8_t h[10] = {'R','D',0,0,1,0,0,0,0xC0,0x2E}; bool ok = a.feed(h, 10, [&](const FrameHeader&, const int16_t*) {}); check(!ok, "zero-length frame is a protocol error"); }
  // sequence gaps are counted
  { NetStats st; st.onFrame(10, 0); st.onFrame(11, 43); st.onFrame(15, 300); check(st.gaps == 1 && st.lostFrames == 3 && st.maxGapMs == 257, "NetStats counts gaps, lost frames and the longest silence"); }
}

// A simulated 10 minutes: frames of 512 samples every 42.67 ms with network delay; the speaker consumes in real time.
struct Sim {
  Playout po; uint32_t fill = 0, spkQueued = 0, maxFill = 0, stuck = 0; double spkPos = 0;
  // speaker: two slots, each chunk plays for chunk/rate seconds
  double slotEnd[2] = {0, 0}; int slotsUsed = 0;
  uint32_t played = 0;
};
static constexpr uint32_t FRAME = 1024;  // a real KiwiSDR sends 1024 samples (85 ms) per frame; a test source sends 512
static void simulate(const char* name, double seconds, double jitterMs, double stallAtS, double stallMs, double ppm, int expectUnderruns, bool exact) {
  std::mt19937 rng(3); Sim s; const double rate = 12000.0 * (1.0 + ppm * 1e-6), frameMs = FRAME / 12000.0 * 1000.0;
  // arrival times of frames (sender clock = 12000 exactly, our consumer clock = rate)
  std::vector<std::pair<double, int>> arr; double t = 0; int n = 0;
  double lastArr = 0;
  while (t < seconds * 1000) {
    double d = jitterMs > 0 ? (double)(rng() % (int)(jitterMs * 10)) / 10.0 : 0; double a = t + d;
    if (stallAtS >= 0 && t >= stallAtS * 1000 && t < stallAtS * 1000 + stallMs) a = stallAtS * 1000 + stallMs + (t - stallAtS * 1000) * 0.0 + (double)(n % 2); // held, then delivered in a burst
    if (a < lastArr) a = lastArr; lastArr = a;   // TCP keeps order
    arr.push_back({a, n++}); t += frameMs;
  }
  size_t ai = 0; double now = 0; const double dt = 1.0; uint32_t maxFill = 0; bool wasPlaying = false;
  double chunkMs = 512.0 / rate * 1000.0; double slotBusyUntil[2] = {0, 0};
  for (now = 0; now < seconds * 1000; now += dt) {
    while (ai < arr.size() && arr[ai].first <= now) { if (s.po.admit(s.fill, FRAME)) s.fill += FRAME; ai++; }
    // speaker consumes: the chunk in slot 0 plays until slotBusyUntil[0], then slot 1 ...
    bool busy0 = slotBusyUntil[0] > now, busy1 = slotBusyUntil[1] > now;
    bool slotFree = !(busy0 && busy1), idle = !busy0 && !busy1;
    uint32_t give = s.po.onFeed(s.fill, slotFree, idle);
    if (give) { s.fill -= give; double start = std::max(now, std::max(slotBusyUntil[0], slotBusyUntil[1])); // queue behind the playing chunk
      if (!busy0) slotBusyUntil[0] = start + chunkMs; else slotBusyUntil[1] = start + chunkMs;
      if (!busy0 && !busy1) {} 
      // normalise so slot 0 is the earlier one
      if (slotBusyUntil[1] < slotBusyUntil[0]) std::swap(slotBusyUntil[0], slotBusyUntil[1]); s.played += give; }
    if (s.fill > maxFill) maxFill = s.fill;
    wasPlaying = s.po.playing;
  }
  char m[220]; snprintf(m, sizeof m, "%s: underruns %u (expected %s%d), overruns %u, rebuffers %u, max fill %u of %u, played %.1f s", name, s.po.underruns, exact ? "" : "<=", expectUnderruns, s.po.overruns, s.po.rebuffers, maxFill, s.po.p.capacity, s.played / 12000.0);
  bool okU = exact ? s.po.underruns == (uint32_t)expectUnderruns : s.po.underruns <= (uint32_t)expectUnderruns;
  check(okU && maxFill <= s.po.p.capacity && s.po.playing, m);
}

int main(int argc, char** argv) {
  if (argc > 1) testStream(argv[1]);
  simulate("clean network, 5 min", 300, 0, -1, 0, 0, 0, true);
  simulate("Wi-Fi jitter up to 120 ms, 10 min", 600, 120, -1, 0, 0, 0, true);
  simulate("one 600 ms stall (longer than the 250 ms pre-buffer), recovers by itself", 120, 20, 60, 600, 0, 1, true);
  simulate("one 150 ms stall is absorbed by the buffer", 120, 20, 60, 150, 0, 0, true);
  simulate("sender 100 ppm faster (buffer fills, frames dropped, never stuck), 10 min", 600, 20, -1, 0, -100, 0, false);
  simulate("our clock 100 ppm faster (buffer drains, a rare re-buffer), 10 min", 600, 20, -1, 0, +100, 40, false);
  simulate("STRESS sender 1% faster (frames must be dropped, never stuck), 10 min", 600, 20, -1, 0, -10000, 0, false);
  simulate("STRESS our clock 1% faster (repeated re-buffering, always recovers), 10 min", 600, 20, -1, 0, +10000, 100000, false);
  printf("%d failure(s)\n", fails);
  return fails ? 1 : 0;
}
