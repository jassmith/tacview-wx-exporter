#include "acmi_inject.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace dkswx {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kRadToDeg = 180.0 / kPi;

/** Round to `decimals` places — keeps emitted lines the same shape as
 *  Tacview's own (it writes 2-3 significant decimals, not full doubles). */
double roundTo(double v, int decimals) {
  double f = std::pow(10.0, decimals);
  return std::round(v * f) / f;
}

/** Split an ACMI property list on unescaped commas. */
std::vector<std::string> splitProps(const std::string& s) {
  std::vector<std::string> out;
  std::string cur;
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '\\' && i + 1 < s.size()) {
      cur += s[i + 1];
      ++i;
      continue;
    }
    if (s[i] == ',') {
      out.push_back(cur);
      cur.clear();
      continue;
    }
    cur += s[i];
  }
  out.push_back(cur);
  return out;
}

/**
 * Altitude out of a `T=` value. The transform is
 * `Lon|Lat|Alt|Roll|Pitch|Yaw|U|V|Heading` with any field allowed to be empty,
 * meaning "unchanged" — so a missing altitude is not zero, it is "keep the
 * previous value", which is why the caller holds last-known state.
 */
bool parseAltitude(const std::string& t, double* outAlt) {
  int field = 0;
  size_t start = 0;
  while (field <= 2) {
    size_t bar = t.find('|', start);
    std::string piece =
        (bar == std::string::npos) ? t.substr(start) : t.substr(start, bar - start);
    if (field == 2) {
      if (piece.empty()) return false;
      *outAlt = std::atof(piece.c_str());
      return true;
    }
    if (bar == std::string::npos) return false;
    start = bar + 1;
    ++field;
  }
  return false;
}

}  // namespace

// ---------------------------------------------------------------------------
// WindProfile
// ---------------------------------------------------------------------------

void WindProfile::clear() { samples_.clear(); }

void WindProfile::add(const WindSample& s) {
  samples_.push_back(s);
  std::sort(samples_.begin(), samples_.end(),
            [](const WindSample& a, const WindSample& b) {
              return a.altitudeM < b.altitudeM;
            });
}

WindSample WindProfile::at(double altitudeM) const {
  if (samples_.empty()) return WindSample{};
  if (altitudeM <= samples_.front().altitudeM) return samples_.front();
  if (altitudeM >= samples_.back().altitudeM) return samples_.back();

  for (size_t i = 1; i < samples_.size(); ++i) {
    const WindSample& hi = samples_[i];
    if (altitudeM > hi.altitudeM) continue;
    const WindSample& lo = samples_[i - 1];
    double span = hi.altitudeM - lo.altitudeM;
    double f = (span > 0.0) ? (altitudeM - lo.altitudeM) / span : 0.0;
    WindSample out;
    out.altitudeM = altitudeM;
    out.north = lo.north + (hi.north - lo.north) * f;
    out.up = lo.up + (hi.up - lo.up) * f;
    out.east = lo.east + (hi.east - lo.east) * f;
    return out;
  }
  return samples_.back();
}

std::string WindProfile::formatAt(double altitudeM) const {
  WindSample w = at(altitudeM);

  double horiz = std::sqrt(w.north * w.north + w.east * w.east);
  // DCS vec3: x = north, z = east. atan2(east, north) gives a compass bearing
  // measured clockwise from true north — the direction the wind blows TO.
  double dirTo = std::atan2(w.east, w.north) * kRadToDeg;
  if (dirTo > 180.0) dirTo -= 360.0;
  if (dirTo <= -180.0) dirTo += 360.0;
  double pitch = (horiz > 1e-9) ? std::atan2(w.up, horiz) * kRadToDeg : 0.0;

  // Spherical reading: WindSpeed is the FULL 3-D magnitude, with WindPitch the
  // elevation off horizontal. Real recordings cannot distinguish this from
  // "WindSpeed = horizontal magnitude" — |WindPitch| never exceeds ~3° — but
  // the DKS parser resolves the triple sphericallly, and the exporter and the
  // reader must agree or we bake in an error of our own making. Spherical is
  // also the self-consistent choice: |V| == WindSpeed exactly, with no tan()
  // blowing up as the vector approaches vertical.
  double speed = std::sqrt(w.north * w.north + w.up * w.up + w.east * w.east);

  char buf[128];
  std::snprintf(buf, sizeof(buf), "WindDirection=%.1f,WindPitch=%.1f,WindSpeed=%.2f",
                roundTo(dirTo, 1), roundTo(pitch, 1), roundTo(speed, 2));
  return std::string(buf);
}

// ---------------------------------------------------------------------------
// Injector
// ---------------------------------------------------------------------------

void Injector::reset() {
  hold_.clear();
  frames_.clear();
  parseCursor_ = 0;
  lineStart_ = 0;
  injectedBytes_ = 0;
  time_ = 0.0;
  sawFirstFrame_ = false;
  qnhEmitted_ = false;
  objects_.clear();
  unitToObject_.clear();
  events_.clear();
  destroyed_.clear();
  lastSeen_.clear();
  eventsEmitted_ = 0;
}

