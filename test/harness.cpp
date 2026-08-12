// Offline harness for the ACMI weather injector.
//
// Replays a real .txt.acmi through the injector the same way the DLL will see
// it in DCS — arbitrary-sized buffers that usually do NOT end on a line
// boundary — and writes the enriched stream out. This is where the stream
// logic gets verified, so we are never debugging it inside a live server.
//
//   g++ -O2 -std=c++17 -o harness test/harness.cpp src/acmi_inject.cpp -Isrc
//   ./harness in.txt.acmi out.txt.acmi
//
// The wind ladder here stands in for the sampler: it is DCS's three-layer
// model (ground / 2000 m / 8000 m) with the values measured off a real
// Persian Gulf recording, expanded to a 1 km ladder exactly as the in-sim
// sampler will do with atmosphere.getWind.

#include "acmi_inject.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr double kDegToRad = 3.14159265358979323846 / 180.0;

struct Layer {
  double altitudeM;
  double speedMs;
  double dirToDeg;  // direction the wind blows TO
};

/** DCS interpolates linearly between its three layers; the sampler will read
 *  the same field back out of the sim rather than assuming these numbers. */
const Layer kLayers[] = {
    {0.0, 2.83, 129.7},
    {2000.0, 7.22, 129.7},
    {8000.0, 13.40, 41.7},
};

dkswx::WindSample layerAt(double alt) {
  const Layer* lo = &kLayers[0];
  const Layer* hi = &kLayers[2];
  for (int i = 1; i < 3; ++i) {
    if (alt <= kLayers[i].altitudeM) {
      lo = &kLayers[i - 1];
      hi = &kLayers[i];
      break;
    }
  }
  double span = hi->altitudeM - lo->altitudeM;
  double f = (span > 0.0) ? (alt - lo->altitudeM) / span : 0.0;
  if (f < 0.0) f = 0.0;
  if (f > 1.0) f = 1.0;
  double speed = lo->speedMs + (hi->speedMs - lo->speedMs) * f;
  double dir = lo->dirToDeg + (hi->dirToDeg - lo->dirToDeg) * f;

  dkswx::WindSample s;
  s.altitudeM = alt;
  s.north = speed * std::cos(dir * kDegToRad);
  s.east = speed * std::sin(dir * kDegToRad);
  s.up = 0.0;
  return s;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: harness <in.txt.acmi> <out.txt.acmi>\n");
    return 2;
  }

  FILE* in = std::fopen(argv[1], "rb");
  if (!in) {
    std::fprintf(stderr, "cannot open %s\n", argv[1]);
    return 1;
  }
  FILE* out = std::fopen(argv[2], "wb");
  if (!out) {
    std::fprintf(stderr, "cannot write %s\n", argv[2]);
    return 1;
  }

  dkswx::WindProfile profile;
  for (double alt = 0.0; alt <= 12000.0; alt += 1000.0) profile.add(layerAt(alt));

  dkswx::Injector inj;
  inj.setProfile(profile);
  inj.setQnhHpa(1013.25);

  // Deliberately awkward buffer sizes: the injector must cope with writes that
  // end mid-line, mid-number, or mid-property.
  const size_t kSizes[] = {8192, 137, 65536, 4096, 991, 1, 32768};
  size_t sizeIdx = 0;

  std::vector<char> buf(65536);
  size_t injectedBytes = 0;
  size_t injectedLines = 0;

  for (;;) {
    size_t want = kSizes[sizeIdx++ % (sizeof(kSizes) / sizeof(kSizes[0]))];
    size_t got = std::fread(buf.data(), 1, want, in);
    if (got == 0) break;

    // Feed the injector and write whatever is old enough to leave the
    // hold-back window — the same sequence the DLL performs.
    inj.push(buf.data(), got);
    std::string ready = inj.takeReady(5.0);
    if (!ready.empty()) std::fwrite(ready.data(), 1, ready.size(), out);
  }

  // Nothing may be left behind at close.
  std::string rest = inj.takeAll();
  if (!rest.empty()) std::fwrite(rest.data(), 1, rest.size(), out);
  injectedBytes = inj.injectedBytes();

  std::fclose(in);
  std::fclose(out);

  std::printf("injected %zu bytes, final frame t=%.2f, live aircraft %zu\n",
              injectedBytes, inj.currentTime(), inj.liveAircraftCount());
  return 0;
}
