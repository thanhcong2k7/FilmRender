#pragma once

#include "MediaUtils.h"
#include "ProcessRunner.h"

#include <atomic>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace filmrender {

struct RenderOptions {
    std::filesystem::path videoDir;
    std::filesystem::path mp3Dir;
    std::string outputFilenameOrPath = "final_output.mp4";
    std::string ffmpegExecutable = "ffmpeg";
    double speedFactor = 3.7;
    bool sanitizeFilenames = true;
    bool includeAudio = true;
    bool shuffleAudio = true;
    bool loopAudio = true;
    bool shortest = true;
    bool overwriteOutput = true;
    std::string logLevel = "warning"; // "error", "warning", "info", "verbose"
    std::vector<std::filesystem::path> orderedVideos;
};

struct RenderCallbacks {
    std::function<void(const std::string& line)> onLog;
    std::function<void(const std::string& statsLine)> onStats;
    std::function<void(const std::string& phaseText)> onPhase;
    std::function<void(const std::map<std::string, std::filesystem::path>& renamed)> onVideosRenamed;
};

struct RenderResult {
    bool success = false;
    bool cancelled = false;
    std::filesystem::path outputPath;
    std::string message;
};

class RenderEngine {
public:
    RenderEngine() = default;

    /// Resolve output path: if `outputFilenameOrPath` is relative, place it inside `videoDir`.
    static std::filesystem::path ResolveOutputPath(const std::filesystem::path& videoDir,
                                                   const std::string& outputFilenameOrPath);

    /// Build a preview string showing the FFmpeg commands that will be executed.
    static std::string BuildCommandPreview(const RenderOptions& options);

    /// Execute the full render pipeline (blocking; call on worker thread).
    RenderResult Run(const RenderOptions& options, const RenderCallbacks& callbacks);

    /// Cancel an active render job.
    void Cancel();

private:
    ProcessRunner runner_;
    std::atomic<bool> cancelled_{false};
};

} // namespace filmrender
