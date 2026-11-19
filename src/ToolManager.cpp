#include "ToolManager.h"

#include <cstdlib>
#include <random>
#include <sstream>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace filmrender {

namespace {

class StagingDirGuard {
public:
    StagingDirGuard() {
        std::error_code ec;
        const fs::path base = fs::temp_directory_path(ec);
        std::random_device rd;
        std::mt19937_64 gen(rd());
        for (int attempt = 0; attempt < 32; ++attempt) {
            const fs::path cand = base / ("filmrender_tool_" + std::to_string(gen()));
            if (fs::create_directory(cand, ec) && !ec) {
                dir_ = cand;
                return;
            }
        }
    }

    ~StagingDirGuard() {
        if (!dir_.empty()) {
            std::error_code ec;
            fs::remove_all(dir_, ec);
        }
    }

    StagingDirGuard(const StagingDirGuard&) = delete;
    StagingDirGuard& operator=(const StagingDirGuard&) = delete;

    bool IsValid() const { return !dir_.empty(); }
    const fs::path& Path() const { return dir_; }

private:
    fs::path dir_;
};

bool IsExecutableFile(const fs::path& p) {
    std::error_code ec;
    if (!fs::exists(p, ec) || !fs::is_regular_file(p, ec)) {
        return false;
    }
#ifdef _WIN32
    return true;
#else
    return ::access(p.c_str(), X_OK) == 0;
#endif
}

std::string GetEnvVar(const char* name) {
#ifdef _WIN32
    char* buf = nullptr;
    size_t len = 0;
    if (_dupenv_s(&buf, &len, name) == 0 && buf != nullptr) {
        std::string val(buf);
        free(buf);
        return val;
    }
    return {};
#else
    const char* val = std::getenv(name);
    return val ? std::string(val) : std::string();
#endif
}

fs::path FindInPath(const std::string& binName) {
    const std::string pathEnv = GetEnvVar("PATH");
    if (pathEnv.empty()) {
        return {};
    }

#ifdef _WIN32
    const char sep = ';';
#else
    const char sep = ':';
#endif

    std::stringstream ss(pathEnv);
    std::string dir;
    while (std::getline(ss, dir, sep)) {
        if (dir.empty()) {
            continue;
        }
        const fs::path cand = Utf8ToPath(dir) / Utf8ToPath(binName);
        if (IsExecutableFile(cand)) {
            std::error_code ec;
            fs::path norm = fs::weakly_canonical(cand, ec);
            return ec ? cand : norm;
        }
    }
    return {};
}

bool MakeExecutable(const fs::path& p, std::string& errorOut) {
#ifdef _WIN32
    (void)p;
    (void)errorOut;
    return true;
#else
    std::error_code ec;
    fs::permissions(
        p,
        fs::perms::owner_read | fs::perms::owner_write | fs::perms::owner_exec |
            fs::perms::group_read | fs::perms::group_exec |
            fs::perms::others_read | fs::perms::others_exec,
        fs::perm_options::replace,
        ec);
    if (ec) {
        errorOut = "Failed to set executable permissions on " + PathToUtf8(p) + ": " + ec.message();
        return false;
    }
    return true;
#endif
}

fs::path FindExtractedBinary(const fs::path& root, const std::string& filename) {
    std::error_code ec;
    for (const auto& entry : fs::recursive_directory_iterator(root, ec)) {
        if (ec) {
            break;
        }
        std::error_code entryEc;
        if (!entry.is_regular_file(entryEc)) {
            continue;
        }
        if (PathToUtf8(entry.path().filename()) == filename) {
            return entry.path();
        }
    }
    return {};
}

bool EndsWith(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() &&
           s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

#ifdef _WIN32
std::string EscapePowerShellSingleQuoted(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char ch : s) {
        if (ch == '\'') {
            out += "''";
        } else {
            out.push_back(ch);
        }
    }
    return out;
}
#endif

} // namespace

