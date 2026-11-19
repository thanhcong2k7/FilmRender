#pragma once

#include <string>
#include <vector>

namespace filmrender {

struct YtVideoEntry {
    std::string id;
    std::string title;
    std::string channel;
    std::string webpageUrl;
    std::string thumbnailUrl;
    std::string suggestedFilename; // Filename stem without .mp3
};

/// Sanitize a filename stem for saving across Linux/macOS/Windows.
/// Strips path separators, forbidden characters, and optional trailing ".mp3".
std::string SanitizeDownloadFilename(const std::string& rawName);

/// Parse a single JSON line emitted by `yt-dlp --flat-playlist --dump-json`.
bool ParseYtDlpJsonLine(const std::string& jsonLine, YtVideoEntry& out);

/// Parse progress percentage and status message from a `yt-dlp --newline` output line.
/// Returns true if progress or status was updated.
bool ParseYtDlpProgress(const std::string& line, double& percentOut, std::string& statusOut);

} // namespace filmrender
