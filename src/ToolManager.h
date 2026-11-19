#pragma once

#include "MediaUtils.h"
#include "ProcessRunner.h"

#include <filesystem>
#include <string>
#include <vector>

namespace filmrender {

enum class ToolId {
    Ffmpeg,
    YtDlp,
    Bun
};

enum class ToolLocation {
    NotFound,
    ToolsDir,
    PathEnv,
    CustomPath
};

struct ToolStatus {
    ToolId id = ToolId::Ffmpeg;
    std::string name;
    ToolLocation location = ToolLocation::NotFound;
    std::filesystem::path resolvedPath;

    bool IsFound() const { return location != ToolLocation::NotFound; }
    std::string LocationLabel() const;
};

struct AllToolsStatus {
    std::filesystem::path toolsDir;
    ToolStatus ffmpeg;
    ToolStatus ytDlp;
    ToolStatus bun;

    bool AllAvailable() const {
        return ffmpeg.IsFound() && ytDlp.IsFound() && bun.IsFound();
    }
};

class ToolManager {
public:
    /// Determine the `tools/` directory (next to project root when running from build/, else next to executable).
    static std::filesystem::path ResolveToolsDir(const std::filesystem::path& executablePath = {});

    /// Executable filename for the current OS (e.g. "yt-dlp" or "yt-dlp.exe").
    static std::string BinaryName(ToolId id);

    /// Official download URL for the latest binary on the current OS/architecture.
    static std::string DownloadUrl(ToolId id);

    /// Search `tools/` and environment `PATH` (plus optional custom path for ffmpeg).
    static ToolStatus FindTool(ToolId id,
                               const std::filesystem::path& toolsDir,
                               const std::string& customPath = {});

    /// Check all 3 tools (`ffmpeg`, `yt-dlp`, `bun`).
    static AllToolsStatus CheckAll(const std::filesystem::path& toolsDir,
                                   const std::string& customFfmpegPath = {});

    /// Download and install the latest binary for `id` into `toolsDir`.
    /// Blocking; run on a worker thread.
    static bool DownloadTool(ToolId id,
                             const std::filesystem::path& toolsDir,
                             ProcessRunner& runner,
                             const LogCallback& onLog,
                             std::string& errorOut);
};

} // namespace filmrender
