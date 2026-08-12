#include "zipw.h"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <vector>

namespace dkswx {
namespace {

// ---------------------------------------------------------------------------
// zlib, bound at runtime
// ---------------------------------------------------------------------------

/**
 * zlib's `z_stream` as laid out by an MSVC x64 build.
 *
 * The trap here is `uLong`: it is `unsigned long`, which is 32 bits on Windows
 * and 64 on Linux. Getting it wrong silently misaligns every field after
 * `total_in`. Rather than trust this declaration, `zlibUsable()` round-trips a
 * known buffer through deflate + inflate before we ever point it at a
 * recording.
 */
struct ZStream {
  const unsigned char* next_in;
  unsigned int avail_in;
  unsigned long total_in;
  unsigned char* next_out;
  unsigned int avail_out;
  unsigned long total_out;
  const char* msg;
  void* state;
  void* zalloc;
  void* zfree;
  void* opaque;
  int data_type;
  unsigned long adler;
  unsigned long reserved;
};

constexpr int kZOk = 0;
constexpr int kZStreamEnd = 1;
constexpr int kZNoFlush = 0;
constexpr int kZFinish = 4;
constexpr int kZDeflated = 8;
constexpr int kZDefaultStrategy = 0;
/** Negative window bits selects a raw deflate stream — what ZIP stores. */
constexpr int kRawWindowBits = -15;

using deflateInit2_t = int(__cdecl*)(ZStream*, int, int, int, int, int, const char*, int);
using deflate_t = int(__cdecl*)(ZStream*, int);
using deflateEnd_t = int(__cdecl*)(ZStream*);
using inflateInit2_t = int(__cdecl*)(ZStream*, int, const char*, int);
using inflate_t = int(__cdecl*)(ZStream*, int);
using inflateEnd_t = int(__cdecl*)(ZStream*);

struct Zlib {
  HMODULE mod = nullptr;
  deflateInit2_t deflateInit2 = nullptr;
  deflate_t deflate = nullptr;
  deflateEnd_t deflateEnd = nullptr;
  inflateInit2_t inflateInit2 = nullptr;
  inflate_t inflate = nullptr;
  inflateEnd_t inflateEnd = nullptr;
  bool verified = false;
  bool checked = false;
};

Zlib g_zlib;

bool bindZlib() {
  if (g_zlib.mod != nullptr) return true;
  // Already resident in the DCS process; GetModuleHandle avoids a second load
  // and avoids caring where it lives on disk.
  HMODULE m = GetModuleHandleW(L"zlib1.dll");
  if (m == nullptr) m = LoadLibraryW(L"zlib1.dll");
  if (m == nullptr) return false;

  g_zlib.deflateInit2 = reinterpret_cast<deflateInit2_t>(GetProcAddress(m, "deflateInit2_"));
  g_zlib.deflate = reinterpret_cast<deflate_t>(GetProcAddress(m, "deflate"));
  g_zlib.deflateEnd = reinterpret_cast<deflateEnd_t>(GetProcAddress(m, "deflateEnd"));
  g_zlib.inflateInit2 = reinterpret_cast<inflateInit2_t>(GetProcAddress(m, "inflateInit2_"));
  g_zlib.inflate = reinterpret_cast<inflate_t>(GetProcAddress(m, "inflate"));
  g_zlib.inflateEnd = reinterpret_cast<inflateEnd_t>(GetProcAddress(m, "inflateEnd"));

  if (g_zlib.deflateInit2 == nullptr || g_zlib.deflate == nullptr ||
      g_zlib.deflateEnd == nullptr || g_zlib.inflateInit2 == nullptr ||
      g_zlib.inflate == nullptr || g_zlib.inflateEnd == nullptr) {
    return false;
  }
  g_zlib.mod = m;
  return true;
}

/** Prove the ABI before trusting it with a recording. */
bool zlibUsable() {
  if (g_zlib.checked) return g_zlib.verified;
  g_zlib.checked = true;
  if (!bindZlib()) return false;

  // Compressible but not trivial, and long enough to exercise a real block.
  std::string src;
  for (int i = 0; i < 200; ++i) src += "0,QNH=1013.25\n5901,WindSpeed=2.21\n";

  std::vector<unsigned char> packed(src.size() + 1024);
  ZStream ds{};
  if (g_zlib.deflateInit2(&ds, 6, kZDeflated, kRawWindowBits, 8, kZDefaultStrategy, "1.2.11",
                          static_cast<int>(sizeof(ZStream))) != kZOk) {
    return false;
  }
  ds.next_in = reinterpret_cast<const unsigned char*>(src.data());
  ds.avail_in = static_cast<unsigned int>(src.size());
  ds.next_out = packed.data();
  ds.avail_out = static_cast<unsigned int>(packed.size());
  int rc = g_zlib.deflate(&ds, kZFinish);
  unsigned long packedLen = ds.total_out;
  unsigned long consumed = ds.total_in;
  g_zlib.deflateEnd(&ds);

  // Wrong struct layout shows up here as nonsense counters, not just a bad rc.
  if (rc != kZStreamEnd || consumed != src.size() || packedLen == 0 ||
      packedLen > packed.size()) {
    return false;
  }

  std::vector<unsigned char> back(src.size() + 16);
  ZStream is{};
  if (g_zlib.inflateInit2(&is, kRawWindowBits, "1.2.11",
                          static_cast<int>(sizeof(ZStream))) != kZOk) {
    return false;
  }
  is.next_in = packed.data();
  is.avail_in = static_cast<unsigned int>(packedLen);
  is.next_out = back.data();
  is.avail_out = static_cast<unsigned int>(back.size());
  rc = g_zlib.inflate(&is, kZFinish);
  unsigned long backLen = is.total_out;
  g_zlib.inflateEnd(&is);

  if (rc != kZStreamEnd || backLen != src.size()) return false;
  if (std::memcmp(back.data(), src.data(), src.size()) != 0) return false;

  g_zlib.verified = true;
  return true;
}

// ---------------------------------------------------------------------------
// CRC32 (our own table — no dependency on zlib's calling convention)
// ---------------------------------------------------------------------------

unsigned int g_crcTable[256];
bool g_crcReady = false;

void initCrc() {
  if (g_crcReady) return;
  for (unsigned int i = 0; i < 256; ++i) {
    unsigned int c = i;
    for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
    g_crcTable[i] = c;
  }
  g_crcReady = true;
}

unsigned int crcUpdate(unsigned int crc, const unsigned char* buf, size_t len) {
  crc = ~crc;
  for (size_t i = 0; i < len; ++i) crc = g_crcTable[(crc ^ buf[i]) & 0xFF] ^ (crc >> 8);
  return ~crc;
}

// ---------------------------------------------------------------------------
// Little-endian helpers
// ---------------------------------------------------------------------------

void put16(std::vector<unsigned char>& v, unsigned int x) {
  v.push_back(static_cast<unsigned char>(x & 0xFF));
  v.push_back(static_cast<unsigned char>((x >> 8) & 0xFF));
}

void put32(std::vector<unsigned char>& v, unsigned long long x) {
  for (int i = 0; i < 4; ++i) v.push_back(static_cast<unsigned char>((x >> (8 * i)) & 0xFF));
}

bool writeAll(HANDLE h, const void* data, size_t len) {
  const unsigned char* p = static_cast<const unsigned char*>(data);
  while (len > 0) {
    DWORD chunk = static_cast<DWORD>(len > 0x10000000 ? 0x10000000 : len);
    DWORD wrote = 0;
    if (!WriteFile(h, p, chunk, &wrote, nullptr) || wrote == 0) return false;
    p += wrote;
    len -= wrote;
  }
  return true;
}

void dosTimeNow(unsigned int* time, unsigned int* date) {
  SYSTEMTIME t;
  GetLocalTime(&t);
  *time = (t.wHour << 11) | (t.wMinute << 5) | (t.wSecond / 2);
  *date = ((t.wYear - 1980) << 9) | (t.wMonth << 5) | t.wDay;
}

}  // namespace

// ---------------------------------------------------------------------------
// Writer
// ---------------------------------------------------------------------------

ZipResult writeSingleEntryZip(const std::wstring& sourcePath, const std::wstring& zipPath,
                              const std::string& entryName) {
  ZipResult out;
  initCrc();

  HANDLE src = CreateFileW(sourcePath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (src == INVALID_HANDLE_VALUE) {
    out.error = "cannot open journal";
    return out;
  }
  // Tacview has just written this archive, and something else may still have
  // it open — observed for real when the export folder sits inside a file-sync
  // share, where the sync client grabs the new file immediately. Backup and
  // antivirus software does the same. Retry briefly rather than give up and
  // leave the unenriched archive in place.
  HANDLE dst = INVALID_HANDLE_VALUE;
  DWORD lastErr = 0;
  for (int attempt = 0; attempt < 20; ++attempt) {
    dst = CreateFileW(zipPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                      CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (dst != INVALID_HANDLE_VALUE) break;
    lastErr = GetLastError();
    if (lastErr != ERROR_SHARING_VIOLATION && lastErr != ERROR_ACCESS_DENIED) break;
    Sleep(250);
  }
  if (dst == INVALID_HANDLE_VALUE) {
    CloseHandle(src);
    char buf[96];
    snprintf(buf, sizeof(buf), "cannot create archive (win32 %lu)", lastErr);
    out.error = buf;
    return out;
  }

  const bool useDeflate = zlibUsable();
  out.compressed = useDeflate;

  unsigned int dosTime = 0, dosDate = 0;
  dosTimeNow(&dosTime, &dosDate);

  // Local file header. Sizes are unknown while streaming, so bit 3 is set and
  // the real values follow the data in a descriptor.
  std::vector<unsigned char> hdr;
  put32(hdr, 0x04034b50);
  put16(hdr, 20);
  put16(hdr, 0x0008);
  put16(hdr, useDeflate ? 8 : 0);
  put16(hdr, dosTime);
  put16(hdr, dosDate);
  put32(hdr, 0);
  put32(hdr, 0);
  put32(hdr, 0);
  put16(hdr, static_cast<unsigned int>(entryName.size()));
  put16(hdr, 0);
  hdr.insert(hdr.end(), entryName.begin(), entryName.end());
  if (!writeAll(dst, hdr.data(), hdr.size())) {
    CloseHandle(src);
    CloseHandle(dst);
    out.error = "header write failed";
    return out;
  }

  std::vector<unsigned char> inBuf(256 * 1024);
  std::vector<unsigned char> outBuf(256 * 1024);
  unsigned int crc = 0;
  unsigned long long rawBytes = 0;
  unsigned long long compBytes = 0;
  bool failed = false;

  ZStream zs{};
  if (useDeflate) {
    if (g_zlib.deflateInit2(&zs, 6, kZDeflated, kRawWindowBits, 8, kZDefaultStrategy, "1.2.11",
                            static_cast<int>(sizeof(ZStream))) != kZOk) {
      failed = true;
    }
  }

  for (;;) {
    if (failed) break;
    DWORD got = 0;
    if (!ReadFile(src, inBuf.data(), static_cast<DWORD>(inBuf.size()), &got, nullptr)) {
      failed = true;
      break;
    }
    const bool last = (got == 0);
    if (got > 0) {
      crc = crcUpdate(crc, inBuf.data(), got);
      rawBytes += got;
    }

    if (!useDeflate) {
      if (got > 0 && !writeAll(dst, inBuf.data(), got)) failed = true;
      compBytes += got;
      if (last) break;
      continue;
    }

    zs.next_in = inBuf.data();
    zs.avail_in = got;
    for (;;) {
      zs.next_out = outBuf.data();
      zs.avail_out = static_cast<unsigned int>(outBuf.size());
      int rc = g_zlib.deflate(&zs, last ? kZFinish : kZNoFlush);
      size_t produced = outBuf.size() - zs.avail_out;
      if (produced > 0) {
        if (!writeAll(dst, outBuf.data(), produced)) {
          failed = true;
          break;
        }
        compBytes += produced;
      }
      if (last) {
        if (rc == kZStreamEnd) break;
        if (rc != kZOk) {
          failed = true;
          break;
        }
      } else if (zs.avail_in == 0) {
        break;
      }
    }
    if (last) break;
  }

  if (useDeflate) g_zlib.deflateEnd(&zs);
  CloseHandle(src);

  if (failed) {
    CloseHandle(dst);
    out.error = "compression or write failed";
    return out;
  }

  // Data descriptor, then the central directory.
  std::vector<unsigned char> tail;
  put32(tail, 0x08074b50);
  put32(tail, crc);
  put32(tail, compBytes);
  put32(tail, rawBytes);

  const unsigned long long cdOffset = hdr.size() + compBytes + 16;
  put32(tail, 0x02014b50);
  put16(tail, 20);
  put16(tail, 20);
  put16(tail, 0x0008);
  put16(tail, useDeflate ? 8 : 0);
  put16(tail, dosTime);
  put16(tail, dosDate);
  put32(tail, crc);
  put32(tail, compBytes);
  put32(tail, rawBytes);
  put16(tail, static_cast<unsigned int>(entryName.size()));
  put16(tail, 0);
  put16(tail, 0);
  put16(tail, 0);
  put16(tail, 0);
  put32(tail, 0);
  put32(tail, 0);  // local header offset — entry starts at 0
  tail.insert(tail.end(), entryName.begin(), entryName.end());

  const unsigned long long cdSize = 46 + entryName.size();
  put32(tail, 0x06054b50);
  put16(tail, 0);
  put16(tail, 0);
  put16(tail, 1);
  put16(tail, 1);
  put32(tail, cdSize);
  put32(tail, cdOffset);
  put16(tail, 0);

  if (!writeAll(dst, tail.data(), tail.size())) {
    CloseHandle(dst);
    out.error = "directory write failed";
    return out;
  }
  CloseHandle(dst);

  out.ok = true;
  out.sourceBytes = rawBytes;
  out.archiveBytes = cdOffset + cdSize + 22;
  return out;
}

}  // namespace dkswx
