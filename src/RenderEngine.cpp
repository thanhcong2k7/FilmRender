#include "RenderEngine.h"

#include <chrono>
#include <fstream>
#include <random>
#include <sstream>

namespace fs = std::filesystem;

namespace filmrender {

namespace {

class TempDirGuard {
public:
    TempDirGuard() {
        std::error_code ec;
        const fs::path baseTmp = fs::temp_directory_path(ec);
        std::random_device rd;
        std::mt19937_64 gen(rd());
        for (int attempt = 0; attempt < 32; ++attempt) {
            const auto val = gen();
            const fs::path candidate = baseTmp / ("filmrender_" + std::to_string(val));
            if (fs::create_directory(candidate, ec) && !ec) {
                dir_ = candidate;
                return;
            }
        }
    }

    ~TempDirGuard() {
        if (!dir_.empty()) {
            std::error_code ec;
            fs::remove_all(dir_, ec);
        }
    }

    TempDirGuard(const TempDirGuard&) = delete;
    TempDirGuard& operator=(const TempDirGuard&) = delete;

    bool IsValid() const { return !dir_.empty(); }
    const fs::path& Path() const { return dir_; }

private:
    fs::path dir_;
};

bool WriteConcatFile(const fs::path& filePath,
                     const std::vector<fs::path>& mediaPaths,
                     std::string* errorOut) {
    std::ofstream ofs(filePath, std::ios::binary | std::ios::trunc);
    if (!ofs.is_open()) {
        if (errorOut) {
            *errorOut = "Failed to open temporary concat file: " + PathToUtf8(filePath);
        }
        return false;
    }
    for (const fs::path& p : mediaPaths) {
        ofs << FormatConcatLine(p) << '\n';
    }
    ofs.flush();
    if (!ofs) {
        if (errorOut) {
            *errorOut = "Failed to write temporary concat file: " + PathToUtf8(filePath);
        }
        return false;
    }
    return true;
}

std::vector<std::string> BuildAudioMergeArgs(const fs::path& audioListPath,
                                             const fs::path& mergedAudioPath) {
    return {
        "-y",
        "-hide_banner",
        "-loglevel", "error",
        "-f", "concat",
        "-safe", "0",
        "-i", PathToUtf8(audioListPath),
        "-vn",
        "-c:a", "copy",
        PathToUtf8(mergedAudioPath)
    };
}

std::vector<std::string> BuildRenderArgs(const RenderOptions& options,
                                         const fs::path& videoListPath,
                                         const fs::path& mergedAudioPath,
                                         const fs::path& outputPath) {
    const std::string tsMult = ComputeTimestampMultiplier(options.speedFactor);
    const std::string logLevel = options.logLevel.empty() ? "warning" : options.logLevel;

    std::vector<std::string> args;
    args.push_back(options.overwriteOutput ? "-y" : "-n");
    args.push_back("-hide_banner");
    args.push_back("-loglevel");
    args.push_back(logLevel);
    args.push_back("-stats");
    args.push_back("-f");
    args.push_back("concat");
    args.push_back("-safe");
    args.push_back("0");
    args.push_back("-i");
    args.push_back(PathToUtf8(videoListPath));

    if (options.includeAudio) {
        if (options.loopAudio) {
            args.push_back("-stream_loop");
            args.push_back("-1");
        }
        args.push_back("-i");
        args.push_back(PathToUtf8(mergedAudioPath));
        args.push_back("-map");
        args.push_back("0:v:0");
        args.push_back("-map");
        args.push_back("1:a:0");
        args.push_back("-c:v");
        args.push_back("copy");
        args.push_back("-c:a");
        args.push_back("copy");
    } else {
        args.push_back("-map");
        args.push_back("0:v:0");
        args.push_back("-c:v");
        args.push_back("copy");
        args.push_back("-an");
    }

    args.push_back("-bsf:v");
    args.push_back("setts=dts=DTS*" + tsMult + ":pts=PTS*" + tsMult);

    if (options.includeAudio && options.shortest) {
        args.push_back("-shortest");
    }

    args.push_back(PathToUtf8(outputPath));
    return args;
}

} // namespace

fs::path RenderEngine::ResolveOutputPath(const fs::path& videoDir,
                                         const std::string& outputFilenameOrPath) {
    std::string trimmed = outputFilenameOrPath;
    if (trimmed.empty()) {
        trimmed = "final_output.mp4";
    }
    fs::path out = Utf8ToPath(trimmed);
    if (out.is_relative() && !videoDir.empty()) {
        out = videoDir / out;
    }
    std::error_code ec;
    fs::path norm = fs::weakly_canonical(out, ec);
    return ec ? out.lexically_normal() : norm;
}

std::string RenderEngine::BuildCommandPreview(const RenderOptions& options) {
    const fs::path outputPath = ResolveOutputPath(options.videoDir, options.outputFilenameOrPath);
    const std::string exe = options.ffmpegExecutable.empty() ? "ffmpeg" : options.ffmpegExecutable;
    const fs::path dummyVideoList = "<temp>/video_list.txt";
    const fs::path dummyAudioList = "<temp>/audio_list.txt";
    const fs::path dummyMergedAudio = "<temp>/merged_audio.mp3";

    std::ostringstream oss;
    oss << "=== Render Pipeline Preview ===\n";
    oss << "Film Folder      : " << PathToUtf8(options.videoDir) << "\n";
    oss << "Output File      : " << PathToUtf8(outputPath) << "\n";
    oss << "Speed Factor     : " << options.speedFactor
        << "x (TS_MULT = " << ComputeTimestampMultiplier(options.speedFactor) << ")\n";
    oss << "Sanitize Names   : " << (options.sanitizeFilenames ? "Yes" : "No") << "\n";
    oss << "Queued Videos    : " << options.orderedVideos.size() << "\n\n";

    if (options.includeAudio) {
        oss << "[Step 1] Merge MP3 Audio (" << (options.shuffleAudio ? "shuffled" : "sorted") << "):\n";
        oss << "  " << ProcessRunner::FormatCommand(exe, BuildAudioMergeArgs(dummyAudioList, dummyMergedAudio)) << "\n\n";
    }

    oss << "[Step " << (options.includeAudio ? "2" : "1") << "] Concatenate, Speed Up, and Mux:\n";
    oss << "  " << ProcessRunner::FormatCommand(
               exe, BuildRenderArgs(options, dummyVideoList, dummyMergedAudio, outputPath))
        << "\n";

    return oss.str();
}

void RenderEngine::Cancel() {
    cancelled_.store(true);
    runner_.Cancel();
}

RenderResult RenderEngine::Run(const RenderOptions& options, const RenderCallbacks& callbacks) {
    cancelled_.store(false);
    RenderResult result;

    auto log = [&callbacks](const std::string& msg) {
        if (callbacks.onLog) {
            callbacks.onLog(msg);
        }
    };
    auto setPhase = [&callbacks](const std::string& phase) {
        if (callbacks.onPhase) {
            callbacks.onPhase(phase);
        }
    };
    auto setStats = [&callbacks](const std::string& stats) {
        if (callbacks.onStats) {
            callbacks.onStats(stats);
        }
    };

    const std::string ffmpegExe = options.ffmpegExecutable.empty() ? "ffmpeg" : options.ffmpegExecutable;

    std::error_code ec;
    if (options.videoDir.empty() || !fs::exists(options.videoDir, ec) || !fs::is_directory(options.videoDir, ec)) {
        result.message = "Error: Film directory not found: '" + PathToUtf8(options.videoDir) + "'";
        log(result.message);
        return result;
    }

    if (options.includeAudio) {
        if (options.mp3Dir.empty() || !fs::exists(options.mp3Dir, ec) || !fs::is_directory(options.mp3Dir, ec)) {
            result.message = "Error: MP3 directory not found: '" + PathToUtf8(options.mp3Dir) + "'";
            log(result.message);
            return result;
        }
    }

    if (options.speedFactor <= 0.0) {
        result.message = "Error: Speed factor must be greater than 0.";
        log(result.message);
        return result;
    }

    const fs::path videoDirAbs = fs::weakly_canonical(options.videoDir, ec);
    const fs::path resolvedVideoDir = ec ? fs::absolute(options.videoDir) : videoDirAbs;
    const fs::path outputPath = ResolveOutputPath(resolvedVideoDir, options.outputFilenameOrPath);
    result.outputPath = outputPath;

    if (!options.overwriteOutput && fs::exists(outputPath, ec)) {
        result.message = "Error: Output file already exists and overwrite is disabled: " + PathToUtf8(outputPath);
        log(result.message);
        return result;
    }

    TempDirGuard tmpDir;
    if (!tmpDir.IsValid()) {
        result.message = "Error: Could not create temporary working directory.";
        log(result.message);
        return result;
    }

    std::vector<fs::path> videoQueue = options.orderedVideos;

    // --- 1. Sanitize Video Filenames ---
    if (options.sanitizeFilenames) {
        setPhase("Sanitizing video filenames...");
        log("==================== Filename Sanitizer ====================");
        std::string err;
        auto renamedMap = SanitizeVideoFiles(resolvedVideoDir, outputPath, log, &err);
        if (!err.empty()) {
            result.message = "Error: " + err;
            log(result.message);
            return result;
        }
        if (renamedMap.empty()) {
            log("All video filenames are already sanitized.");
        } else {
            for (fs::path& vp : videoQueue) {
                std::error_code normEc;
                fs::path norm = fs::weakly_canonical(vp, normEc);
                const std::string key = PathToUtf8(normEc ? vp : norm);
                auto it = renamedMap.find(key);
                if (it != renamedMap.end()) {
                    vp = it->second;
                }
            }
            if (callbacks.onVideosRenamed) {
                callbacks.onVideosRenamed(renamedMap);
            }
        }
    }

    if (cancelled_.load()) {
        result.cancelled = true;
        result.message = "Render cancelled.";
        log(result.message);
        return result;
    }

    // --- 2. Build Video Concat List ---
    if (videoQueue.empty()) {
        auto scanned = ScanVideoFiles(resolvedVideoDir, outputPath, true);
        for (const auto& item : scanned) {
            videoQueue.push_back(item.path);
        }
    }

    if (videoQueue.empty()) {
        result.message = "Error: No source .mp4 files queued from '" + PathToUtf8(resolvedVideoDir) + "'.";
        log(result.message);
        return result;
    }

    const fs::path videoListPath = tmpDir.Path() / "video_list.txt";
    std::string writeErr;
    if (!WriteConcatFile(videoListPath, videoQueue, &writeErr)) {
        result.message = "Error: " + writeErr;
        log(result.message);
        return result;
    }

    log("Queued " + std::to_string(videoQueue.size()) + " videos:");
    for (std::size_t i = 0; i < videoQueue.size(); ++i) {
        log("  [" + std::to_string(i + 1) + "] " + PathToUtf8(videoQueue[i].filename()));
    }

    if (cancelled_.load()) {
        result.cancelled = true;
        result.message = "Render cancelled.";
        log(result.message);
        return result;
    }

    // --- 3. Merge Randomized Audio ---
    const fs::path audioListPath = tmpDir.Path() / "audio_list.txt";
    const fs::path mergedAudioPath = tmpDir.Path() / "merged_audio.mp3";

    if (options.includeAudio) {
        setPhase("Merging MP3 audio tracks...");
        log("==================== Media Renderer ====================");
        auto audioFiles = ScanAudioFiles(options.mp3Dir, options.shuffleAudio);
        if (audioFiles.empty()) {
            result.message = "Error: No .mp3 files found in '" + PathToUtf8(options.mp3Dir) + "'.";
            log(result.message);
            return result;
        }

        if (!WriteConcatFile(audioListPath, audioFiles, &writeErr)) {
            result.message = "Error: " + writeErr;
            log(result.message);
            return result;
        }

        log("Merging " + std::to_string(audioFiles.size()) +
            (options.shuffleAudio ? " randomized" : " sorted") + " MP3 files...");

        const auto audioArgs = BuildAudioMergeArgs(audioListPath, mergedAudioPath);
        log("> " + ProcessRunner::FormatCommand(ffmpegExe, audioArgs));

        int audioExit = -1;
        std::string audioErr;
        const bool audioOk = runner_.Run(
            ffmpegExe,
            audioArgs,
            [&log](const std::string& line, bool /*isProgress*/) {
                if (!line.empty()) {
                    log(line);
                }
            },
            audioExit,
            audioErr);

        if (cancelled_.load() || runner_.WasCancelled()) {
            result.cancelled = true;
            result.message = "Render cancelled during audio merge.";
            log(result.message);
            return result;
        }

        if (!audioOk) {
            result.message = "Audio merge failed: " + audioErr;
            log(result.message);
            return result;
        }
    } else {
        log("==================== Media Renderer ====================");
        log("Audio track disabled; rendering video only.");
    }

    if (cancelled_.load()) {
        result.cancelled = true;
        result.message = "Render cancelled.";
        log(result.message);
        return result;
    }

    // --- 4. Concatenate, Speed Up, and Mux ---
    setPhase("Rendering video with FFmpeg...");
    const std::string tsMult = ComputeTimestampMultiplier(options.speedFactor);
    log("Rendering " + PathToUtf8(outputPath) + " at " +
        std::to_string(options.speedFactor) + "x speed (TS_MULT=" + tsMult + ")...");

    const auto renderArgs = BuildRenderArgs(options, videoListPath, mergedAudioPath, outputPath);
    log("> " + ProcessRunner::FormatCommand(ffmpegExe, renderArgs));

    auto lastLoggedStats = std::chrono::steady_clock::now() - std::chrono::seconds(5);
    std::string lastStatsLine;

    int renderExit = -1;
    std::string renderErr;
    const bool renderOk = runner_.Run(
        ffmpegExe,
        renderArgs,
        [&log, &setStats, &lastLoggedStats, &lastStatsLine](const std::string& line, bool isProgress) {
            if (line.empty()) {
                return;
            }
            if (isProgress) {
                setStats(line);
                lastStatsLine = line;
                const auto now = std::chrono::steady_clock::now();
                if (now - lastLoggedStats >= std::chrono::seconds(2)) {
                    log(line);
                    lastLoggedStats = now;
                }
            } else {
                log(line);
            }
        },
        renderExit,
        renderErr);

    if (!lastStatsLine.empty()) {
        setStats(lastStatsLine);
        log(lastStatsLine);
    }

    if (cancelled_.load() || runner_.WasCancelled()) {
        result.cancelled = true;
        result.message = "Render cancelled.";
        log(result.message);
        return result;
    }

    if (!renderOk) {
        result.message = "Render failed: " + renderErr;
        log(result.message);
        return result;
    }

    result.success = true;
    result.message = "Done! Saved to: " + PathToUtf8(outputPath);
    log(result.message);
    return result;
}

} // namespace filmrender
