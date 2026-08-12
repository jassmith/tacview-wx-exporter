// DKS Tacview weather exporter — proxy DLL.
//
// Installs as Mods/tech/Tacview/bin/tacview.dll with Tacview's own recorder
// renamed alongside it as tacview_real.dll. Tacview's stock hook
// (Scripts/Hooks/TacviewGameGUI.lua) then loads us with no Lua changes at all:
// it only ever calls `require('tacview')`, and `luaopen_tacview` is the single
// symbol the real recorder exports.
//
// We forward that symbol untouched, so Tacview records exactly as it always
// did. The only thing we add is an IAT patch on the real module's CreateFileW
// and WriteFile: when it writes the plaintext .txt.acmi, we split its buffer
// at the last line boundary and insert weather property lines there.
//
// Wind comes from lua/DKSWeatherSampler.lua, which samples atmosphere.getWind
// in the mission environment and writes an altitude ladder to a small text
// file beside this DLL. Keeping the sampler in Lua keeps the sim-facing half
// readable and auditable, and keeps the Lua API out of this binary.

#include <windows.h>

#include <cstdio>
#include <string>
#include <vector>

#include "acmi_inject.h"
#include "zipw.h"

namespace {

// ---------------------------------------------------------------------------
// Shared state
// ---------------------------------------------------------------------------

CRITICAL_SECTION g_lock;
dkswx::Injector* g_injector = nullptr;
HANDLE g_acmiHandle = INVALID_HANDLE_VALUE;
HMODULE g_realModule = nullptr;
std::wstring g_moduleDir;
std::wstring g_profilePath;
std::wstring g_eventsPath;
LONGLONG g_eventsOffset = 0;
ULONGLONG g_lastProfilePoll = 0;
FILETIME g_lastProfileWrite = {};

using CreateFileW_t = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES,
                                      DWORD, DWORD, HANDLE);
using WriteFile_t = BOOL(WINAPI*)(HANDLE, LPCVOID, DWORD, LPDWORD, LPOVERLAPPED);
using SetFilePointer_t = DWORD(WINAPI*)(HANDLE, LONG, PLONG, DWORD);
using DeleteFileW_t = BOOL(WINAPI*)(LPCWSTR);
using CloseHandle_t = BOOL(WINAPI*)(HANDLE);
using SetFilePointerEx_t = BOOL(WINAPI*)(HANDLE, LARGE_INTEGER, PLARGE_INTEGER, DWORD);

/**
 * Seconds of stream held back before writing.
 *
 * Events reach us asynchronously — the sampler drains them every 0.5 s and we
 * only see the file at Tacview's next flush — so by the time we know about a
 * kill, the frame it belongs to has usually already been handed to us. Holding
 * the tail keeps that frame in our buffer where it can still be spliced. Five
 * seconds comfortably covers the drain plus flush jitter; the cost of a hard
 * crash is losing that much of the recording, which the close hook makes a
 * non-issue for any normal shutdown.
 */
constexpr double kHoldSeconds = 5.0;

CreateFileW_t g_realCreateFileW = nullptr;
WriteFile_t g_realWriteFile = nullptr;
SetFilePointer_t g_realSetFilePointer = nullptr;
DeleteFileW_t g_realDeleteFileW = nullptr;
CloseHandle_t g_realCloseHandle = nullptr;
SetFilePointerEx_t g_realSetFilePointerEx = nullptr;

/**
 * Total bytes we have inserted into the current recording.
 *
 * Tacview does not append blindly — it seeks to absolute offsets computed from
 * its OWN byte count, which knows nothing about our insertions. Left alone,
 * its next 16 KB block seeks back over what we wrote and overwrites it, which
 * is exactly why injected lines vanished from the finished file. Every
 * FILE_BEGIN seek on the recording handle is therefore shifted by this much.
 */
LONGLONG g_injectedTotal = 0;

/** Cost accounting, so the overhead claim can be measured rather than
 *  asserted: wall-clock spent inside our part of WriteFile, across a run. */
LONGLONG g_hookTicks = 0;
LONGLONG g_tickFreq = 0;
unsigned long g_writeCount = 0;
unsigned long g_injectCount = 0;

std::wstring g_logPath;

/** Diagnostics go to a file beside the DLL as well as the debugger — a DCS
 *  server is usually headless, so OutputDebugString alone is unobservable. */
