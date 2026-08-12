// ACMI weather injection — pure stream logic, no Windows dependencies.
//
// Watches the plaintext ACMI byte stream Tacview writes and decides when to
// append weather property lines to it. Everything here is deterministic over
// its inputs so it can be exercised offline against real recordings
// (see test/harness.cpp) instead of only inside DCS.
//
// Why append rather than splice: writes go to the same file handle Tacview
// owns, so the file pointer is shared and sequential. We only ever emit after
// a buffer that ended on '\n', which makes an appended block indistinguishable
// from lines Tacview wrote itself — and lands it in the frame (`#t`) currently
// in effect, so no clock mapping is needed.
//
// Why property-only lines on Tacview's own objects: the recording may be
// protected (PlaybackDelay > 0), and that cipher advances per encrypted digit
// across Lng/Lat/U/V of DYNAMIC objects only. Property lines carry none of
// those fields, so they cannot desync it. Introducing a synthetic moving
// object would.

#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace dkswx {

// ---------------------------------------------------------------------------
// Wind profile
// ---------------------------------------------------------------------------

/** One sampled layer: wind vector in DCS's frame, metres per second. */
struct WindSample {
  double altitudeM = 0.0;
  double north = 0.0;  // vec3.x
  double up = 0.0;     // vec3.y
  double east = 0.0;   // vec3.z
};

/**
 * An altitude ladder sampled from `atmosphere.getWind`. DCS interpolates
 * linearly between its ground / 2000 m / 8000 m layers, so a ladder sampled
 * across that span reproduces the sim's field rather than approximating it.
 * Values between rungs are interpolated the same way; outside the ladder the
 * nearest rung is held.
 */
class WindProfile {
 public:
  void clear();
  void add(const WindSample& s);
  bool empty() const { return samples_.empty(); }

  /** Interpolated wind at an MSL altitude, in metres per second. */
  WindSample at(double altitudeM) const;

  /**
   * Format the Tacview property triple for an altitude.
   *
   * Units and conventions match what DCS's own client-side recordings emit,
   * measured against a wind-triangle solve on a real recording:
   *   WindSpeed     — horizontal magnitude, m/s
   *   WindDirection — direction the wind blows TO, signed degrees (-180, 180]
   *   WindPitch     — vertical component, degrees above horizontal
   * Returns e.g. "WindDirection=-90.3,WindPitch=0.1,WindSpeed=2.21".
   */
  std::string formatAt(double altitudeM) const;

 private:
  std::vector<WindSample> samples_;  // kept sorted by altitude
};

// ---------------------------------------------------------------------------
// Stream injector
// ---------------------------------------------------------------------------

struct InjectorConfig {
  /** Recording-time seconds between weather refreshes for a given object. */
  double refreshIntervalSec = 10.0;
  /** Emit `0,QNH=` once, the first time a QNH value is supplied. */
  bool emitQnh = true;
};

/**
 * An engine-sourced event waiting to be placed in the stream.
 *
 * `time` is DCS mission time, which shares an origin with the ACMI `#t`
 * clock, so an event can be held until the stream actually reaches it rather
 * than being dropped wherever the recorder happened to flush.
 */
struct PendingEvent {
  /** Tacview event name: Destroyed / TakenOff / Landed / Bookmark / Message. */
  std::string kind;
  double time = 0.0;
  /** DCS unit name of the object the event is about (the ACMI primary id). */
  std::string subjectUnit;
  /**
   * Optional second party — the shooter for a hit, the killer for a kill.
   * The ACMI event line carries two object ids, so this is where it belongs
   * rather than buried in free text. Emitted empty if it cannot be resolved.
   */
  std::string secondaryUnit;
  /** Free text. Tacview shows it; other parsers ignore it. */
  std::string text;
  /**
   * Alternate key for the subject, tried if `subjectUnit` does not resolve.
   *
   * DCS events name the UNIT, but Tacview writes the PLAYER name in `Pilot=`
   * for human-flown aircraft (and the unit name only for AI). Without the
   * second key every human event — traps, LSO grades, ejections — resolves to
   * nothing and is dropped.
   */
  std::string altSubjectUnit;
  /** Collapse repeats naming the same subject (one death, three events). */
  bool dedupeBySubject = false;
  /**
   * Collapse a repeat of the same kind+subject within this many seconds.
   *
   * One real departure can arrive twice because the classic TAKEOFF/LAND
   * events and the runway pair are both subscribed and overlap. Zero disables,
   * which is right for anything that legitimately repeats in quick succession
   * (hits).
   */
  double dedupeWindowSec = 0.0;
};