std::string ToolStatus::LocationLabel() const {
    switch (location) {
    case ToolLocation::ToolsDir:
        return "tools/";
    case ToolLocation::PathEnv:
        return "PATH";
    case ToolLocation::CustomPath:
        return "custom";
    case ToolLocation::NotFound:
    default:
        return "Missing";
    }
}

fs::path ToolManager::ResolveToolsDir(const fs::path& executablePath) {
    std::error_code ec;
    const fs::path cwd = fs::current_path(ec);
    if (!cwd.empty() && fs::exists(cwd / "tools", ec) && fs::is_directory(cwd / "tools", ec)) {
        return fs::weakly_canonical(cwd / "tools", ec);
    }

    fs::path exeDir = executablePath.empty() ? fs::path() : executablePath.parent_path();
    if (!exeDir.empty() && fs::exists(exeDir / "tools", ec) && fs::is_directory(exeDir / "tools", ec)) {
        return fs::weakly_canonical(exeDir / "tools", ec);
    }

    // When running from a build directory inside the project (e.g. build/FilmRender),
    // prefer <project_root>/tools so binaries stay in one predictable tools/ folder.
    if (!cwd.empty() && fs::exists(cwd / "CMakeLists.txt", ec)) {
        return cwd / "tools";
    }
    if (!exeDir.empty() && fs::exists(exeDir.parent_path() / "CMakeLists.txt", ec)) {
        return exeDir.parent_path() / "tools";
    }
    if (!exeDir.empty()) {
        return exeDir / "tools";
    }
    return cwd.empty() ? fs::path("tools") : (cwd / "tools");
}

std::string ToolManager::BinaryName(ToolId id) {
#ifdef _WIN32
    switch (id) {
    case ToolId::Ffmpeg:
        return "ffmpeg.exe";
    case ToolId::YtDlp:
        return "yt-dlp.exe";
    case ToolId::Bun:
        return "bun.exe";
    }
#else
    switch (id) {
    case ToolId::Ffmpeg:
        return "ffmpeg";
    case ToolId::YtDlp:
        return "yt-dlp";
    case ToolId::Bun:
        return "bun";
    }
#endif
    return "";
}

std::string ToolManager::DownloadUrl(ToolId id) {
#ifdef _WIN32
    switch (id) {
    case ToolId::YtDlp:
#if defined(_M_ARM64) || defined(__aarch64__)
        return "https://github.com/yt-dlp/yt-dlp/releases/latest/download/yt-dlp_arm64.exe";
#else
        return "https://github.com/yt-dlp/yt-dlp/releases/latest/download/yt-dlp.exe";
#endif
    case ToolId::Bun:
        return "https://github.com/oven-sh/bun/releases/latest/download/bun-windows-x64.zip";
    case ToolId::Ffmpeg:
        return "https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/ffmpeg-master-latest-win64-gpl.zip";
    }
#elif defined(__APPLE__)
    switch (id) {
    case ToolId::YtDlp:
        return "https://github.com/yt-dlp/yt-dlp/releases/latest/download/yt-dlp_macos";
    case ToolId::Bun:
#if defined(__aarch64__) || defined(__arm64__)
        return "https://github.com/oven-sh/bun/releases/latest/download/bun-darwin-aarch64.zip";
#else
        return "https://github.com/oven-sh/bun/releases/latest/download/bun-darwin-x64.zip";
#endif
    case ToolId::Ffmpeg:
        return "https://evermeet.cx/ffmpeg/get/zip";
    }
#else
    switch (id) {
    case ToolId::YtDlp:
#if defined(__aarch64__) || defined(__arm64__)
        return "https://github.com/yt-dlp/yt-dlp/releases/latest/download/yt-dlp_linux_aarch64";
#else
        return "https://github.com/yt-dlp/yt-dlp/releases/latest/download/yt-dlp_linux";
#endif
    case ToolId::Bun:
#if defined(__aarch64__) || defined(__arm64__)
        return "https://github.com/oven-sh/bun/releases/latest/download/bun-linux-aarch64.zip";
#else
        return "https://github.com/oven-sh/bun/releases/latest/download/bun-linux-x64.zip";
#endif
    case ToolId::Ffmpeg:
#if defined(__aarch64__) || defined(__arm64__)
        return "https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/ffmpeg-master-latest-linuxarm64-gpl.tar.xz";
#else
        return "https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/ffmpeg-master-latest-linux64-gpl.tar.xz";
#endif
    }
#endif
    return "";
}

