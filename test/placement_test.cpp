// Frame-accuracy test for event placement.
//
// The thing being proved: an injected event lands inside the frame it belongs
// to, not wherever the recorder happened to flush. Tacview writes 16 KB blocks
// and how much mission time a block covers depends purely on traffic — 3-9 s
// on a busy mission, ~25 s (82 s for the first block) on a quiet one. Placing
// at the end of a block would therefore put a kill up to a minute late on a
// quiet recording.
//
//   g++ -O2 -std=c++17 -o placement_test test/placement_test.cpp src/acmi_inject.cpp -Isrc
//   ./placement_test

#include "acmi_inject.h"

#include <cstdio>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void check(bool ok, const std::string& what) {
  std::printf("%s  %s\n", ok ? "  ok  " : "  FAIL", what.c_str());
  if (!ok) ++g_failures;
}

/**
 * A recording with frames every 0.5 s, padded with filler objects so the data
 * rate resembles a real mission: a 16 KB write block should span a handful of
 * seconds, not minutes. That ratio is what decides whether a hold-back window
 * of a few seconds actually covers the gap between an event happening and us
 * being told about it.
 */
std::string buildStream(int frames) {
  std::string s =
      "FileType=text/acmi/tacview\n"
      "FileVersion=2.2\n"
      "0,ReferenceTime=2024-01-01T00:00:00Z\n";
  for (int i = 0; i < frames; ++i) {
    char buf[256];
    std::snprintf(buf, sizeof(buf), "#%.2f\n", i * 0.5);
    s += buf;
    // Two objects reporting each frame, one of them named so events resolve.
    std::snprintf(buf, sizeof(buf),
                  "101,T=1.0%03d|2.0|1000|0|0|90,Type=Air+FixedWing,Pilot=Enfield 1-1\n", i);
    s += buf;
    std::snprintf(buf, sizeof(buf), "202,T=3.0%03d|4.0|50|0|0|90,Pilot=Ground Unit 7\n", i);
    s += buf;
    // Filler traffic: ~20 objects per frame puts a 16 KB block at roughly
    // 6 s of stream, matching what the 2,238-unit mission produced.
    for (int j = 0; j < 20; ++j) {
      std::snprintf(buf, sizeof(buf), "%x,T=5.0%03d|6.0|%d|0|0|90\n", 0x300 + j, i, 100 + j);
      s += buf;
    }
  }
  return s;
}

/** Hold-back window, in stream seconds — mirrors the DLL's setting. */
constexpr double kDelay = 5.0;

/**
 * Run the stream through the injector exactly as the DLL does: push, write
 * whatever is old enough, and flush the remainder at close.
 *
 * `lateEvent` is queued once the stream clock passes `lateAt`, simulating an
 * event that reaches us only after its own frame has already been pushed —
 * the case the hold-back buffer exists for.
 */
std::string runThrough(dkswx::Injector& inj, const std::string& stream,
                       const std::vector<size_t>& chunkSizes,
                       const dkswx::PendingEvent* lateEvent = nullptr,
                       double lateAt = -1) {
  std::string out;
  size_t pos = 0;
  size_t k = 0;
  bool queued = false;
  while (pos < stream.size()) {
    size_t want = chunkSizes[k++ % chunkSizes.size()];
    size_t len = (pos + want > stream.size()) ? stream.size() - pos : want;

    inj.push(stream.data() + pos, len);
    out += inj.takeReady(kDelay);
    pos += len;

    if (lateEvent != nullptr && !queued && inj.currentTime() >= lateAt) {
      queued = true;
      inj.queueEvent(*lateEvent);
    }
  }
  out += inj.takeAll();
  return out;
}

/** The frame time in effect at the line containing `needle`. */
double frameOf(const std::string& text, const std::string& needle, bool* found) {
  *found = false;
  size_t at = text.find(needle);
  if (at == std::string::npos) return -1;
  *found = true;

  double frame = -1;
  size_t pos = 0;
  while (pos < at) {
    size_t nl = text.find('\n', pos);
    if (nl == std::string::npos || nl > at) break;
    if (text[pos] == '#') frame = atof(text.c_str() + pos + 1);
    pos = nl + 1;
  }
  return frame;
}

}  // namespace