size_t Injector::liveAircraftCount() const {
  size_t n = 0;
  for (const auto& kv : objects_) {
    if (kv.second.isAircraft && kv.second.alive) ++n;
  }
  return n;
}

void Injector::push(const char* data, size_t len) {
  if (len == 0) return;
  hold_.append(data, len);

  // Parse forward from where we left off. Splicing inside this loop shifts
  // everything after the insertion point, so the cursor is advanced by the
  // inserted length rather than recomputed.
  while (true) {
    size_t nl = hold_.find('\n', parseCursor_);
    if (nl == std::string::npos) break;

    std::string line = hold_.substr(lineStart_, nl - lineStart_);
    if (!line.empty() && line.back() == '\r') line.pop_back();

    const bool isFrame = !line.empty() && line[0] == '#';
    consumeLine(line);

    size_t afterLine = nl + 1;
    if (isFrame) {
      FrameMark fm;
      fm.offset = afterLine;
      fm.time = time_;
      frames_.push_back(fm);

    }

    parseCursor_ = afterLine;
    lineStart_ = afterLine;
  }

  placeDueEvents();

  // Weather rides at the newest frame boundary; it is insensitive to a few
  // seconds either way, so it costs no extra bookkeeping.
  if (!frames_.empty()) {
    std::string wx = buildWeatherBlock();
    if (!wx.empty()) {
      // spliceAt already fixes up parseCursor_/lineStart_; adjusting them
      // again here double-counted the insertion, so the parser skipped roughly
      // one weather block's worth of content after every injection.
      spliceAt(frames_.back().offset, wx);
    }
  }
}

void Injector::spliceAt(size_t offset, const std::string& text) {
  if (text.empty()) return;
  hold_.insert(offset, text);
  for (FrameMark& f : frames_) {
    if (f.offset > offset) f.offset += text.size();
  }
  if (parseCursor_ > offset) parseCursor_ += text.size();
  if (lineStart_ > offset) lineStart_ += text.size();
  injectedBytes_ += text.size();
}

std::string Injector::takeReady(double delaySec) {
  if (frames_.empty()) return std::string();

  const double threshold = time_ - delaySec;
  size_t cutoff = 0;
  size_t keepFrom = 0;
  for (size_t i = 0; i < frames_.size(); ++i) {
    if (frames_[i].time > threshold) break;
    cutoff = frames_[i].offset;
    keepFrom = i + 1;
  }
  if (cutoff == 0) return std::string();

  std::string out = hold_.substr(0, cutoff);
  hold_.erase(0, cutoff);
  frames_.erase(frames_.begin(), frames_.begin() + keepFrom);
  for (FrameMark& f : frames_) f.offset -= cutoff;
  parseCursor_ = (parseCursor_ > cutoff) ? parseCursor_ - cutoff : 0;
  lineStart_ = (lineStart_ > cutoff) ? lineStart_ - cutoff : 0;
  return out;
}

std::string Injector::takeAll() {
  // Anything still due at close would otherwise be dropped: no further frame
  // arrives to carry it. Emit it at the last frame we saw — late, but a
  // recorded kill beats a lost one, and this only affects events that arrive
  // in the final moments of a mission.
  placeDueEvents();

  std::string out;
  out.swap(hold_);
  frames_.clear();
  parseCursor_ = 0;
  lineStart_ = 0;
  return out;
}

void Injector::consumeLine(const std::string& line) {
  if (line.empty()) return;

  // Frame marker — everything after this belongs to a new recording time.
  if (line[0] == '#') {
    time_ = std::atof(line.c_str() + 1);
    sawFirstFrame_ = true;
    return;
  }

  // Object removal.
  if (line[0] == '-') {
    auto it = objects_.find(line.substr(1));
    if (it != objects_.end()) it->second.alive = false;
    return;
  }

  size_t comma = line.find(',');
  if (comma == std::string::npos) return;
  std::string id = line.substr(0, comma);
  if (id == "0") return;  // global property line

  ObjectState& st = objects_[id];
  st.alive = true;

  for (const std::string& prop : splitProps(line.substr(comma + 1))) {
    size_t eq = prop.find('=');
    if (eq == std::string::npos) continue;
    std::string key = prop.substr(0, eq);
    if (key == "Type") {
      // Tacview's own classification — "Air+FixedWing", "Air+Rotorcraft".
      st.isAircraft = prop.find("Air+") != std::string::npos &&
                      prop.find("Missile") == std::string::npos;
    } else if (key == "Pilot") {
      // Tacview's `Pilot` carries the DCS unit name for AI and statics, and
      // the player name for humans. Either way it is the key DCS events
      // reference, and it is the only bridge between an event and a hex id.
      if (!prop.empty() && eq + 1 < prop.size()) {
        unitToObject_[prop.substr(eq + 1)] = id;
      }
    } else if (key == "T") {
      double alt = 0.0;
      if (parseAltitude(prop.substr(eq + 1), &alt)) {
        st.altitudeM = alt;
        st.haveAltitude = true;
      }
    }
  }
}

