#include "MediaUtils.h"

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <locale>
#include <random>
#include <sstream>

namespace fs = std::filesystem;

namespace filmrender {

std::string PathToUtf8(const fs::path& p) {
    return p.u8string();
}

fs::path Utf8ToPath(const std::string& utf8) {
    return fs::u8path(utf8);
}

static std::string ToLowerAscii(std::string s) {
    for (char& ch : s) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return s;
}

bool HasExtensionCaseInsensitive(const fs::path& p, const std::string& extWithDot) {
    if (!p.has_extension()) {
        return false;
    }
    return ToLowerAscii(PathToUtf8(p.extension())) == ToLowerAscii(extWithDot);
}

bool NaturalLess(const std::string& a, const std::string& b) {
    std::size_t i = 0;
    std::size_t j = 0;
    const std::size_t n = a.size();
    const std::size_t m = b.size();

    while (i < n && j < m) {
        const unsigned char ca = static_cast<unsigned char>(a[i]);
        const unsigned char cb = static_cast<unsigned char>(b[j]);

        if (std::isdigit(ca) && std::isdigit(cb)) {
            const std::size_t startA = i;
            const std::size_t startB = j;

            while (i < n && a[i] == '0') {
                ++i;
            }
            while (j < m && b[j] == '0') {
                ++j;
            }
            const std::size_t nzA = i;
            const std::size_t nzB = j;

            while (i < n && std::isdigit(static_cast<unsigned char>(a[i]))) {
                ++i;
            }
            while (j < m && std::isdigit(static_cast<unsigned char>(b[j]))) {
                ++j;
            }

            const std::size_t lenA = i - nzA;
            const std::size_t lenB = j - nzB;

            if (lenA != lenB) {
                return lenA < lenB;
            }

            for (std::size_t k = 0; k < lenA; ++k) {
                if (a[nzA + k] != b[nzB + k]) {
                    return static_cast<unsigned char>(a[nzA + k]) <
                           static_cast<unsigned char>(b[nzB + k]);
                }
            }

            // Equal numeric values: fewer leading zeros comes first.
            const std::size_t zerosA = nzA - startA;
            const std::size_t zerosB = nzB - startB;
            if (zerosA != zerosB) {
                return zerosA < zerosB;
            }
        } else {
            if (ca != cb) {
                return ca < cb;
            }
            ++i;
            ++j;
        }
    }

    return i == n && j < m;
}

std::string SanitizeBaseName(const std::string& base) {
    std::string clean;
    clean.reserve(base.size());
    for (unsigned char ch : base) {
        if ((ch >= 'a' && ch <= 'z') ||
            (ch >= 'A' && ch <= 'Z') ||
            (ch >= '0' && ch <= '9')) {
            clean.push_back(static_cast<char>(ch));
        }
    }
    if (clean.empty()) {
        return "video";
    }
    return clean;
}

std::string FormatConcatLine(const fs::path& p) {
    std::error_code ec;
    fs::path absPath = fs::weakly_canonical(p, ec);
    if (ec) {
        absPath = fs::absolute(p, ec);
    }
    if (ec) {
        absPath = p;
    }

    const std::string raw = absPath.generic_u8string();
    std::string escaped;
    escaped.reserve(raw.size() + 8);
    for (char ch : raw) {
        if (ch == '\'') {
            escaped += "'\\''";
        } else {
            escaped.push_back(ch);
        }
    }
    return "file '" + escaped + "'";
}

std::string ComputeTimestampMultiplier(double speedFactor) {
    if (speedFactor <= 0.0) {
        speedFactor = 1.0;
    }
    std::ostringstream oss;
    oss.imbue(std::locale::classic());
    oss << std::fixed << std::setprecision(6) << (1.0 / speedFactor);
    return oss.str();
}

std::string FormatFileSize(std::uintmax_t bytes) {
    static const char* kUnits[] = {"B", "KB", "MB", "GB", "TB"};
    double value = static_cast<double>(bytes);
    int unitIdx = 0;
    while (value >= 1024.0 && unitIdx < 4) {
        value /= 1024.0;
        ++unitIdx;
    }
    std::ostringstream oss;
    oss.imbue(std::locale::classic());
    if (unitIdx == 0) {
        oss << bytes << " B";
    } else {
        oss << std::fixed << std::setprecision(1) << value << " " << kUnits[unitIdx];
    }
    return oss.str();
}

static fs::path NormalizePath(const fs::path& p) {
    std::error_code ec;
    fs::path res = fs::weakly_canonical(p, ec);
    if (ec) {
        res = fs::absolute(p, ec);
    }
    if (ec) {
        res = p.lexically_normal();
    }
    return res;
}

bool PathsEquivalent(const fs::path& a, const fs::path& b) {
    if (a.empty() || b.empty()) {
        return false;
    }
    std::error_code ec;
    if (fs::exists(a, ec) && fs::exists(b, ec)) {
        if (fs::equivalent(a, b, ec) && !ec) {
            return true;
        }
    }
    return NormalizePath(a) == NormalizePath(b);
}

std::vector<VideoFileItem> ScanVideoFiles(const fs::path& videoDir,
                                          const fs::path& outputPath,
                                          bool naturalSort) {
    std::vector<VideoFileItem> items;
    std::error_code ec;
    if (!fs::exists(videoDir, ec) || !fs::is_directory(videoDir, ec)) {
        return items;
    }

    for (const auto& entry : fs::directory_iterator(videoDir, ec)) {
        if (ec) {
            break;
        }
        std::error_code entryEc;
        if (!entry.is_regular_file(entryEc)) {
            continue;
        }
        const fs::path& p = entry.path();
        if (!HasExtensionCaseInsensitive(p, ".mp4")) {
            continue;
        }
        if (!outputPath.empty() && PathsEquivalent(p, outputPath)) {
            continue;
        }

        VideoFileItem item;
        item.path = NormalizePath(p);
        item.filename = PathToUtf8(p.filename());
        item.sizeBytes = entry.file_size(entryEc);
        if (entryEc) {
            item.sizeBytes = 0;
        }
        item.selected = true;
        items.push_back(std::move(item));
    }

    std::sort(items.begin(), items.end(), [naturalSort](const VideoFileItem& lhs, const VideoFileItem& rhs) {
        if (naturalSort) {
            return NaturalLess(lhs.filename, rhs.filename);
        }
        return lhs.filename < rhs.filename;
    });

    return items;
}

std::vector<fs::path> ScanAudioFiles(const fs::path& mp3Dir, bool shuffle) {
    std::vector<fs::path> files;
    std::error_code ec;
    if (!fs::exists(mp3Dir, ec) || !fs::is_directory(mp3Dir, ec)) {
        return files;
    }

    for (const auto& entry : fs::directory_iterator(mp3Dir, ec)) {
        if (ec) {
            break;
        }
        std::error_code entryEc;
        if (!entry.is_regular_file(entryEc)) {
            continue;
        }
        const fs::path& p = entry.path();
        if (!HasExtensionCaseInsensitive(p, ".mp3")) {
            continue;
        }
        files.push_back(NormalizePath(p));
    }

    std::sort(files.begin(), files.end(), [](const fs::path& a, const fs::path& b) {
        return PathToUtf8(a.filename()) < PathToUtf8(b.filename());
    });

    if (shuffle && files.size() > 1) {
        std::random_device rd;
        std::mt19937 gen(rd());
        std::shuffle(files.begin(), files.end(), gen);
    }

    return files;
}

std::map<std::string, fs::path> SanitizeVideoFiles(
    const fs::path& videoDir,
    const fs::path& outputPath,
    const LogCallback& logCb,
    std::string* errorOut) {
    std::map<std::string, fs::path> renamedMap;
    std::error_code ec;
    if (!fs::exists(videoDir, ec) || !fs::is_directory(videoDir, ec)) {
        if (errorOut) {
            *errorOut = "Film directory not found: " + PathToUtf8(videoDir);
        }
        return renamedMap;
    }

    const fs::path dirAbs = NormalizePath(videoDir);

    // Collect existing .mp4 files first so directory iteration isn't affected by renames.
    std::vector<fs::path> candidates;
    for (const auto& entry : fs::directory_iterator(dirAbs, ec)) {
        if (ec) {
            break;
        }
        std::error_code entryEc;
        if (!entry.is_regular_file(entryEc)) {
            continue;
        }
        const fs::path& p = entry.path();
        if (!HasExtensionCaseInsensitive(p, ".mp4")) {
            continue;
        }
        if (!outputPath.empty() && PathsEquivalent(p, outputPath)) {
            continue;
        }
        candidates.push_back(NormalizePath(p));
    }

    std::sort(candidates.begin(), candidates.end(), [](const fs::path& a, const fs::path& b) {
        return NaturalLess(PathToUtf8(a.filename()), PathToUtf8(b.filename()));
    });

    for (const fs::path& filepath : candidates) {
        const std::string ext = PathToUtf8(filepath.extension());
        const std::string base = PathToUtf8(filepath.stem());
        const std::string cleanBase = SanitizeBaseName(base);

        fs::path targetPath = dirAbs / Utf8ToPath(cleanBase + ext);
        if (!PathsEquivalent(filepath, targetPath)) {
            int counter = 1;
            while (fs::exists(targetPath, ec)) {
                targetPath = dirAbs / Utf8ToPath(cleanBase + "_" + std::to_string(counter) + ext);
                ++counter;
            }

            fs::rename(filepath, targetPath, ec);
            if (ec) {
                if (errorOut) {
                    *errorOut = "Failed to rename '" + PathToUtf8(filepath.filename()) +
                                "' -> '" + PathToUtf8(targetPath.filename()) +
                                "': " + ec.message();
                }
                return renamedMap;
            }

            const fs::path normTarget = NormalizePath(targetPath);
            renamedMap[PathToUtf8(filepath)] = normTarget;
            if (logCb) {
                logCb("renamed '" + PathToUtf8(filepath.filename()) + "' -> '" +
                      PathToUtf8(normTarget.filename()) + "'");
            }
        }
    }

    return renamedMap;
}

} // namespace filmrender
