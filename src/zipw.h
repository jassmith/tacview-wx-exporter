// Minimal ZIP writer for the finished recording.
//
// Tacview builds its .zip.acmi from its own in-memory model, which never sees
// our injected lines — the enriched stream only exists in the plaintext
// journal it writes alongside and deletes at close. So we keep that journal
// and produce the archive ourselves.
//
// Compression comes from zlib, which DCS already ships and has loaded
// (bin\zlib1.dll). We bind to it at runtime rather than vendoring a copy, and
// because we declare `z_stream` ourselves we PROVE the ABI before trusting it:
// a one-time deflate/inflate round-trip on a known buffer. If that fails, the
// writer falls back to STORED entries rather than emitting a corrupt archive.

#pragma once

#include <string>

namespace dkswx {

struct ZipResult {
  bool ok = false;
  bool compressed = false;   // false => fell back to STORED
  unsigned long long sourceBytes = 0;
  unsigned long long archiveBytes = 0;
  std::string error;
};

/**
 * Write `zipPath` containing `sourcePath` stored under `entryName`.
 *
 * Streams the source in chunks, so peak memory is a fixed buffer rather than
 * the recording size — a long mission can be hundreds of megabytes.
 */
ZipResult writeSingleEntryZip(const std::wstring& sourcePath, const std::wstring& zipPath,
                              const std::string& entryName);

}  // namespace dkswx