ToolStatus ToolManager::FindTool(ToolId id,
                                 const fs::path& toolsDir,
                                 const std::string& customPath) {
    ToolStatus status;
    status.id = id;
    status.name = BinaryName(id);

    // 1. Custom path (for ffmpeg if user specified a non-default binary path)
    if (!customPath.empty() && customPath != "ffmpeg" && customPath != "ffmpeg.exe") {
        const fs::path p = Utf8ToPath(customPath);
        if (IsExecutableFile(p)) {
            status.location = ToolLocation::CustomPath;
            status.resolvedPath = p;
            return status;
        }
        const fs::path inPath = FindInPath(customPath);
        if (!inPath.empty()) {
            status.location = ToolLocation::CustomPath;
            status.resolvedPath = inPath;
            return status;
        }
    }

    // 2. Local tools/ directory (prioritized so downloaded latest binaries take effect immediately)
    if (!toolsDir.empty()) {
        const fs::path localBin = toolsDir / Utf8ToPath(status.name);
        if (IsExecutableFile(localBin)) {
            std::error_code ec;
            fs::path norm = fs::weakly_canonical(localBin, ec);
            status.location = ToolLocation::ToolsDir;
            status.resolvedPath = ec ? localBin : norm;
            return status;
        }
    }

    // 3. System PATH
    const fs::path pathBin = FindInPath(status.name);
    if (!pathBin.empty()) {
        status.location = ToolLocation::PathEnv;
        status.resolvedPath = pathBin;
        return status;
    }

    status.location = ToolLocation::NotFound;
    return status;
}

AllToolsStatus ToolManager::CheckAll(const fs::path& toolsDir,
                                     const std::string& customFfmpegPath) {
    AllToolsStatus all;
    all.toolsDir = toolsDir;
    all.ffmpeg = FindTool(ToolId::Ffmpeg, toolsDir, customFfmpegPath);
    all.ytDlp = FindTool(ToolId::YtDlp, toolsDir);
    all.bun = FindTool(ToolId::Bun, toolsDir);
    return all;
}

