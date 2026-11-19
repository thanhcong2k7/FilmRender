#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace filmrender {

struct VideoFileItem {
    std::filesystem::path path;
    std::string filename;
    std::uintmax_t sizeBytes = 0;
    bool selected = true;
};

using LogCallback = std::function<void(const std::string&)>;

/// Convert a filesystem path to a UTF-8 std::string across platforms.
std::string PathToUtf8(const std::filesystem::path& p);

/// Construct a filesystem path from a UTF-8 std::string across platforms.
std::filesystem::path Utf8ToPath(const std::string& utf8);

/// Case-insensitive extension check (e.g., ".mp4", ".mp3").
bool HasExtensionCaseInsensitive(const std::filesystem::path& p, const std::string& extWithDot);

/// Natural/version comparison (e.g., "Tp2" < "Tp10") similar to `LC_ALL=C sort -V`.
bool NaturalLess(const std::string& a, const std::string& b);

/// Keep only ASCII a-z, A-Z, 0-9; falls back to "video" if empty.
std::string SanitizeBaseName(const std::string& base);

/// Format a path for FFmpeg's concat demuxer (`file '...'` with escaped quotes).
std::string FormatConcatLine(const std::filesystem::path& p);

/// Format `1.0 / speedFactor` with 6 decimal places using C locale.
std::string ComputeTimestampMultiplier(double speedFactor);

/// Format file size in human-readable units (B, KB, MB, GB).
std::string FormatFileSize(std::uintmax_t bytes);

/// Check whether two paths refer to the same location (without throwing).
bool PathsEquivalent(const std::filesystem::path& a, const std::filesystem::path& b);

/// Scan depth-1 .mp4 files in `videoDir`, excluding `outputPath`.
std::vector<VideoFileItem> ScanVideoFiles(const std::filesystem::path& videoDir,
                                          const std::filesystem::path& outputPath,
                                          bool naturalSort = true);

/// Scan depth-1 .mp3 files in `mp3Dir`, optionally shuffling them.
std::vector<std::filesystem::path> ScanAudioFiles(const std::filesystem::path& mp3Dir,
                                                  bool shuffle = true);

/// Sanitize .mp4 filenames in `videoDir` (excluding `outputPath`) to ASCII alphanumeric.
/// Returns a map from old canonical/weakly-canonical path string to new path.
std::map<std::string, std::filesystem::path> SanitizeVideoFiles(
    const std::filesystem::path& videoDir,
    const std::filesystem::path& outputPath,
    const LogCallback& logCb,
    std::string* errorOut = nullptr);

} // namespace filmrender