int main() {
  const std::string stream = buildStream(600);  // 0 .. 299.5 s

  // Deliberately awkward: blocks that span many frames, and chunk edges that
  // fall mid-line, mid-number and mid-property.
  const std::vector<size_t> chunks = {16384, 137, 8192, 1, 16384, 991};

  {
    dkswx::Injector inj;
    dkswx::PendingEvent kill;
    kill.kind = "Destroyed";
    kill.dedupeBySubject = true;
    kill.time = 42.3;  // between frames 42.0 and 42.5
    kill.subjectUnit = "Ground Unit 7";
    kill.text = "Enfield 1-1";
    inj.queueEvent(kill);

    dkswx::PendingEvent late;
    late.kind = "Destroyed";
    late.dedupeBySubject = true;
    late.time = 150.0;  // exactly on a frame
    late.subjectUnit = "Enfield 1-1";
    inj.queueEvent(late);

    const std::string out = runThrough(inj, stream, chunks);

    bool found = false;
    double f = frameOf(out, "0,Event=Destroyed|202|", &found);
    check(found, "kill event present");
    // 42.3 falls between frames; the first frame at or after it is 42.5.
    check(found && f == 42.5,
          "kill lands in the first frame at or after its time (got " +
              std::to_string(f) + ", want 42.5)");

    f = frameOf(out, "0,Event=Destroyed|101|", &found);
    check(found, "second event present");
    check(found && f == 150.0,
          "event on an exact frame lands in that frame (got " + std::to_string(f) + ")");

    check(inj.emittedEventCount() == 2, "both events emitted");
  }

  {
    // The engine fires Kill, Dead and UnitLost for one death; the ACMI wants
    // a single Destroyed.
    dkswx::Injector inj;
    for (int i = 0; i < 3; ++i) {
      dkswx::PendingEvent e;
      e.kind = "Destroyed";
      e.dedupeBySubject = true;
      e.time = 10.0;
      e.subjectUnit = "Ground Unit 7";
      inj.queueEvent(e);
    }
    const std::string out = runThrough(inj, stream, chunks);

    size_t count = 0;
    for (size_t p = out.find("0,Event=Destroyed|202|"); p != std::string::npos;
         p = out.find("0,Event=Destroyed|202|", p + 1)) {
      ++count;
    }
    check(count == 1, "triple-fired death collapses to one Destroyed (got " +
                          std::to_string(count) + ")");
  }

  {
    // An event for a unit Tacview never declared must be dropped, not guessed.
    dkswx::Injector inj;
    dkswx::PendingEvent e;
    e.kind = "Destroyed";
    e.time = 5.0;
    e.subjectUnit = "Not In This Recording";
    inj.queueEvent(e);
    const std::string out = runThrough(inj, stream, chunks);

    check(out.find("0,Event=") == std::string::npos, "unresolvable event dropped");
    check(inj.emittedEventCount() == 0, "nothing emitted for an unknown unit");
  }

  {
    // Everything that is not an insertion must survive byte-for-byte.
    dkswx::Injector inj;
    const std::string out = runThrough(inj, stream, chunks);
    check(out == stream, "stream passes through untouched when nothing is queued");
  }

  {
    // The real case: the kill is only reported to us after its frame has been
    // pushed. Without the hold-back it would land wherever the stream had got
    // to; with it, the frame is still in our buffer and can be spliced.
    dkswx::Injector inj;
    dkswx::PendingEvent late;
    late.kind = "Destroyed";
    late.dedupeBySubject = true;
    late.time = 100.0;
    late.subjectUnit = "Ground Unit 7";
    // Queued once the stream is 2 s past it — the realistic lag between a
    // kill firing and the sampler handing it to us.
    const std::string out = runThrough(inj, stream, chunks, &late, 102.0);

    bool found = false;
    double f = frameOf(out, "0,Event=Destroyed|202|", &found);
    check(found, "late-arriving event present");
    check(found && f == 100.0,
          "late event still lands in its own frame (got " + std::to_string(f) +
              ", want 100.0)");
  }

  {
    // A hold-back is only useful if nothing is lost at the end.
    dkswx::Injector inj;
    const std::string out = runThrough(inj, stream, chunks);
    check(out.size() == stream.size(), "no bytes lost across hold and flush");
    check(inj.heldBytes() == 0, "nothing left held after takeAll");
  }

  {
    // Repeats must only collapse where the engine genuinely double-reports.
    // Several hits on one target are distinct facts, unlike the Kill/Dead/
    // UnitLost trio that describes a single death.
    dkswx::Injector inj;
    for (int i = 0; i < 3; ++i) {
      dkswx::PendingEvent hit;
      hit.kind = "Message";
      hit.time = 20.0 + i;
      hit.subjectUnit = "Ground Unit 7";
      hit.secondaryUnit = "Enfield 1-1";
      hit.text = "DKS:Hit FAB_250";
      inj.queueEvent(hit);
    }
    const std::string out = runThrough(inj, stream, chunks);

    size_t count = 0;
    for (size_t p = out.find("DKS:Hit"); p != std::string::npos;
         p = out.find("DKS:Hit", p + 1)) {
      ++count;
    }
    check(count == 3, "repeated hits are NOT collapsed (got " + std::to_string(count) + ")");
    check(out.find("0,Event=Message|202|101|DKS:Hit") != std::string::npos,
          "hit carries target as primary and shooter as secondary id");
  }

  {
    // Regression: the weather splice used to fix up the parse cursor twice —
    // once inside spliceAt and again at the call site — leaving it mid-line
    // and skipping roughly one weather block of content after every
    // injection. Byte output stayed plausible, so only the invariant catches
    // it.
    dkswx::Injector inj;
    dkswx::WindProfile profile;
    for (double alt = 0; alt <= 4000; alt += 500) {
      dkswx::WindSample w;
      w.altitudeM = alt;
      w.north = 5.0;
      w.east = 5.0;
      profile.add(w);
    }
    inj.setProfile(profile);
    inj.setQnhHpa(1013.25);

    size_t pos = 0;
    size_t k = 0;
    bool everBroken = false;
    while (pos < stream.size()) {
      size_t want = chunks[k++ % chunks.size()];
      size_t len = (pos + want > stream.size()) ? stream.size() - pos : want;
      inj.push(stream.data() + pos, len);
      inj.takeReady(kDelay);
      if (!inj.cursorAtLineBoundary()) everBroken = true;
      pos += len;
    }
    inj.takeAll();

    check(!everBroken, "parse cursor stays on a line boundary across weather splices");
    check(inj.injectedBytes() > 0, "weather actually got injected during that run");
  }

  {
    // The classic TAKEOFF/LAND fallbacks deliberately overlap the runway
    // pair, so one departure can arrive twice from two different events.
    dkswx::Injector inj;
    for (int i = 0; i < 2; ++i) {
      dkswx::PendingEvent to;
      to.kind = "TakenOff";
      to.time = 30.0;
      to.subjectUnit = "Enfield 1-1";
      to.dedupeWindowSec = 20.0;
      inj.queueEvent(to);
    }
    // The late classic event for the same departure, a few seconds behind.
    dkswx::PendingEvent late;
    late.kind = "TakenOff";
    late.time = 34.0;
    late.subjectUnit = "Enfield 1-1";
    late.dedupeWindowSec = 20.0;
    inj.queueEvent(late);

    // A genuinely separate departure, well outside the window.
    dkswx::PendingEvent again;
    again.kind = "TakenOff";
    again.time = 120.0;
    again.subjectUnit = "Enfield 1-1";
    again.dedupeWindowSec = 20.0;
    inj.queueEvent(again);

    const std::string out = runThrough(inj, stream, chunks);
    size_t count = 0;
    for (size_t p = out.find("0,Event=TakenOff"); p != std::string::npos;
         p = out.find("0,Event=TakenOff", p + 1)) {
      ++count;
    }
    check(count == 2, "overlapping fallback collapses, real repeat kept (got " +
                          std::to_string(count) + ")");
  }

  {
    // A human trap: DCS names the UNIT ("14A Initial"), Tacview writes the
    // PLAYER name in Pilot=. Resolving only by unit name dropped every human
    // event on a live server — traps, LSO grades, ejections, the lot.
    dkswx::Injector inj;
    dkswx::PendingEvent lso;
    lso.kind = "Bookmark";
    lso.time = 50.0;
    lso.subjectUnit = "14A Initial";      // what the engine reports
    lso.altSubjectUnit = "Enfield 1-1";   // the player name Tacview declared
    lso.text = "LSO: GRADE:--- : WIRE# 3";
    inj.queueEvent(lso);

    const std::string out = runThrough(inj, stream, chunks);
    check(out.find("0,Event=Bookmark|101||LSO: GRADE:--- : WIRE# 3") !=
              std::string::npos,
          "human event resolves via the player name when the unit name is unknown");

    // And an event that matches neither key is still dropped, not guessed.
    dkswx::Injector inj2;
    dkswx::PendingEvent orphan;
    orphan.kind = "Bookmark";
    orphan.time = 50.0;
    orphan.subjectUnit = "Nobody";
    orphan.altSubjectUnit = "Also Nobody";
    inj2.queueEvent(orphan);
    const std::string out2 = runThrough(inj2, stream, chunks);
    check(out2.find("0,Event=") == std::string::npos,
          "an event matching neither key is still dropped");
  }

  {
    // A human shot down: DCS fires Kill (subject = victim, but reported by the
    // killer so it carries no victim player name) before Dead (which does).
    // Collapsing by subject must not throw away the only resolvable key, or
    // the most common human event of all stays unresolvable.
    dkswx::Injector inj;

    dkswx::PendingEvent kill;
    kill.kind = "Destroyed";
    kill.dedupeBySubject = true;
    kill.time = 60.0;
    kill.subjectUnit = "14A Initial";   // unit name — Tacview never declares it
    kill.secondaryUnit = "Enfield 1-1";
    inj.queueEvent(kill);

    dkswx::PendingEvent dead;
    dead.kind = "Destroyed";
    dead.dedupeBySubject = true;
    dead.time = 60.0;
    dead.subjectUnit = "14A Initial";
    dead.altSubjectUnit = "Ground Unit 7";  // the player name Tacview declared
    inj.queueEvent(dead);

    const std::string out = runThrough(inj, stream, chunks);

    size_t count = 0;
    for (size_t p = out.find("0,Event=Destroyed"); p != std::string::npos;
         p = out.find("0,Event=Destroyed", p + 1)) {
      ++count;
    }
    check(count == 1, "one Destroyed for one death (got " + std::to_string(count) + ")");
    check(out.find("0,Event=Destroyed|202|") != std::string::npos,
          "the kill resolves via the alt key carried by the later duplicate");
  }

  std::printf("\n%s\n", g_failures == 0 ? "ALL PASS" : "FAILURES");
  return g_failures == 0 ? 0 : 1;
}