void logLine(const char* msg) {
  OutputDebugStringA("[DKS-WX] ");
  OutputDebugStringA(msg);
  OutputDebugStringA("\n");

  if (g_logPath.empty()) return;
  HANDLE f = CreateFileW(g_logPath.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                         OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE) return;
  SYSTEMTIME t;
  GetLocalTime(&t);
  char line[512];
  int n = snprintf(line, sizeof(line), "%02d:%02d:%02d %s\r\n", t.wHour, t.wMinute, t.wSecond, msg);
  DWORD wrote = 0;
  WriteFile(f, line, static_cast<DWORD>(n), &wrote, nullptr);
  CloseHandle(f);
}

// ---------------------------------------------------------------------------
// IAT patching
// ---------------------------------------------------------------------------

/**
 * Redirect an imported function in `mod`'s import address table.
 *
 * Matches on function name across every import descriptor rather than
 * requiring a particular DLL name, because Windows may route these through
 * API-set stubs (api-ms-win-core-file-*) instead of KERNEL32 directly.
 */
bool patchImport(HMODULE mod, const char* funcName, void* replacement, void** original) {
  auto base = reinterpret_cast<BYTE*>(mod);
  auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
  if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
  auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
  if (nt->Signature != IMAGE_NT_SIGNATURE) return false;

  auto dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
  if (dir.VirtualAddress == 0) return false;

  auto desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress);
  for (; desc->Name != 0; ++desc) {
    if (desc->OriginalFirstThunk == 0 || desc->FirstThunk == 0) continue;
    auto names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + desc->OriginalFirstThunk);
    auto addrs = reinterpret_cast<IMAGE_THUNK_DATA*>(base + desc->FirstThunk);

    for (; names->u1.AddressOfData != 0; ++names, ++addrs) {
      if (names->u1.Ordinal & IMAGE_ORDINAL_FLAG) continue;
      auto import = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
      if (lstrcmpA(reinterpret_cast<const char*>(import->Name), funcName) != 0) continue;

      DWORD old = 0;
      if (!VirtualProtect(&addrs->u1.Function, sizeof(void*), PAGE_READWRITE, &old)) {
        return false;
      }
      if (original != nullptr) {
        *original = reinterpret_cast<void*>(addrs->u1.Function);
      }
      addrs->u1.Function = reinterpret_cast<ULONGLONG>(replacement);
      VirtualProtect(&addrs->u1.Function, sizeof(void*), old, &old);
      return true;
    }
  }
  return false;
}

// ---------------------------------------------------------------------------
// Wind ladder file
// ---------------------------------------------------------------------------

/**
 * Reload the ladder if the sampler has rewritten it. Format, one record per
 * line, metres and metres/second in DCS's frame:
 *
 *   qnh <hectopascals>
 *   w <altitudeM> <north> <up> <east>
 *
 * Anything unparseable is ignored rather than fatal — a half-written file just
 * means we keep the previous profile until the next poll.
 */
void refreshProfileIfChanged() {
  ULONGLONG now = GetTickCount64();
  if (now - g_lastProfilePoll < 2000) return;
  g_lastProfilePoll = now;

  WIN32_FILE_ATTRIBUTE_DATA attr = {};
  if (!GetFileAttributesExW(g_profilePath.c_str(), GetFileExInfoStandard, &attr)) return;
  if (CompareFileTime(&attr.ftLastWriteTime, &g_lastProfileWrite) == 0) return;

  HANDLE f = g_realCreateFileW(g_profilePath.c_str(), GENERIC_READ,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE) return;

  std::string text;
  char buf[4096];
  DWORD got = 0;
  while (ReadFile(f, buf, sizeof(buf), &got, nullptr) && got > 0) {
    text.append(buf, got);
  }
  CloseHandle(f);

  dkswx::WindProfile profile;
  double qnh = 0.0;
  size_t pos = 0;
  while (pos < text.size()) {
    size_t nl = text.find('\n', pos);
    std::string line = text.substr(pos, (nl == std::string::npos) ? nl : nl - pos);
    pos = (nl == std::string::npos) ? text.size() : nl + 1;

    if (line.rfind("w ", 0) == 0) {
      dkswx::WindSample s;
      if (sscanf_s(line.c_str() + 2, "%lf %lf %lf %lf", &s.altitudeM, &s.north, &s.up,
                   &s.east) == 4) {
        profile.add(s);
      }
    } else if (line.rfind("qnh ", 0) == 0) {
      qnh = atof(line.c_str() + 4);
    }
  }

  if (profile.empty()) {
    logLine("profile file present but contained no usable rungs");
    return;
  }
  g_lastProfileWrite = attr.ftLastWriteTime;
  logLine("wind profile reloaded");

  EnterCriticalSection(&g_lock);
  if (g_injector != nullptr) {
    g_injector->setProfile(profile);
    if (qnh > 0.0) g_injector->setQnhHpa(qnh);
  }
  LeaveCriticalSection(&g_lock);
}

