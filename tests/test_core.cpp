#include "MediaUtils.h"
#include "ProcessRunner.h"
#include "RenderEngine.h"
#include "ToolManager.h"
#include "YtDlpUtils.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>

namespace fs = std::filesystem;
using namespace filmrender;

#define CHECK(expr)                                                              \
    do {                                                                         \
        if (!(expr)) {                                                           \
            std::cerr << "CHECK failed at line " << __LINE__ << ": " #expr "\n"; \
            std::exit(1);                                                        \
        }                                                                        \
    } while (0)

namespace {

void TestSanitizeBaseName() {
    CHECK(SanitizeBaseName("Chitose Tập 1 [abc]!") == "ChitoseTp1abc");
    CHECK(SanitizeBaseName("---___   ") == "video");
    CHECK(SanitizeBaseName("Episode0123") == "Episode0123");
}

void TestNaturalLess() {
    CHECK(NaturalLess("Tp1.mp4", "Tp2.mp4"));
    CHECK(NaturalLess("Tp2.mp4", "Tp10.mp4"));
    CHECK(NaturalLess("Tp10.mp4", "Tp135.mp4"));
    CHECK(!NaturalLess("Tp10.mp4", "Tp2.mp4"));
    CHECK(!NaturalLess("same.mp4", "same.mp4"));
    CHECK(NaturalLess("ep01.mp4", "ep001.mp4"));
}

void TestTimestampMultiplier() {
    CHECK(ComputeTimestampMultiplier(3.7) == "0.270270");
    CHECK(ComputeTimestampMultiplier(2.0) == "0.500000");
}

void TestFormatConcatLine() {
    const fs::path p = fs::temp_directory_path() / "it's a test" / "video.mp4";
    const std::string line = FormatConcatLine(p);
    CHECK(line.rfind("file '", 0) == 0);
    CHECK(line.find("it'\\''s a test/video.mp4'") != std::string::npos);
#ifndef _WIN32
    if (fs::temp_directory_path() == fs::path("/tmp")) {
        CHECK(line == "file '/tmp/it'\\''s a test/video.mp4'");
    }
#endif
}

void TestScanAndSanitize() {
    const fs::path tmpDir = fs::temp_directory_path() / "filmrender_unit_test_dir";
    std::error_code ec;
    fs::remove_all(tmpDir, ec);
    fs::create_directories(tmpDir);

    auto touch = [](const fs::path& p) {
        std::ofstream ofs(p);
        ofs << "dummy";
    };

    touch(tmpDir / "Ep 10!.mp4");
    touch(tmpDir / "Ep 2!.mp4");
    touch(tmpDir / "Ep-2!.MP4");
    touch(tmpDir / "final_output.mp4");
    touch(tmpDir / "track1.mp3");

    const fs::path outPath = RenderEngine::ResolveOutputPath(tmpDir, "final_output.mp4");

    // ScanVideoFiles excludes final_output.mp4 and sorts naturally
    auto videos = ScanVideoFiles(tmpDir, outPath, true);
    CHECK(videos.size() == 3);
    CHECK(videos[0].filename == "Ep 2!.mp4");
    CHECK(videos[1].filename == "Ep 10!.mp4");
    CHECK(videos[2].filename == "Ep-2!.MP4");

    // SanitizeVideoFiles renames collisions with _1 and leaves final_output.mp4 untouched
    std::vector<std::string> logs;
    std::string err;
    auto renamed = SanitizeVideoFiles(
        tmpDir,
        outPath,
        [&logs](const std::string& msg) { logs.push_back(msg); },
        &err);
    CHECK(err.empty());
    CHECK(renamed.size() == 3);
    CHECK(fs::exists(tmpDir / "final_output.mp4"));
    CHECK(fs::exists(tmpDir / "Ep2.mp4"));
    CHECK(fs::exists(tmpDir / "Ep2_1.MP4") || fs::exists(tmpDir / "Ep2.MP4"));
    CHECK(fs::exists(tmpDir / "Ep10.mp4"));

    auto audio = ScanAudioFiles(tmpDir, false);
    CHECK(audio.size() == 1);
    CHECK(audio[0].filename() == "track1.mp3");

    fs::remove_all(tmpDir, ec);
}

void TestProcessRunner() {
    ProcessRunner runner;
    int exitCode = -1;
    std::string err;
    std::vector<std::string> lines;

#ifdef _WIN32
    const bool ok = runner.Run(
        "cmd.exe",
        {"/c", "echo", "hello_filmrender"},
        [&lines](const std::string& line, bool /*isProgress*/) { lines.push_back(line); },
        exitCode,
        err);
#else
    const bool ok = runner.Run(
        "/bin/sh",
        {"-c", "echo hello_filmrender"},
        [&lines](const std::string& line, bool /*isProgress*/) { lines.push_back(line); },
        exitCode,
        err);
#endif
    CHECK(ok);
    CHECK(exitCode == 0);
    CHECK(!lines.empty());
    CHECK(lines[0].find("hello_filmrender") != std::string::npos);

    // Non-existent executable should fail cleanly
    int badExit = 0;
    std::string badErr;
    const bool badOk = runner.Run(
        "filmrender_nonexistent_binary_12345",
        {},
        nullptr,
        badExit,
        badErr);
    CHECK(!badOk);
    CHECK(badExit != 0);
}

void TestEndToEndRenderWithFfmpeg() {
    ProcessRunner probe;
    int probeExit = -1;
    std::string probeErr;
    if (!probe.Run("ffmpeg", {"-version"}, nullptr, probeExit, probeErr)) {
        std::cout << "Skipping FFmpeg end-to-end test (ffmpeg not found in PATH).\n";
        return;
    }

    const fs::path baseTmp = fs::temp_directory_path() / "filmrender_e2e_test";
    const fs::path vidDir = baseTmp / "videos";
    const fs::path mp3Dir = baseTmp / "mp3";
    std::error_code ec;
    fs::remove_all(baseTmp, ec);
    fs::create_directories(vidDir);
    fs::create_directories(mp3Dir);

    int exitCode = -1;
    std::string err;
    CHECK(probe.Run("ffmpeg",
                    {"-y", "-hide_banner", "-loglevel", "error", "-f", "lavfi",
                     "-i", "color=c=blue:s=160x120:d=3", "-c:v", "libx264",
                     "-pix_fmt", "yuv420p", PathToUtf8(vidDir / "clip 1!.mp4")},
                    nullptr, exitCode, err));
    CHECK(probe.Run("ffmpeg",
                    {"-y", "-hide_banner", "-loglevel", "error", "-f", "lavfi",
                     "-i", "color=c=red:s=160x120:d=3", "-c:v", "libx264",
                     "-pix_fmt", "yuv420p", PathToUtf8(vidDir / "clip 2!.mp4")},
                    nullptr, exitCode, err));
    CHECK(probe.Run("ffmpeg",
                    {"-y", "-hide_banner", "-loglevel", "error", "-f", "lavfi",
                     "-i", "sine=frequency=440:duration=1", "-c:a", "libmp3lame",
                     PathToUtf8(mp3Dir / "bg1.mp3")},
                    nullptr, exitCode, err));

    RenderOptions opts;
    opts.videoDir = vidDir;
    opts.mp3Dir = mp3Dir;
    opts.outputFilenameOrPath = "final_output.mp4";
    opts.speedFactor = 2.0;
    opts.sanitizeFilenames = true;
    opts.includeAudio = true;
    opts.shuffleAudio = false;
    opts.loopAudio = true;
    opts.shortest = true;
    opts.overwriteOutput = true;
    opts.orderedVideos = {vidDir / "clip 1!.mp4", vidDir / "clip 2!.mp4"};

    std::vector<std::string> logs;
    RenderCallbacks cbs;
    cbs.onLog = [&logs](const std::string& line) { logs.push_back(line); };

    RenderEngine engine;
    const RenderResult res = engine.Run(opts, cbs);
    CHECK(res.success);
    CHECK(!res.cancelled);
    CHECK(fs::exists(vidDir / "final_output.mp4"));
    CHECK(fs::file_size(vidDir / "final_output.mp4") > 0);
    CHECK(fs::exists(vidDir / "clip1.mp4"));
    CHECK(fs::exists(vidDir / "clip2.mp4"));

    // Verify the 1s MP3 looped across the full ~3s sped-up video (not cut at 1s).
    std::string durStr;
    if (probe.Run("ffprobe",
                  {"-v", "error", "-show_entries", "format=duration",
                   "-of", "default=noprint_wrappers=1:nokey=1",
                   PathToUtf8(vidDir / "final_output.mp4")},
                  [&durStr](const std::string& line, bool /*isProgress*/) {
                      if (!line.empty() && durStr.empty()) {
                          durStr = line;
                      }
                  },
                  exitCode, err)) {
        const double dur = std::stod(durStr);
        CHECK(dur >= 2.8);
    }

    fs::remove_all(baseTmp, ec);
}

void TestToolManager() {
    CHECK(!ToolManager::BinaryName(ToolId::Ffmpeg).empty());
    CHECK(!ToolManager::BinaryName(ToolId::YtDlp).empty());
    CHECK(!ToolManager::BinaryName(ToolId::Bun).empty());

    CHECK(!ToolManager::DownloadUrl(ToolId::Ffmpeg).empty());
    CHECK(!ToolManager::DownloadUrl(ToolId::YtDlp).empty());
    CHECK(!ToolManager::DownloadUrl(ToolId::Bun).empty());

    const fs::path toolsDir = fs::temp_directory_path() / "filmrender_tools_unit_test";
    std::error_code ec;
    fs::remove_all(toolsDir, ec);
    fs::create_directories(toolsDir);

    const fs::path dummyYtDlp = toolsDir / Utf8ToPath(ToolManager::BinaryName(ToolId::YtDlp));
    {
        std::ofstream ofs(dummyYtDlp);
        ofs << "#!/bin/sh\necho ok\n";
    }
#ifndef _WIN32
    fs::permissions(dummyYtDlp,
                    fs::perms::owner_read | fs::perms::owner_write | fs::perms::owner_exec,
                    fs::perm_options::replace, ec);
#endif

    const ToolStatus found = ToolManager::FindTool(ToolId::YtDlp, toolsDir);
    CHECK(found.IsFound());
    CHECK(found.location == ToolLocation::ToolsDir);

    const ToolStatus customMissing = ToolManager::FindTool(
        ToolId::Ffmpeg, toolsDir, "/nonexistent/path/ffmpeg_xyz_999");
    CHECK(customMissing.location != ToolLocation::CustomPath);

    fs::remove_all(toolsDir, ec);
}

void TestYtDlpUtils() {
    CHECK(SanitizeDownloadFilename("Artist / Track: \"Best\"?.mp3") == "Artist Track Best");
    CHECK(SanitizeDownloadFilename("  ///... ") == "audio");

    // Flat playlist JSON line with thumbnails array & channel
    const std::string json1 =
        R"({"id":"dQw4w9WgXcQ","title":"Song \u0026 Video","channel":"Sample Channel","url":"https://www.youtube.com/watch?v=dQw4w9WgXcQ","thumbnails":[{"url":"https://i.ytimg.com/vi/dQw4w9WgXcQ/hqdefault.webp","width":480},{"url":"https://i.ytimg.com/vi/dQw4w9WgXcQ/mqdefault.jpg","width":320}]})";
    YtVideoEntry entry1;
    CHECK(ParseYtDlpJsonLine(json1, entry1));
    CHECK(entry1.id == "dQw4w9WgXcQ");
    CHECK(entry1.title == "Song & Video");
    CHECK(entry1.channel == "Sample Channel");
    CHECK(entry1.thumbnailUrl == "https://i.ytimg.com/vi/dQw4w9WgXcQ/mqdefault.jpg");
    CHECK(entry1.suggestedFilename == "Song & Video [dQw4w9WgXcQ]");

    // Playlist wrapper entry should be skipped
    YtVideoEntry wrapper;
    CHECK(!ParseYtDlpJsonLine(R"({"_type":"playlist","id":"PL123","title":"Mix"})", wrapper));

    // Progress parsing
    double pct = 0.0;
    std::string status;
    CHECK(ParseYtDlpProgress("[download]  42.5% of   4.12MiB at  1.50MiB/s ETA 00:02", pct, status));
    CHECK(pct > 42.0 && pct < 43.0);
    CHECK(ParseYtDlpProgress("[ExtractAudio] Destination: out.mp3", pct, status));
    CHECK(pct == 100.0);
    CHECK(status == "Converting to MP3...");
}

} // namespace

int main() {
    TestSanitizeBaseName();
    TestNaturalLess();
    TestTimestampMultiplier();
    TestFormatConcatLine();
    TestScanAndSanitize();
    TestProcessRunner();
    TestEndToEndRenderWithFfmpeg();
    TestToolManager();
    TestYtDlpUtils();
    std::cout << "All FilmRender core tests passed!\n";
    return 0;
}