/**
 * Consumes the outgoing ACMI byte stream and returns what to splice into it.
 *
 * Usage per WriteFile: call observe() with the bytes Tacview is about to
 * write, then write the buffer in segments around the returned insertions.
 *
 * Events are placed at the frame they belong to, not at the end of the write.
 * That distinction matters: Tacview flushes 16 KB at a time, and how much
 * mission time that covers depends entirely on how much traffic there is —
 * measured at 3-9 s on a 2,238-unit mission but ~25 s (and 82 s for the first
 * block) on a two-aircraft one. Appending at the end of the block would put a
 * kill up to a minute late on a quiet recording.
 */
class Injector {
 public:
  explicit Injector(InjectorConfig cfg = {}) : cfg_(cfg) {}

  /** Replace the wind ladder (called when the sampler produces a new one). */
  void setProfile(const WindProfile& p) { profile_ = p; }
  void setQnhHpa(double hpa) { qnhHpa_ = hpa; }

  /**
   * Queue an engine event. Emitted once the stream's clock reaches its time
   * AND the subject's DCS unit name can be matched to an object Tacview has
   * declared — the ACMI references objects by hex id, which only the stream
   * knows. Unresolvable events are dropped rather than guessed at.
   */
  void queueEvent(const PendingEvent& e);
  size_t pendingEventCount() const { return events_.size(); }
  unsigned long emittedEventCount() const { return eventsEmitted_; }

  /** Feed the bytes Tacview wants written. They are held, not emitted. */
  void push(const char* data, size_t len);

  /**
   * Bytes that are now safe to write: everything up to the newest frame older
   * than `delaySec` of stream time. Holding the tail is what makes exact event
   * placement possible — an event can only be spliced into a frame that has
   * not gone to disk yet, and we learn about events asynchronously.
   */
  std::string takeReady(double delaySec);

  /** Everything still held. For close, and before any seek or truncate. */
  std::string takeAll();

  size_t heldBytes() const { return hold_.size(); }
  size_t injectedBytes() const { return injectedBytes_; }

  /**
   * Invariant: the parse position always sits at the start of a line.
   *
   * Splicing shifts every offset after the insertion point, and getting that
   * fix-up wrong (applying it twice, say) leaves the cursor mid-line, after
   * which the parser silently skips content. Cheap to assert, and the only
   * direct symptom before things go subtly wrong downstream.
   */
  bool cursorAtLineBoundary() const {
    return lineStart_ == 0 ||
           (lineStart_ <= hold_.size() && hold_[lineStart_ - 1] == '\n');
  }

  /** Optional diagnostic sink; the DLL points this at its log file. */
  void setTraceSink(void (*sink)(const char*)) { traceSink_ = sink; }

  /** Reset between recordings. */
  void reset();

  // Introspection for tests.
  double currentTime() const { return time_; }
  size_t liveAircraftCount() const;

 private:
  struct ObjectState {
    bool isAircraft = false;
    bool alive = true;
    bool haveAltitude = false;
    double altitudeM = 0.0;
    double lastEmitTime = -1e9;
  };

  /** A frame marker inside `hold_`: where its line ends, and its time. */
  struct FrameMark {
    size_t offset = 0;
    double time = 0.0;
  };

  void consumeLine(const std::string& line);
  std::string buildWeatherBlock();
  /** Splice every event whose time has been reached into its own frame. */
  void placeDueEvents();
  /** Splice `text` into hold_ at `offset`, fixing up everything after it. */
  void spliceAt(size_t offset, const std::string& text);

  InjectorConfig cfg_;
  WindProfile profile_;
  double qnhHpa_ = 0.0;
  bool qnhEmitted_ = false;

  /** Output not yet written: Tacview's bytes plus the splices already made. */
  std::string hold_;
  /** How far into hold_ we have parsed. */
  size_t parseCursor_ = 0;
  /** Start of the line currently being assembled. */
  size_t lineStart_ = 0;
  std::vector<FrameMark> frames_;
  double time_ = 0.0;     // current `#t` frame time
  bool sawFirstFrame_ = false;
  std::unordered_map<std::string, ObjectState> objects_;

  /** DCS unit name (Tacview's `Pilot=`) -> hex object id. */
  std::unordered_map<std::string, std::string> unitToObject_;
  std::vector<PendingEvent> events_;
  /** Victims already marked destroyed. DCS fires Kill, Dead and UnitLost for
   *  a single death, all on the same tick; the ACMI wants one Destroyed. */
  std::unordered_set<std::string> destroyed_;
  /** kind+subject -> time of the last one emitted, for windowed dedup. */
  std::unordered_map<std::string, double> lastSeen_;
  unsigned long eventsEmitted_ = 0;
  size_t injectedBytes_ = 0;
  void (*traceSink_)(const char*) = nullptr;
};

}  // namespace dkswx