/**
 * Read any engine events the sampler has appended since last time.
 *
 * The file is append-only and we track our own offset, so neither side needs
 * a lock: the sampler only ever adds whole lines at the end. A partial final
 * line (write interleaved with our read) is left for the next pass.
 */
void drainEventFile() {
  HANDLE f = g_realCreateFileW(g_eventsPath.c_str(), GENERIC_READ,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE) return;

  LARGE_INTEGER size;
  if (!GetFileSizeEx(f, &size) || size.QuadPart <= g_eventsOffset) {
    CloseHandle(f);
    return;
  }

  LARGE_INTEGER from;
  from.QuadPart = g_eventsOffset;
  if (!SetFilePointerEx(f, from, nullptr, FILE_BEGIN)) {
    CloseHandle(f);
    return;
  }

  std::string text;
  char buf[8192];
  DWORD got = 0;
  while (ReadFile(f, buf, sizeof(buf), &got, nullptr) && got > 0) text.append(buf, got);
  CloseHandle(f);

  size_t consumed = 0;
  size_t pos = 0;
  while (pos < text.size()) {
    size_t nl = text.find('\n', pos);
    if (nl == std::string::npos) break;  // partial line — wait for the rest
    std::string line = text.substr(pos, nl - pos);
    pos = nl + 1;
    consumed = pos;

    // KIND|time|drainTime|primaryUnit|secondaryUnit|text
    // Split the first six fields on '|', then take the REST of the line as
    // the last one. The trailing field is a player name, which is free text
    // and routinely contains pipes ("FIWB | Miyagi | 400"); splitting it would
    // truncate at the first one and the name would never match what Tacview
    // recorded. Fields 0-5 are safe to split because the sampler scrubs pipes
    // out of the only free-text field among them.
    std::string field[7];
    size_t fi = 0;
    size_t fs = 0;
    for (size_t k = 0; k <= line.size() && fi < 6; ++k) {
      if (k == line.size() || line[k] == '|') {
        field[fi++] = line.substr(fs, k - fs);
        fs = k + 1;
      }
    }
    if (fi == 6 && fs <= line.size()) {
      field[6] = line.substr(fs);
      fi = 7;
    }
    if (fi < 5) continue;

    dkswx::PendingEvent e;
    e.kind = field[0];
    e.time = atof(field[1].c_str());
    const double drainTime = atof(field[2].c_str());
    e.subjectUnit = field[3];
    e.secondaryUnit = field[4];
    e.text = field[5];
    e.altSubjectUnit = field[6];
    // One death fires Kill, Dead and UnitLost on the same tick; the ACMI
    // wants a single Destroyed. Nothing else repeats like that.
    e.dedupeBySubject = (e.kind == "Destroyed");
    // The classic TAKEOFF/LAND fallbacks overlap the runway pair by design,
    // so one departure can arrive twice from two different engine events.
    // (An earlier duplicate seen on a live server was NOT the engine
    // double-firing — it was the event file replaying a previous mission,
    // fixed by rotating it at mission load. This window is the remaining
    // guard.) Hits and shots legitimately repeat, so they are left alone.
    if (e.kind == "TakenOff" || e.kind == "Landed" || e.kind == "Bookmark") {
      e.dedupeWindowSec = 20.0;
    }
    if (e.kind.empty() || e.subjectUnit.empty()) continue;

    double streamNow = 0.0;
    EnterCriticalSection(&g_lock);
    if (g_injector != nullptr) {
      streamNow = g_injector->currentTime();
      g_injector->queueEvent(e);
    }
    LeaveCriticalSection(&g_lock);

    char msg[224];
    snprintf(msg, sizeof(msg),
             "queued %s eventT=%.2f drainT=%.2f (sampler lag %.2f) streamT=%.2f "
             "(handoff lag %.2f)",
             e.kind.c_str(), e.time, drainTime, drainTime - e.time, streamNow,
             streamNow - drainTime);
    logLine(msg);
  }
  g_eventsOffset += static_cast<LONGLONG>(consumed);
}

// ---------------------------------------------------------------------------
// Hooks
// ---------------------------------------------------------------------------

