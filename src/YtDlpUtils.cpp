#include "YtDlpUtils.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <variant>

namespace filmrender {

namespace {

// --- Minimal RFC-8259 JSON parser ---
struct JsonValue;
using JsonArray = std::vector<JsonValue>;
using JsonObject = std::map<std::string, JsonValue>;

struct JsonValue {
    std::variant<std::nullptr_t, bool, double, std::string, JsonArray, JsonObject> data{nullptr};

    bool IsObject() const { return std::holds_alternative<JsonObject>(data); }
    bool IsArray() const { return std::holds_alternative<JsonArray>(data); }
    bool IsString() const { return std::holds_alternative<std::string>(data); }
    bool IsNumber() const { return std::holds_alternative<double>(data); }

    const JsonObject* AsObject() const { return std::get_if<JsonObject>(&data); }
    const JsonArray* AsArray() const { return std::get_if<JsonArray>(&data); }
    const std::string* AsString() const { return std::get_if<std::string>(&data); }
    double AsNumber(double def = 0.0) const {
        if (const double* d = std::get_if<double>(&data)) {
            return *d;
        }
        return def;
    }

    std::string GetString(const std::string& key) const {
        const auto* obj = AsObject();
        if (!obj) {
            return {};
        }
        auto it = obj->find(key);
        if (it == obj->end()) {
            return {};
        }
        if (const auto* s = it->second.AsString()) {
            return *s;
        }
        return {};
    }

    const JsonValue* Get(const std::string& key) const {
        const auto* obj = AsObject();
        if (!obj) {
            return nullptr;
        }
        auto it = obj->find(key);
        return it == obj->end() ? nullptr : &it->second;
    }
};

class JsonParser {
public:
    explicit JsonParser(const std::string& src) : src_(src) {}

    bool Parse(JsonValue& out) {
        SkipWs();
        if (!ParseValue(out)) {
            return false;
        }
        SkipWs();
        return pos_ == src_.size();
    }

private:
    void SkipWs() {
        while (pos_ < src_.size() &&
               (src_[pos_] == ' ' || src_[pos_] == '\t' ||
                src_[pos_] == '\r' || src_[pos_] == '\n')) {
            ++pos_;
        }
    }

    bool ParseValue(JsonValue& out) {
        SkipWs();
        if (pos_ >= src_.size()) {
            return false;
        }
        const char ch = src_[pos_];
        if (ch == '{') {
            return ParseObject(out);
        }
        if (ch == '[') {
            return ParseArray(out);
        }
        if (ch == '"') {
            std::string s;
            if (!ParseString(s)) {
                return false;
            }
            out.data = std::move(s);
            return true;
        }
        if (ch == 't' && src_.compare(pos_, 4, "true") == 0) {
            pos_ += 4;
            out.data = true;
            return true;
        }
        if (ch == 'f' && src_.compare(pos_, 5, "false") == 0) {
            pos_ += 5;
            out.data = false;
            return true;
        }
        if (ch == 'n' && src_.compare(pos_, 4, "null") == 0) {
            pos_ += 4;
            out.data = nullptr;
            return true;
        }
        if (ch == '-' || std::isdigit(static_cast<unsigned char>(ch))) {
            return ParseNumber(out);
        }
        return false;
    }