void Injector::queueEvent(const PendingEvent& e) {
  // One death fires Kill, Dead and UnitLost on the same tick. The ACMI models
  // that as a single Destroyed, so collapse them here rather than emitting
  // three and having every downstream consumer count the kill three times.
  if (e.dedupeBySubject) {
    if (destroyed_.count(e.subjectUnit) > 0) {
      // Already have one for this subject, but a later duplicate may carry the
      // alternate key the first lacked: DCS fires Kill before Dead, and only
      // Dead names the victim itself, so the Kill arrives without the victim's
      // player name. Merge it in rather than discarding a resolvable key.
      if (!e.altSubjectUnit.empty()) {
        for (PendingEvent& queued : events_) {
          if (queued.kind == e.kind && queued.subjectUnit == e.subjectUnit &&
              queued.altSubjectUnit.empty()) {
            queued.altSubjectUnit = e.altSubjectUnit;
            break;
          }
        }
      }
      return;
    }
    destroyed_.insert(e.subjectUnit);
  }

  if (e.dedupeWindowSec > 0.0) {
    const std::string key = e.kind + "\x1f" + e.subjectUnit;
    auto prev = lastSeen_.find(key);
    if (prev != lastSeen_.end() && e.time - prev->second < e.dedupeWindowSec &&
        e.time >= prev->second) {
      return;
    }
    lastSeen_[key] = e.time;
  }

  events_.push_back(e);
}

std::string Injector::buildWeatherBlock() {
  // Never emit into the header block: property lines are only meaningful once
  // a frame time is in effect.
  if (!sawFirstFrame_) return std::string();

  std::string out;
  if (cfg_.emitQnh && !qnhEmitted_ && qnhHpa_ > 0.0) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "0,QNH=%.2f\n", qnhHpa_);
    out += buf;
    qnhEmitted_ = true;
  }

  if (profile_.empty()) return out;

  for (auto& kv : objects_) {
    ObjectState& st = kv.second;
    if (!st.isAircraft || !st.alive || !st.haveAltitude) continue;
    if (time_ - st.lastEmitTime < cfg_.refreshIntervalSec) continue;
    st.lastEmitTime = time_;
    out += kv.first;
    out += ',';
    out += profile_.formatAt(st.altitudeM);
    out += '\n';
  }
  return out;
}

void Injector::placeDueEvents() {
  if (events_.empty() || frames_.empty()) return;

  size_t keep = 0;
  for (size_t i = 0; i < events_.size(); ++i) {
    const PendingEvent e = events_[i];

    // The stream has not reached it yet; its frame is still to come.
    if (e.time > time_) {
      events_[keep++] = e;
      continue;
    }

    auto it = unitToObject_.find(e.subjectUnit);
    if (it == unitToObject_.end() && !e.altSubjectUnit.empty()) {
      it = unitToObject_.find(e.altSubjectUnit);
    }
    if (it == unitToObject_.end()) {
      // Tacview has not declared this object. It may still be coming, so hold
      // briefly; after that drop it rather than invent an id — Tacview does
      // not record every DCS unit.
      if (time_ - e.time < 30.0) {
        events_[keep++] = e;
      } else if (traceSink_ != nullptr) {
        // Visible rather than silent: a dropped kill is a missing kill in the
        // debrief, and the count matters when judging whether Tacview's object
        // coverage is good enough to rely on.
        char msg[224];
        std::snprintf(msg, sizeof(msg),
                      "DROPPED %s eventT=%.2f unit='%s' - never declared by Tacview",
                      e.kind.c_str(), e.time, e.subjectUnit.c_str());
        traceSink_(msg);
      }
      continue;
    }

    // Place it in the frame it actually belongs to: the earliest held frame at
    // or after its time. This is the whole point of holding the tail — the
    // event usually arrives after that frame has been parsed, and without this
    // it would land wherever the stream had since got to.
    size_t target = frames_.back().offset;
    double targetTime = frames_.back().time;
    for (const FrameMark& f : frames_) {
      if (f.time >= e.time) {
        target = f.offset;
        targetTime = f.time;
        break;
      }
    }

    // 0,Event=<Kind>|<primaryId>|<secondaryId>|<text>
    std::string secondaryId;
    if (!e.secondaryUnit.empty()) {
      auto sit = unitToObject_.find(e.secondaryUnit);
      if (sit != unitToObject_.end()) secondaryId = sit->second;
    }

    std::string line = "0,Event=";
    line += e.kind;
    line += '|';
    line += it->second;
    line += '|';
    line += secondaryId;
    line += '|';
    line += e.text;
    line += '\n';
    spliceAt(target, line);
    ++eventsEmitted_;
    if (traceSink_ != nullptr) {
      char msg[224];
      std::snprintf(msg, sizeof(msg),
                    "placed %s eventT=%.2f -> frame %.2f (streamT=%.2f, held %.2f..%.2f)",
                    e.kind.c_str(), e.time, targetTime, time_, frames_.front().time,
                    frames_.back().time);
      traceSink_(msg);
    }
  }
  events_.resize(keep);
}

}  // namespace dkswx