bool endsWithNoCase(const std::wstring& s, const wchar_t* suffix) {
  size_t n = wcslen(suffix);
  if (s.size() < n) return false;
  return _wcsicmp(s.c_str() + (s.size() - n), suffix) == 0;
}

/**
 * Keep Tacview's absolute seeks pointing at the byte it means, not the byte
 * that used to be there before we inserted anything.
 *
 * Only FILE_BEGIN needs shifting: FILE_CURRENT is relative to a pointer that
 * already reflects our writes, and FILE_END is relative to a length that does
 * too.
 */
void flushHeld(HANDLE h);

DWORD WINAPI hookedSetFilePointer(HANDLE h, LONG distance, PLONG distanceHigh,
                                  DWORD method) {
  if (h == g_acmiHandle) flushHeld(h);
  if (h != g_acmiHandle || method != FILE_BEGIN || g_injectedTotal == 0) {
    return g_realSetFilePointer(h, distance, distanceHigh, method);
  }

  LARGE_INTEGER target;
  target.LowPart = static_cast<DWORD>(distance);
  target.HighPart = (distanceHigh != nullptr) ? *distanceHigh : 0;
  if (distanceHigh == nullptr) target.QuadPart = distance;  // sign-extend

  target.QuadPart += g_injectedTotal;

  LONG high = target.HighPart;
  return g_realSetFilePointer(h, target.LowPart, (distanceHigh != nullptr) ? &high : nullptr,
                              method);
}

/**
 * Tacview deletes the plaintext journal once it has written the archive.
 * That journal is the ONLY copy of the enriched stream — the archive is built
 * from Tacview's own in-memory representation and never sees our insertions —
 * so we rename it aside rather than let it go.
 */
BOOL WINAPI hookedDeleteFileW(LPCWSTR name) {
  if (name == nullptr || !endsWithNoCase(name, L".txt.acmi")) {
    return g_realDeleteFileW(name);
  }

  // Tacview has finished its own archive and is discarding the journal. That
  // journal is the only copy of the enriched stream, so rebuild the archive
  // from it before letting it go.
  std::wstring journal(name);
  std::wstring zipPath = journal.substr(0, journal.size() - 9) + L".zip.acmi";

  size_t slash = journal.find_last_of(L"\\/");
  std::wstring leaf = (slash == std::wstring::npos) ? journal : journal.substr(slash + 1);
  std::string entryName;
  entryName.reserve(leaf.size());
  for (wchar_t c : leaf) entryName += (c < 128) ? static_cast<char>(c) : '_';

  LARGE_INTEGER z0, z1;
  QueryPerformanceCounter(&z0);
  dkswx::ZipResult r = dkswx::writeSingleEntryZip(journal, zipPath, entryName);
  QueryPerformanceCounter(&z1);

  if (g_tickFreq == 0) {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_tickFreq = f.QuadPart;
  }
  char cost[256];
  snprintf(cost, sizeof(cost),
           "cost: %lu writes, %lu injections, %lu events, %.1f ms total in hook "
           "(%.1f us/write), repack %.0f ms",
           g_writeCount, g_injectCount,
           g_injector ? g_injector->emittedEventCount() : 0UL,
           (g_hookTicks * 1000.0) / g_tickFreq,
           g_writeCount ? (g_hookTicks * 1000000.0) / g_tickFreq / g_writeCount : 0.0,
           ((z1.QuadPart - z0.QuadPart) * 1000.0) / g_tickFreq);
  logLine(cost);

  char msg[320];
  if (r.ok) {
    snprintf(msg, sizeof(msg), "rebuilt archive: %llu -> %llu bytes (%s)", r.sourceBytes,
             r.archiveBytes, r.compressed ? "deflate" : "STORED");
    logLine(msg);
    return g_realDeleteFileW(name);
  }

  // Never lose the enriched stream: if we could not repack it, keep the
  // journal so it can be recovered by hand.
  snprintf(msg, sizeof(msg), "archive rebuild FAILED (%s) - keeping journal", r.error.c_str());
  logLine(msg);
  std::wstring kept = journal + L".dkswx";
  g_realDeleteFileW(kept.c_str());
  if (MoveFileW(name, kept.c_str())) return TRUE;
  return g_realDeleteFileW(name);
}