bool ToolManager::DownloadTool(ToolId id,
                               const fs::path& toolsDir,
                               ProcessRunner& runner,
                               const LogCallback& onLog,
                               std::string& errorOut) {
    auto log = [&onLog](const std::string& msg) {
        if (onLog && !msg.empty()) {
            onLog(msg);
        }
    };

    const std::string binName = BinaryName(id);
    const std::string url = DownloadUrl(id);
    if (url.empty()) {
        errorOut = "No download URL configured for " + binName;
        return false;
    }

    std::error_code ec;
    fs::create_directories(toolsDir, ec);
    if (ec) {
        errorOut = "Failed to create tools directory '" + PathToUtf8(toolsDir) + "': " + ec.message();
        return false;
    }

    StagingDirGuard staging;
    if (!staging.IsValid()) {
        errorOut = "Failed to create temporary download directory.";
        return false;
    }

    const bool isZip = EndsWith(url, ".zip");
    const bool isTarXz = EndsWith(url, ".tar.xz");
    const fs::path downloadDest = staging.Path() / (isZip ? "archive.zip" : (isTarXz ? "archive.tar.xz" : binName));

    log("Downloading " + binName + " from:");
    log("  " + url);

    int exitCode = -1;
    std::string runErr;
    bool downloaded = false;

    auto outCb = [&log](const std::string& line, bool isProgress) {
        if (!isProgress && !line.empty()) {
            log(line);
        }
    };

#ifdef _WIN32
    if (!FindInPath("curl.exe").empty()) {
        downloaded = runner.Run(
            "curl.exe",
            {"-fSL", "--connect-timeout", "20", "--retry", "2", "-o", PathToUtf8(downloadDest), url},
            outCb, exitCode, runErr);
    } else {
        const std::string psCmd =
            "Invoke-WebRequest -UseBasicParsing -Uri '" + EscapePowerShellSingleQuoted(url) +
            "' -OutFile '" + EscapePowerShellSingleQuoted(PathToUtf8(downloadDest)) + "'";
        downloaded = runner.Run(
            "powershell.exe",
            {"-NoProfile", "-ExecutionPolicy", "Bypass", "-Command", psCmd},
            outCb, exitCode, runErr);
    }
#else
    if (!FindInPath("curl").empty()) {
        downloaded = runner.Run(
            "curl",
            {"-fSL", "--connect-timeout", "20", "--retry", "2", "-o", PathToUtf8(downloadDest), url},
            outCb, exitCode, runErr);
    } else if (!FindInPath("wget").empty()) {
        downloaded = runner.Run(
            "wget",
            {"-q", "--show-progress", "-O", PathToUtf8(downloadDest), url},
            outCb, exitCode, runErr);
    } else {
        errorOut = "Neither 'curl' nor 'wget' is installed to download " + binName + ".";
        return false;
    }
#endif

    if (!downloaded) {
        errorOut = "Failed to download " + binName + ": " + runErr;
        return false;
    }

    const fs::path targetBin = toolsDir / Utf8ToPath(binName);

    if (!isZip && !isTarXz) {
        fs::copy_file(downloadDest, targetBin, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            errorOut = "Failed to install " + binName + ": " + ec.message();
            return false;
        }
    } else {
        const fs::path extractDir = staging.Path() / "extracted";
        fs::create_directories(extractDir, ec);
        if (ec) {
            errorOut = "Failed to create extraction directory: " + ec.message();
            return false;
        }

        log("Extracting " + binName + "...");
        bool extracted = false;

#ifdef _WIN32
        const std::string psExtract =
            "Expand-Archive -LiteralPath '" + EscapePowerShellSingleQuoted(PathToUtf8(downloadDest)) +
            "' -DestinationPath '" + EscapePowerShellSingleQuoted(PathToUtf8(extractDir)) + "' -Force";
        extracted = runner.Run(
            "powershell.exe",
            {"-NoProfile", "-ExecutionPolicy", "Bypass", "-Command", psExtract},
            outCb, exitCode, runErr);
#else
        if (isZip) {
            if (!FindInPath("unzip").empty()) {
                extracted = runner.Run(
                    "unzip",
                    {"-o", "-q", PathToUtf8(downloadDest), "-d", PathToUtf8(extractDir)},
                    outCb, exitCode, runErr);
            } else if (!FindInPath("python3").empty()) {
                extracted = runner.Run(
                    "python3",
                    {"-m", "zipfile", "-e", PathToUtf8(downloadDest), PathToUtf8(extractDir)},
                    outCb, exitCode, runErr);
            } else {
                errorOut = "'unzip' (or 'python3') is required to extract " + binName + ".";
                return false;
            }
        } else if (isTarXz) {
            extracted = runner.Run(
                "tar",
                {"-xf", PathToUtf8(downloadDest), "-C", PathToUtf8(extractDir)},
                outCb, exitCode, runErr);
        }
#endif

        if (!extracted) {
            errorOut = "Failed to extract " + binName + ": " + runErr;
            return false;
        }

        const fs::path foundBin = FindExtractedBinary(extractDir, binName);
        if (foundBin.empty()) {
            errorOut = "Downloaded archive did not contain '" + binName + "'.";
            return false;
        }

        fs::copy_file(foundBin, targetBin, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            errorOut = "Failed to install " + binName + " into tools/: " + ec.message();
            return false;
        }
    }

    if (!MakeExecutable(targetBin, errorOut)) {
        return false;
    }

    log("Installed " + binName + " -> " + PathToUtf8(targetBin));
    return true;
}

} // namespace filmrender