    static void AppendUtf8(std::string& out, std::uint32_t cp) {
        if (cp <= 0x7F) {
            out.push_back(static_cast<char>(cp));
        } else if (cp <= 0x7FF) {
            out.push_back(static_cast<char>(0xC0 | ((cp >> 6) & 0x1F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp <= 0xFFFF) {
            out.push_back(static_cast<char>(0xE0 | ((cp >> 12) & 0x0F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp <= 0x10FFFF) {
            out.push_back(static_cast<char>(0xF0 | ((cp >> 18) & 0x07)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }

    bool ParseHex4(std::uint32_t& val) {
        if (pos_ + 4 > src_.size()) {
            return false;
        }
        val = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = src_[pos_++];
            val <<= 4;
            if (c >= '0' && c <= '9') {
                val |= static_cast<std::uint32_t>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                val |= static_cast<std::uint32_t>(10 + c - 'a');
            } else if (c >= 'A' && c <= 'F') {
                val |= static_cast<std::uint32_t>(10 + c - 'A');
            } else {
                return false;
            }
        }
        return true;
    }

    bool ParseString(std::string& out) {
        if (pos_ >= src_.size() || src_[pos_] != '"') {
            return false;
        }
        ++pos_;
        out.clear();
        while (pos_ < src_.size()) {
            const char ch = src_[pos_++];
            if (ch == '"') {
                return true;
            }
            if (ch == '\\') {
                if (pos_ >= src_.size()) {
                    return false;
                }
                const char esc = src_[pos_++];
                switch (esc) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    std::uint32_t cp = 0;
                    if (!ParseHex4(cp)) {
                        return false;
                    }
                    if (cp >= 0xD800 && cp <= 0xDBFF &&
                        pos_ + 2 <= src_.size() && src_[pos_] == '\\' && src_[pos_ + 1] == 'u') {
                        const std::size_t saved = pos_;
                        pos_ += 2;
                        std::uint32_t low = 0;
                        if (ParseHex4(low) && low >= 0xDC00 && low <= 0xDFFF) {
                            cp = 0x10000 + (((cp - 0xD800) << 10) | (low - 0xDC00));
                        } else {
                            pos_ = saved;
                        }
                    }
                    AppendUtf8(out, cp);
                    break;
                }
                default:
                    return false;
                }
            } else {
                out.push_back(ch);
            }
        }
        return false;
    }

    bool ParseNumber(JsonValue& out) {
        const char* start = src_.c_str() + pos_;
        char* endPtr = nullptr;
        const double val = std::strtod(start, &endPtr);
        if (endPtr == start) {
            return false;
        }
        pos_ = static_cast<std::size_t>(endPtr - src_.c_str());
        out.data = val;
        return true;
    }

    bool ParseArray(JsonValue& out) {
        if (src_[pos_] != '[') {
            return false;
        }
        ++pos_;
        JsonArray arr;
        SkipWs();
        if (pos_ < src_.size() && src_[pos_] == ']') {
            ++pos_;
            out.data = std::move(arr);
            return true;
        }
        while (pos_ < src_.size()) {
            JsonValue elem;
            if (!ParseValue(elem)) {
                return false;
            }
            arr.push_back(std::move(elem));
            SkipWs();
            if (pos_ >= src_.size()) {
                return false;
            }
            if (src_[pos_] == ']') {
                ++pos_;
                out.data = std::move(arr);
                return true;
            }
            if (src_[pos_] != ',') {
                return false;
            }
            ++pos_;
        }
        return false;
    }

    bool ParseObject(JsonValue& out) {
        if (src_[pos_] != '{') {
            return false;
        }
        ++pos_;
        JsonObject obj;
        SkipWs();
        if (pos_ < src_.size() && src_[pos_] == '}') {
            ++pos_;
            out.data = std::move(obj);
            return true;
        }
        while (pos_ < src_.size()) {
            SkipWs();
            std::string key;
            if (!ParseString(key)) {
                return false;
            }
            SkipWs();
            if (pos_ >= src_.size() || src_[pos_] != ':') {
                return false;
            }
            ++pos_;
            JsonValue val;
            if (!ParseValue(val)) {
                return false;
            }
            obj[std::move(key)] = std::move(val);
            SkipWs();
            if (pos_ >= src_.size()) {
                return false;
            }
            if (src_[pos_] == '}') {
                ++pos_;
                out.data = std::move(obj);
                return true;
            }
            if (src_[pos_] != ',') {
                return false;
            }
            ++pos_;
        }
        return false;
    }

    const std::string& src_;
    std::size_t pos_ = 0;
};

std::string ToLowerAscii(std::string s) {
    for (char& ch : s) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return s;
}

bool ContainsCaseInsensitive(const std::string& haystack, const std::string& needle) {
    return ToLowerAscii(haystack).find(ToLowerAscii(needle)) != std::string::npos;
}

bool IsLikelyImageUrlSupportedByWx(const std::string& url) {
    const std::string lower = ToLowerAscii(url);
    return lower.find(".jpg") != std::string::npos ||
           lower.find(".jpeg") != std::string::npos ||
           lower.find(".png") != std::string::npos;
}

bool LooksLikeYoutubeId(const std::string& id) {
    if (id.size() != 11) {
        return false;
    }
    for (unsigned char ch : id) {
        if (!std::isalnum(ch) && ch != '-' && ch != '_') {
            return false;
        }
    }
    return true;
}

std::string TrimString(const std::string& s) {
    std::size_t first = 0;
    while (first < s.size() && std::isspace(static_cast<unsigned char>(s[first]))) {
        ++first;
    }
    std::size_t last = s.size();
    while (last > first && std::isspace(static_cast<unsigned char>(s[last - 1]))) {
        --last;
    }
    return s.substr(first, last - first);
}

} // namespace

std::string SanitizeDownloadFilename(const std::string& rawName) {
    std::string work = TrimString(rawName);

    // Strip trailing .mp3 if user typed it
    if (work.size() >= 4) {
        const std::string tail = ToLowerAscii(work.substr(work.size() - 4));
        if (tail == ".mp3") {
            work.erase(work.size() - 4);
        }
    }

    std::string out;
    out.reserve(work.size());
    bool lastWasSpace = false;
    for (unsigned char ch : work) {
        if (ch < 0x20 || ch == 0x7F) {
            continue;
        }
        if (ch == '/' || ch == '\\' || ch == ':' || ch == '*' ||
            ch == '?' || ch == '"' || ch == '<' || ch == '>' || ch == '|') {
            if (!lastWasSpace && !out.empty()) {
                out.push_back(' ');
                lastWasSpace = true;
            }
            continue;
        }
        if (std::isspace(ch)) {
            if (!lastWasSpace && !out.empty()) {
                out.push_back(' ');
                lastWasSpace = true;
            }
            continue;
        }
        out.push_back(static_cast<char>(ch));
        lastWasSpace = false;
    }

    while (!out.empty() && (out.back() == ' ' || out.back() == '.')) {
        out.pop_back();
    }
    while (!out.empty() && (out.front() == ' ' || out.front() == '.')) {
        out.erase(out.begin());
    }

    if (out.size() > 180) {
        out.resize(180);
        while (!out.empty() && (static_cast<unsigned char>(out.back()) & 0xC0) == 0x80) {
            // Avoid splitting a multi-byte UTF-8 sequence
            out.pop_back();
        }
        if (!out.empty() && (static_cast<unsigned char>(out.back()) & 0x80) != 0) {
            out.pop_back();
        }
        while (!out.empty() && (out.back() == ' ' || out.back() == '.')) {
            out.pop_back();
        }
    }

    return out.empty() ? "audio" : out;
}

bool ParseYtDlpJsonLine(const std::string& jsonLine, YtVideoEntry& out) {
    out = YtVideoEntry{};
    JsonValue root;
    JsonParser parser(jsonLine);
    if (!parser.Parse(root) || !root.IsObject()) {
        return false;
    }

    // Skip playlist wrapper records that don't represent an actual video
    const std::string typeField = root.GetString("_type");
    if (typeField == "playlist" || typeField == "multi_video") {
        return false;
    }

    out.id = root.GetString("id");
    out.title = root.GetString("title");
    if (out.title.empty()) {
        out.title = root.GetString("fulltitle");
    }
    if (out.title.empty()) {
        out.title = out.id;
    }
    if (out.id.empty() && out.title.empty()) {
        return false;
    }

    out.channel = root.GetString("channel");
    if (out.channel.empty()) {
        out.channel = root.GetString("uploader");
    }
    if (out.channel.empty()) {
        out.channel = root.GetString("playlist_uploader");
    }
    if (out.channel.empty()) {
        out.channel = root.GetString("channel_id");
    }
    if (out.channel.empty()) {
        out.channel = "Unknown channel";
    }

    out.webpageUrl = root.GetString("webpage_url");
    if (out.webpageUrl.empty()) {
        out.webpageUrl = root.GetString("original_url");
    }
    if (out.webpageUrl.empty()) {
        const std::string rawUrl = root.GetString("url");
        if (rawUrl.find("://") != std::string::npos) {
            out.webpageUrl = rawUrl;
        }
    }
    if (out.webpageUrl.empty() && !out.id.empty()) {
        out.webpageUrl = "https://www.youtube.com/watch?v=" + out.id;
    }
    if (out.webpageUrl.empty()) {
        return false;
    }

    // Pick best wxImage-compatible thumbnail (JPEG/PNG preferred; YouTube mqdefault.jpg fallback)
    const std::string directThumb = root.GetString("thumbnail");
    if (!directThumb.empty() && IsLikelyImageUrlSupportedByWx(directThumb)) {
        out.thumbnailUrl = directThumb;
    }

    if (out.thumbnailUrl.empty()) {
        if (const JsonValue* thumbsVal = root.Get("thumbnails")) {
            if (const JsonArray* thumbs = thumbsVal->AsArray()) {
                double bestScore = -1e9;
                for (const JsonValue& t : *thumbs) {
                    const std::string tUrl = t.GetString("url");
                    if (tUrl.empty() || !IsLikelyImageUrlSupportedByWx(tUrl)) {
                        continue;
                    }
                    double width = 0.0;
                    if (const JsonValue* wv = t.Get("width")) {
                        width = wv->AsNumber(0.0);
                    }
                    // Prefer ~320px wide thumbnails for fast loading & crisp 96x54 display
                    const double score = (width > 0.0) ? -std::abs(width - 320.0) : -500.0;
                    if (score > bestScore) {
                        bestScore = score;
                        out.thumbnailUrl = tUrl;
                    }
                }
            }
        }
    }

    if (out.thumbnailUrl.empty()) {
        if (LooksLikeYoutubeId(out.id) ||
            ContainsCaseInsensitive(out.webpageUrl, "youtube.com") ||
            ContainsCaseInsensitive(out.webpageUrl, "youtu.be")) {
            if (!out.id.empty()) {
                out.thumbnailUrl = "https://i.ytimg.com/vi/" + out.id + "/mqdefault.jpg";
            }
        } else if (!directThumb.empty()) {
            out.thumbnailUrl = directThumb;
        }
    }

    const std::string rawStem = out.id.empty() ? out.title : (out.title + " [" + out.id + "]");
    out.suggestedFilename = SanitizeDownloadFilename(rawStem);
    return true;
}

bool ParseYtDlpProgress(const std::string& line, double& percentOut, std::string& statusOut) {
    if (line.empty()) {
        return false;
    }

    if (line.find("[download]") != std::string::npos) {
        if (line.find("has already been downloaded") != std::string::npos) {
            percentOut = 100.0;
            statusOut = "Already exists (skipped)";
            return true;
        }

        const std::size_t pctPos = line.find('%');
        if (pctPos != std::string::npos) {
            std::size_t numStart = pctPos;
            while (numStart > 0 &&
                   (std::isdigit(static_cast<unsigned char>(line[numStart - 1])) ||
                    line[numStart - 1] == '.')) {
                --numStart;
            }
            if (numStart < pctPos) {
                const double pct = std::strtod(line.substr(numStart, pctPos - numStart).c_str(), nullptr);
                percentOut = std::clamp(pct, 0.0, 100.0);

                // Extract compact status after "[download]"
                const std::size_t tagPos = line.find("[download]");
                statusOut = TrimString(line.substr(tagPos + 10));
                return true;
            }
        }
    }

    if (line.find("[ExtractAudio]") != std::string::npos) {
        percentOut = 100.0;
        statusOut = "Converting to MP3...";
        return true;
    }

    if (line.find("[Metadata]") != std::string::npos ||
        line.find("[EmbedThumbnail]") != std::string::npos ||
        line.find("[Fixup") != std::string::npos) {
        percentOut = 100.0;
        statusOut = "Finalizing MP3...";
        return true;
    }

    return false;
}

} // namespace filmrender