HANDLE WINAPI hookedCreateFileW(LPCWSTR name, DWORD access, DWORD share,
                                LPSECURITY_ATTRIBUTES sec, DWORD disp, DWORD flags,
                                HANDLE tmpl) {
  HANDLE h = g_realCreateFileW(name, access, share, sec, disp, flags, tmpl);

  // Only the live plaintext recording. The .zip.acmi produced at mission end
  // must never be touched — injecting into compressed bytes would corrupt it.
  //
  // Accept any write-capable access mask, not GENERIC_WRITE specifically:
  // FILE_APPEND_DATA and FILE_WRITE_DATA are separate bits and a writer that
  // used either would otherwise slip past unnoticed.
  const DWORD kWritable = GENERIC_WRITE | GENERIC_ALL | FILE_WRITE_DATA | FILE_APPEND_DATA;
  if (h != INVALID_HANDLE_VALUE && name != nullptr && (access & kWritable) != 0 &&
      endsWithNoCase(name, L".txt.acmi")) {
    EnterCriticalSection(&g_lock);
    g_acmiHandle = h;
    g_injectedTotal = 0;
    g_eventsOffset = 0;
    if (g_injector != nullptr) g_injector->reset();
    LeaveCriticalSection(&g_lock);
    logLine("tracking new .txt.acmi recording");
  }
  return h;
}

BOOL WINAPI hookedWriteFile(HANDLE h, LPCVOID buffer, DWORD toWrite, LPDWORD written,
                            LPOVERLAPPED overlapped) {
  // Anything that is not the recording, or is asynchronous, passes straight
  // through — we only ever intervene on Tacview's own synchronous text writes.
  if (h != g_acmiHandle || overlapped != nullptr || buffer == nullptr || toWrite == 0) {
    return g_realWriteFile(h, buffer, toWrite, written, overlapped);
  }

  refreshProfileIfChanged();
  drainEventFile();

  LARGE_INTEGER t0;
  QueryPerformanceCounter(&t0);

  std::string ready;
  double frameTime = 0.0;
  size_t aircraft = 0;
  size_t held = 0;
  EnterCriticalSection(&g_lock);
  if (g_injector != nullptr) {
    g_injector->push(static_cast<const char*>(buffer), toWrite);
    ready = g_injector->takeReady(kHoldSeconds);
    frameTime = g_injector->currentTime();
    aircraft = g_injector->liveAircraftCount();
    held = g_injector->heldBytes();
    g_injectedTotal = static_cast<LONGLONG>(g_injector->injectedBytes());
  }
  LeaveCriticalSection(&g_lock);

  {
    LARGE_INTEGER t1;
    QueryPerformanceCounter(&t1);
    g_hookTicks += t1.QuadPart - t0.QuadPart;
  }

  static unsigned long writes = 0;
  static unsigned long long totalBytes = 0;
  ++writes;
  ++g_writeCount;
  totalBytes += toWrite;
  if (writes <= 8 || writes % 300 == 0) {
    char msg[256];
    snprintf(msg, sizeof(msg),
             "write#%lu len=%lu total=%llu t=%.2f ac=%zu flushed=%zu held=%zu",
             writes, toWrite, totalBytes, frameTime, aircraft, ready.size(), held);
    logLine(msg);
  }

  if (!ready.empty()) {
    DWORD w = 0;
    if (!g_realWriteFile(h, ready.data(), static_cast<DWORD>(ready.size()), &w, nullptr)) {
      if (written != nullptr) *written = 0;
      return FALSE;
    }
    ++g_injectCount;
  }

  // Tacview handed us every byte and we own them now, so report a full write.
  if (written != nullptr) *written = toWrite;
  return TRUE;
}

/** Push everything still held to disk. Must precede any seek, truncate or
 *  close, or the file on disk would be missing its tail. */
void flushHeld(HANDLE h) {
  std::string rest;
  EnterCriticalSection(&g_lock);
  if (g_injector != nullptr) {
    rest = g_injector->takeAll();
    g_injectedTotal = static_cast<LONGLONG>(g_injector->injectedBytes());
  }
  LeaveCriticalSection(&g_lock);

  if (rest.empty()) return;
  DWORD w = 0;
  g_realWriteFile(h, rest.data(), static_cast<DWORD>(rest.size()), &w, nullptr);
  char msg[96];
  snprintf(msg, sizeof(msg), "flushed %zu held bytes", rest.size());
  logLine(msg);
}

BOOL WINAPI hookedCloseHandle(HANDLE h) {
  if (h == g_acmiHandle) {
    flushHeld(h);
    g_acmiHandle = INVALID_HANDLE_VALUE;
  }
  return g_realCloseHandle(h);
}

BOOL WINAPI hookedSetFilePointerEx(HANDLE h, LARGE_INTEGER distance,
                                   PLARGE_INTEGER newPos, DWORD method) {
  if (h == g_acmiHandle) {
    flushHeld(h);
    if (method == FILE_BEGIN) distance.QuadPart += g_injectedTotal;
  }
  return g_realSetFilePointerEx(h, distance, newPos, method);
}

// ---------------------------------------------------------------------------
// Real module
// ---------------------------------------------------------------------------

std::wstring directoryOf(HMODULE mod) {
  wchar_t path[MAX_PATH] = {};
  GetModuleFileNameW(mod, path, MAX_PATH);
  std::wstring s(path);
  size_t slash = s.find_last_of(L"\\/");
  return (slash == std::wstring::npos) ? std::wstring() : s.substr(0, slash + 1);
}

bool loadReal(HMODULE self) {
  g_moduleDir = directoryOf(self);
  g_profilePath = g_moduleDir + L"dks-wx.profile";
  g_eventsPath = g_moduleDir + L"dks-wx.events";
  g_logPath = g_moduleDir + L"dks-wx.log";

  std::wstring realPath = g_moduleDir + L"tacview_real.dll";
  g_realModule = LoadLibraryW(realPath.c_str());
  if (g_realModule == nullptr) {
    logLine("FATAL: tacview_real.dll not found next to this DLL");
    return false;
  }

  g_realCreateFileW = reinterpret_cast<CreateFileW_t>(&CreateFileW);
  g_realWriteFile = reinterpret_cast<WriteFile_t>(&WriteFile);
  g_realSetFilePointer = reinterpret_cast<SetFilePointer_t>(&SetFilePointer);
  g_realDeleteFileW = reinterpret_cast<DeleteFileW_t>(&DeleteFileW);
  g_realCloseHandle = reinterpret_cast<CloseHandle_t>(&CloseHandle);
  g_realSetFilePointerEx = reinterpret_cast<SetFilePointerEx_t>(&SetFilePointerEx);

  bool a = patchImport(g_realModule, "CreateFileW", reinterpret_cast<void*>(&hookedCreateFileW),
                       reinterpret_cast<void**>(&g_realCreateFileW));
  bool b = patchImport(g_realModule, "WriteFile", reinterpret_cast<void*>(&hookedWriteFile),
                       reinterpret_cast<void**>(&g_realWriteFile));
  bool c = patchImport(g_realModule, "SetFilePointer",
                       reinterpret_cast<void*>(&hookedSetFilePointer),
                       reinterpret_cast<void**>(&g_realSetFilePointer));
  // Optional: older builds may not import DeleteFileW at all.
  patchImport(g_realModule, "DeleteFileW", reinterpret_cast<void*>(&hookedDeleteFileW),
              reinterpret_cast<void**>(&g_realDeleteFileW));
  // CloseHandle is the last chance to write the held tail.
  patchImport(g_realModule, "CloseHandle", reinterpret_cast<void*>(&hookedCloseHandle),
              reinterpret_cast<void**>(&g_realCloseHandle));
  patchImport(g_realModule, "SetFilePointerEx",
              reinterpret_cast<void*>(&hookedSetFilePointerEx),
              reinterpret_cast<void**>(&g_realSetFilePointerEx));
  if (!a || !b || !c) {
    logLine("WARNING: could not patch imports — recording continues unmodified");
  }
  return true;
}

HMODULE g_self = nullptr;

}  // namespace

// ---------------------------------------------------------------------------
// Exported entry point
// ---------------------------------------------------------------------------

extern "C" __declspec(dllexport) int luaopen_tacview(void* L) {
  static bool initialized = false;
  if (!initialized) {
    initialized = true;
    if (!loadReal(g_self)) return 0;
    g_injector = new dkswx::Injector();
    g_injector->setTraceSink(&logLine);
    logLine("proxy initialized");
  }
  if (g_realModule == nullptr) return 0;

  using LuaOpen_t = int (*)(void*);
  auto realOpen = reinterpret_cast<LuaOpen_t>(GetProcAddress(g_realModule, "luaopen_tacview"));
  if (realOpen == nullptr) {
    logLine("FATAL: tacview_real.dll has no luaopen_tacview");
    return 0;
  }
  return realOpen(L);
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    g_self = inst;
    InitializeCriticalSection(&g_lock);
    DisableThreadLibraryCalls(inst);
  }
  return TRUE;
}
