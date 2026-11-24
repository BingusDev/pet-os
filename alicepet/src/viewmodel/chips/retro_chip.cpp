/*
 * SPDX-License-Identifier: MIT
 */

#include "viewmodel/chips/retro_chip.h"

#include "platform/clock_sync.h"
#include "platform/json.h"
#include "platform/net.h"
#include "platform/process.h"
#include "viewmodel/chips/jack_in_chip.h"

#include <signal.h>
#include <sys/stat.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <ctime>
#include <fstream>
#include <random>
#include <sstream>

namespace viewmodel::chips {
namespace {

namespace fs = std::filesystem;

constexpr int kApiTimeoutMs = 15000;
// A page of the game list. RomM 5.4 sends ~10 KB of metadata per game with no way
// to ask for less, so pages stay small enough for a slow link (~100 KB/s) to
// finish one well inside the timeout.
constexpr int kPageSize = 25;
constexpr int kPageTimeoutMs = 25000;
constexpr size_t kPageMaxReply = 2u * 1024 * 1024;
constexpr int kLoadAheadGames = 8;       // fetch the next page this close to the end
constexpr size_t kMaxLoadedGames = 5000;
constexpr size_t kCacheGames = 200;      // per tab, on disk
constexpr int kDownloadTimeoutMs = 180000;
constexpr size_t kMaxRomBytes = 64u * 1024 * 1024;
constexpr uint32_t kQuickExitMs = 3000;  // an emulator that dies this fast didn't start

// Tabs in mockup order first; anything else RomM has comes after.
constexpr const char* kTabOrder[] = {"gb", "gbc", "gba", "nes", "snes", "genesis-slash-megadrive", "n64", "psx"};

size_t tab_rank(const std::string& slug) {
    for (size_t i = 0; i < std::size(kTabOrder); ++i) {
        if (slug == kTabOrder[i]) return i;
    }
    return std::size(kTabOrder);
}

std::string lower(std::string value) {
    for (auto& c : value) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return value;
}

std::string replace_all(std::string value, const std::string& from, const std::string& to) {
    for (size_t at = value.find(from); at != std::string::npos; at = value.find(from, at + to.size())) {
        value.replace(at, from.size(), to);
    }
    return value;
}

} // namespace

RetroChip::RetroChip(model::PetConfig config)
    : config_(std::move(config)), dir_(platform::proc::expand_home(config_.retro_dir)),
      cache_dir_(platform::proc::expand_home("~/.cache/alicepet")) {
    base_ = config_.romm_url;
    while (!base_.empty() && base_.back() == '/') base_.pop_back();
}

RetroChip::~RetroChip() {
    if (player_ > 0) platform::proc::stop(player_, SIGTERM, 1500);
}

std::string RetroChip::short_label(const std::string& slug, const std::string& fallback) {
    if (slug == "genesis-slash-megadrive" || slug == "genesis") return "GEN";
    if (slug == "gamegear" || slug == "game-gear") return "GG";
    if (slug == "wonderswan") return "WS";
    if (slug == "wonderswan-color" || slug == "wonderswancolor") return "WSC";
    if (slug == "browser") return "FLASH";
    if (slug == "nds") return "NDS";
    if (slug == "gb" || slug == "gbc" || slug == "gba" || slug == "nes" || slug == "snes" || slug == "n64" || slug == "psx") {
        std::string up = slug;
        for (auto& c : up) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        return up;
    }
    std::string label = fallback.empty() ? slug : fallback;
    if (label.size() > 6) label.resize(6);
    for (auto& c : label) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return label;
}

std::string RetroChip::fit_text(const std::string& slug) {
    // Native sizes against the 320x170 panel (under the 15 px bar the emulator
    // gets the whole screen): integer 1:1 when it fits, otherwise scaled to height.
    int w = 0, h = 0;
    if (slug == "gb" || slug == "gbc") w = 160, h = 144;
    else if (slug == "gba") w = 240, h = 160;
    else if (slug == "nes") w = 256, h = 240;
    else if (slug == "snes") w = 256, h = 224;
    else if (slug == "genesis-slash-megadrive" || slug == "genesis") w = 320, h = 224;
    if (w == 0) return "scaled to fit";
    if (w <= 320 && h <= 170) return "1:1 pixel-perfect";
    return "scaled " + std::to_string(170 * 100 / h) + "%";
}

std::string RetroChip::size_text(long long bytes) {
    char text[24];
    if (bytes >= 1024 * 1024) std::snprintf(text, sizeof(text), "%.1f MB", bytes / (1024.0 * 1024.0));
    else std::snprintf(text, sizeof(text), "%lld KB", (bytes + 1023) / 1024);
    return text;
}

long long RetroChip::parse_iso_time(const std::string& text) {
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, sec = 0;
    if (std::sscanf(text.c_str(), "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &sec) < 3) return 0;
    std::tm tm{};
    tm.tm_year = y - 1900;
    tm.tm_mon = mo - 1;
    tm.tm_mday = d;
    tm.tm_hour = h;
    tm.tm_min = mi;
    tm.tm_sec = sec;
    long long t = static_cast<long long>(timegm(&tm));
    // Offset after the seconds (and optional fraction): +HH:MM / -HH:MM; Z or none = UTC.
    const size_t tpos = text.find('T');
    const size_t sign = tpos == std::string::npos ? std::string::npos : text.find_first_of("+-", tpos);
    if (sign != std::string::npos) {
        int oh = 0, om = 0;
        if (std::sscanf(text.c_str() + sign + 1, "%d:%d", &oh, &om) >= 1) {
            const long long offset = oh * 3600LL + om * 60LL;
            t += text[sign] == '+' ? -offset : offset;
        }
    }
    return t;
}

std::string RetroChip::ago_text(long long then, long long now) {
    const long long s = std::max(0LL, now - then);
    if (s < 120) return "just now";
    if (s < 3600) return std::to_string(s / 60) + " min ago";
    if (s < 86400) return std::to_string(s / 3600) + " h ago";
    if (s < 2 * 86400) return "yesterday";
    return std::to_string(s / 86400) + " days ago";
}

std::vector<RetroChip::Save> RetroChip::parse_saves(const std::string& text) {
    std::vector<Save> out;
    platform::json::Value root;
    if (!platform::json::parse(text, root)) return out;
    const auto& list = root.is_array() ? root : root["items"];
    for (const auto& v : list.items()) {
        Save save;
        save.id = static_cast<int>(v["id"].integer());
        save.file_name = v["file_name"].str();
        save.updated_at = v["updated_at"].str();
        save.updated = parse_iso_time(save.updated_at);
        if (save.id > 0) out.push_back(std::move(save));
    }
    return out;
}

const RetroChip::Save* RetroChip::pick_save(const std::vector<Save>& saves, const std::string& file_name) {
    // Newest with our file name; if none has it (saved from another emulator), the newest overall.
    const Save* named = nullptr;
    const Save* newest = nullptr;
    for (const auto& s : saves) {
        if (s.file_name == file_name && (!named || s.updated > named->updated)) named = &s;
        if (!newest || s.updated > newest->updated) newest = &s;
    }
    return named ? named : newest;
}

std::string RetroChip::safe_name(const std::string& raw, const std::string& fallback) {
    // Drop anything that could change a path or break out of a quoted header value.
    std::string out;
    for (unsigned char c : raw) {
        if (c < 0x20 || c == 0x7f || c == '/' || c == '\\' || c == '"') continue;
        out += static_cast<char>(c);
    }
    // ".." and leading dots would name a parent or hidden entry; spaces at the edges are noise.
    for (size_t at; (at = out.find("..")) != std::string::npos;) out.erase(at, 2);
    size_t first = out.find_first_not_of(" .");
    if (first == std::string::npos) return fallback;
    out = out.substr(first);
    while (!out.empty() && out.back() == ' ') out.pop_back();
    if (out.size() > 200) out.resize(200);
    return out.empty() ? fallback : out;
}

std::string RetroChip::multipart_body(const std::string& boundary, const std::string& field,
                                      const std::string& file_name, const std::string& bytes) {
    std::string body = "--" + boundary + "\r\n";
    body += "Content-Disposition: form-data; name=\"" + field + "\"; filename=\"" + safe_name(file_name, "save.srm") + "\"\r\n";
    body += "Content-Type: application/octet-stream\r\n\r\n";
    body += bytes;
    body += "\r\n--" + boundary + "--\r\n";
    return body;
}

std::vector<RetroChip::Platform> RetroChip::parse_platforms(const std::string& text) {
    std::vector<Platform> out;
    platform::json::Value root;
    if (!platform::json::parse(text, root)) return out;
    const auto& list = root.is_array() ? root : root["items"];
    for (const auto& p : list.items()) {
        Platform platform;
        platform.id = static_cast<int>(p["id"].integer());
        platform.slug = p["slug"].str();
        platform.rom_count = static_cast<int>(p["rom_count"].integer());
        const std::string name = p["display_name"].str(p["name"].str());
        platform.label = short_label(platform.slug, name);
        if (platform.id > 0 && platform.rom_count > 0) out.push_back(std::move(platform));
    }
    std::stable_sort(out.begin(), out.end(), [](const Platform& a, const Platform& b) {
        return tab_rank(a.slug) < tab_rank(b.slug);
    });
    return out;
}

std::vector<RetroChip::Game> RetroChip::parse_games_page(const std::string& text, int& total) {
    std::vector<Game> out;
    total = -1;
    platform::json::Value root;
    if (!platform::json::parse(text, root)) return out;
    // RomM 3.5+ pages results ({"items": [...], "total": N}); older servers return the array.
    const auto& list = root.is_array() ? root : root["items"];
    for (const auto& r : list.items()) {
        Game game;
        game.id = static_cast<int>(r["id"].integer());
        // RomM's matched name; unmatched games have none, so their file name stands in.
        for (const char* field : {"name", "fs_name_no_tags", "fs_name_no_ext", "fs_name"}) {
            if (game.name.empty()) game.name = r[field].str();
        }
        game.file = r["fs_name"].str(r["file_name"].str());
        game.platform = r["platform_slug"].str();
        game.size = r["fs_size_bytes"].integer(r["file_size_bytes"].integer());
        const long long released_ms = r["metadatum"]["first_release_date"].integer(r["first_release_date"].integer());
        if (released_ms > 0) game.year = 1970 + static_cast<int>(released_ms / 31556952000LL);
        if (game.id > 0 && !game.file.empty()) out.push_back(std::move(game));
    }
    total = root.is_array() ? static_cast<int>(out.size()) : static_cast<int>(root["total"].integer(-1));
    return out;
}

bool RetroChip::slug_matches(const std::string& slug, const std::string& wanted) {
    auto squash = [](const std::string& value) {
        std::string out;
        for (const char c : value) {
            if (std::isalnum(static_cast<unsigned char>(c))) out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        return out;
    };
    return !slug.empty() && squash(slug) == squash(wanted);
}

std::vector<RetroChip::Platform> RetroChip::filter_platforms(std::vector<Platform> platforms,
                                                             const std::vector<std::string>& allowed) {
    if (allowed.empty()) return platforms;
    std::vector<Platform> out;
    for (const auto& wanted : allowed) {
        for (auto& p : platforms) {
            if (p.id > 0 && slug_matches(p.slug, wanted)) {
                out.push_back(std::move(p));
                p.id = 0;  // taken
            }
        }
    }
    return out;
}

std::string RetroChip::wrap_title(const std::string& title, size_t cols, size_t lines) {
    if (cols == 0 || lines == 0) return "";
    std::vector<std::string> words;
    std::string word;
    for (const char c : title + " ") {
        if (c == ' ' || c == '\t' || c == '\n') {
            if (!word.empty()) words.push_back(word);
            word.clear();
        } else {
            word += c;
        }
    }
    // Counts UTF-8 characters, not bytes.
    auto width = [](const std::string& s) {
        size_t n = 0;
        for (const char c : s) n += (static_cast<unsigned char>(c) & 0xC0) != 0x80;
        return n;
    };
    auto cut = [](const std::string& s, size_t chars) {
        size_t n = 0, i = 0;
        for (; i < s.size(); ++i) {
            if ((static_cast<unsigned char>(s[i]) & 0xC0) != 0x80 && n++ == chars) break;
        }
        return s.substr(0, i);
    };
    std::vector<std::string> out;
    std::string line;
    size_t i = 0;
    for (; i < words.size(); ++i) {
        std::string w = words[i];
        const std::string candidate = line.empty() ? w : line + " " + w;
        if (width(candidate) <= cols) {
            line = candidate;
            continue;
        }
        if (!line.empty()) {
            out.push_back(line);
            line.clear();
            if (out.size() == lines) break;
        }
        // A single word wider than a line is split, as no space can be found for it.
        while (width(w) > cols && out.size() + 1 < lines) {
            out.push_back(cut(w, cols));
            w = w.substr(cut(w, cols).size());
        }
        line = w;
    }
    const bool more = i < words.size() || width(line) > cols;
    if (!line.empty() && out.size() < lines) out.push_back(line);
    if (more && !out.empty()) {
        std::string& last = out.back();
        // Room for "..." by dropping whole words; only a lone word is cut.
        while (width(last) + 3 > cols && last.find(' ') != std::string::npos) last.erase(last.rfind(' '));
        if (width(last) + 3 > cols) last = cut(last, cols >= 3 ? cols - 3 : 0);
        while (!last.empty() && (last.back() == ' ' || last.back() == ':' || last.back() == ',' || last.back() == '-')) last.pop_back();
        last += "...";
    }
    std::string text;
    for (size_t k = 0; k < out.size(); ++k) text += (k ? "\n" : "") + out[k];
    return text;
}

std::vector<RetroChip::Game> RetroChip::parse_games(const std::string& text) {
    int total = 0;
    auto out = parse_games_page(text, total);
    std::sort(out.begin(), out.end(), [](const Game& a, const Game& b) { return lower(a.name) < lower(b.name); });
    return out;
}

std::string RetroChip::games_url(const std::string& base, int platform_id, int offset, int limit) {
    std::string url = base + "/api/roms?order_by=name&order_dir=asc&limit=" + std::to_string(limit) +
                      "&offset=" + std::to_string(offset) +
                      "&with_char_index=false&with_rom_id_index=false&with_filter_values=false&with_files=false";
    // RomM 5 filters on platform_ids; the old platform_id is silently ignored.
    if (platform_id > 0) url += "&platform_ids=" + std::to_string(platform_id);
    return url;
}

void RetroChip::merge_page(std::vector<Game>& games, int offset, std::vector<Game> page) {
    const size_t at = std::min(games.size(), static_cast<size_t>(std::max(0, offset)));
    std::vector<int> ids;
    // A game is listed once, even if RomM repeats it within a page.
    page.erase(std::remove_if(page.begin(), page.end(), [&](const Game& g) {
                   if (std::find(ids.begin(), ids.end(), g.id) != ids.end()) return true;
                   ids.push_back(g.id);
                   return false;
               }), page.end());
    auto in_page = [&](const Game& g) { return std::find(ids.begin(), ids.end(), g.id) != ids.end(); };
    std::vector<Game> out;
    out.reserve(std::max(games.size(), at + page.size()));
    for (size_t i = 0; i < at; ++i) {
        if (!in_page(games[i])) out.push_back(std::move(games[i]));
    }
    const size_t end = std::min(games.size(), at + page.size());
    for (auto& g : page) out.push_back(std::move(g));
    for (size_t i = end; i < games.size(); ++i) {
        if (!in_page(games[i])) out.push_back(std::move(games[i]));
    }
    games = std::move(out);
}

std::string RetroChip::cache_json(int total, const std::vector<Game>& games, size_t max_games) {
    using platform::net::json_escape;
    std::string out = "{\"total\":" + std::to_string(total) + ",\"items\":[";
    for (size_t i = 0; i < games.size() && i < max_games; ++i) {
        const auto& g = games[i];
        if (i) out += ",";
        out += "{\"id\":" + std::to_string(g.id) + ",\"name\":\"" + json_escape(g.name) + "\",\"fs_name\":\"" +
               json_escape(g.file) + "\",\"platform_slug\":\"" + json_escape(g.platform) +
               "\",\"fs_size_bytes\":" + std::to_string(g.size) + ",\"year\":" + std::to_string(g.year) + "}";
    }
    return out + "]}";
}

std::vector<RetroChip::Game> RetroChip::parse_cache(const std::string& text, int& total) {
    std::vector<Game> out;
    total = 0;
    platform::json::Value root;
    if (!platform::json::parse(text, root) || !root.is_object()) return out;
    for (const auto& r : root["items"].items()) {
        Game game;
        game.id = static_cast<int>(r["id"].integer());
        game.name = r["name"].str();
        game.file = r["fs_name"].str();
        game.platform = r["platform_slug"].str();
        game.size = r["fs_size_bytes"].integer();
        game.year = static_cast<int>(r["year"].integer());
        if (game.id > 0 && !game.file.empty()) out.push_back(std::move(game));
    }
    total = std::max(static_cast<int>(out.size()), static_cast<int>(root["total"].integer()));
    return out;
}

std::string RetroChip::platforms_cache_json(const std::vector<Platform>& platforms) {
    using platform::net::json_escape;
    std::string out = "[";
    for (const auto& p : platforms) {
        if (p.id <= 0) continue;  // ALL is rebuilt on load
        if (out.size() > 1) out += ",";
        out += "{\"id\":" + std::to_string(p.id) + ",\"slug\":\"" + json_escape(p.slug) + "\",\"display_name\":\"" +
               json_escape(p.label) + "\",\"rom_count\":" + std::to_string(p.rom_count) + "}";
    }
    return out + "]";
}

std::vector<std::string> RetroChip::headers() const {
    if (config_.romm_token.empty()) return {};
    return {"Authorization: Bearer " + config_.romm_token};
}

void RetroChip::enter() {
    if (!loaded_) load_platforms();
}

namespace {
std::string read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::stringstream ss;
    if (in) ss << in.rdbuf();
    return ss.str();
}

void write_file_atomic(const std::string& path, const std::string& text) {
    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);
    const std::string tmp = path + ".part";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out << text;
        if (!out) return;
    }
    fs::rename(tmp, path, ec);
}
}  // namespace

std::string RetroChip::cache_path(const std::string& tab) const {
    // The slug comes from the server: keep it a plain file name.
    return cache_dir_ + "/retro-" + (tab.empty() ? "all" : safe_name(tab, "tab")) + ".json";
}

std::string RetroChip::current_tab() const {
    return platform_ < platforms_.size() ? platforms_[platform_].slug : "";
}

void RetroChip::load_platforms() {
    loaded_ = true;
    if (base_.empty()) {
        view_ = View::Error;
        status_ = "No RomM server. Set [romm] url= (and token=) in alicepet.local.conf.";
        touch();
        return;
    }
    // Last session's systems come up at once; RomM's answer replaces them.
    const auto cached = parse_platforms(read_file(cache_path("platforms")));
    if (!cached.empty() && platforms_.empty()) {
        Result seed;
        seed.what = Result::What::Platforms;
        seed.platforms = cached;
        seed.generation = generation_;
        seed.offset = -1;  // marks "from cache"
        on_result(std::move(seed));
    } else if (platforms_.empty()) {
        view_ = View::Loading;
        status_ = "loading systems...";
    }
    const size_t gen = generation_;
    results_.run([url = base_ + "/api/platforms", headers = headers(), gen, clock_set = config_.clock_set]() {
        Result result;
        result.generation = gen;
        auto response = platform::net::http_request("GET", url, "", headers, kApiTimeoutMs, kPageMaxReply);
        if (!response.ok) response = platform::net::http_request("GET", url, "", headers, kPageTimeoutMs, kPageMaxReply);
        // No clock battery: Holden's own RomM server says what time it is before any save is compared.
        if (response.date > 0) platform::clock_sync::offer(clock_set, response.date, true, "RomM");
        if (!response.ok || response.status != 200) {
            result.text = response.ok ? "RomM said HTTP " + std::to_string(response.status) : "RomM: " + response.error;
            result.offset = -2;  // platforms failed
            return result;
        }
        result.what = Result::What::Platforms;
        result.platforms = parse_platforms(response.body);
        return result;
    });
    touch();
}

void RetroChip::load_games() {
    shown_valid_ = false;
    games_.clear();
    selected_ = 0;
    total_ = 0;
    next_offset_ = 0;
    more_failed_ = false;
    loading_more_ = false;
    ++list_generation_;
    int cached_total = 0;
    auto cached = parse_cache(read_file(cache_path(current_tab())), cached_total);
    from_cache_ = !cached.empty();
    if (from_cache_) {
        games_ = std::move(cached);
        total_ = cached_total;
        view_ = View::Library;
        status_.clear();
    } else {
        view_ = View::Loading;
        status_ = "loading games...";
    }
    fetch_page(0);
    touch();
}

void RetroChip::fetch_page(int offset) {
    loading_more_ = true;
    const int platform_id = platform_ < platforms_.size() ? platforms_[platform_].id : 0;
    results_.run([url = games_url(base_, platform_id, offset, kPageSize), headers = headers(),
                  gen = list_generation_, offset]() {
        Result result;
        result.what = Result::What::Games;
        result.generation = gen;
        result.offset = offset;
        const auto response = platform::net::http_request("GET", url, "", headers, kPageTimeoutMs, kPageMaxReply);
        if (!response.ok || response.status != 200) {
            result.text = response.ok ? "RomM said HTTP " + std::to_string(response.status) : "RomM: " + response.error;
            return result;
        }
        result.games = parse_games_page(response.body, result.total);
        return result;
    });
}

void RetroChip::maybe_load_more() {
    if (loading_more_ || more_failed_ || next_offset_ >= total_ || games_.size() >= kMaxLoadedGames) return;
    // Searching only sees what is loaded, so a search pulls the rest in.
    const size_t near = static_cast<size_t>(std::max(0, next_offset_ - kLoadAheadGames));
    if (!search_.empty() || selected_ >= near || games().size() <= selected_ + kLoadAheadGames) fetch_page(next_offset_);
}

std::string RetroChip::local_path(const Game& game) const {
    return dir_ + "/" + safe_name(game.platform, "other") + "/" + safe_name(game.file, "game");
}

bool RetroChip::downloaded(const Game& game) const {
    std::error_code ec;
    return fs::exists(local_path(game), ec);
}

std::string RetroChip::local_save_path(const Game& game) const {
    const std::string stem = fs::path(safe_name(game.file, "game")).stem().string();
    return dir_ + "/" + safe_name(game.platform, "other") + "/saves/" + safe_name(stem, "game") + ".srm";
}

namespace {
long long mtime_of(const std::string& path) {
    struct stat st {};
    return ::stat(path.c_str(), &st) == 0 ? static_cast<long long>(st.st_mtime) : 0;
}

// Marks a PET save that was played on while RomM held a newer one the PET could
// not fetch. Neither copy is the other's ancestor, so nothing is uploaded over
// RomM's until the marker is gone.
std::string conflict_marker(const std::string& save_path) {
    return save_path + ".conflict";
}

// The PET was built after this moment; a clock before it was never set.
constexpr long long kClockFloor = 1790000000;  // 2026-09-21
constexpr long long kFutureSlackS = 300;
}  // namespace

// "Newest wins" only means something when the PET's clock is right, and the
// CardputerZero has no clock battery. A clock that was never set, or a RomM
// save stamped later than the PET's "now", says it is wrong.
bool RetroChip::clock_trusted(const std::vector<Save>& saves, long long now) {
    if (now < kClockFloor) return false;
    for (const auto& s : saves) {
        if (s.updated > now + kFutureSlackS) return false;
    }
    return true;
}

bool RetroChip::sync_paused(const Game& game) const {
    return saves_rom_ == game.id && !clock_trusted(saves_, static_cast<long long>(std::time(nullptr)));
}

std::string RetroChip::save_text(bool& good) const {
    good = false;
    const Game* g = game();
    if (!g) return "";
    if (!save_status_.empty()) return save_status_;
    const long long local = mtime_of(local_save_path(*g));
    if (!config_.retro_sync_saves || base_.empty()) return local ? "on PET (sync off)" : "none yet";
    if (saves_checking_ && saves_rom_ == g->id) return "checking RomM...";
    if (saves_failed_ && saves_rom_ == g->id) return local ? "on PET (RomM offline)" : "RomM unreachable";
    if (sync_paused(*g)) return "PET clock wrong (paused)";
    if (fs::exists(conflict_marker(local_save_path(*g)))) return "differs (not uploaded)";
    const Save* remote = saves_rom_ == g->id ? pick_save(saves_, fs::path(local_save_path(*g)).filename().string()) : nullptr;
    if (remote) {
        good = local == 0 || remote->updated >= local - 2;
        if (local > remote->updated + 2) return "newer on PET (uploads)";
        return "synced " + ago_text(remote->updated, static_cast<long long>(std::time(nullptr)));
    }
    return local ? "on PET only (uploads)" : "none yet";
}

void RetroChip::fetch_saves(const Game& game) {
    if (!config_.retro_sync_saves || base_.empty()) return;
    saves_rom_ = game.id;
    play_local_rom_ = 0;
    start_pending_rom_ = 0;
    saves_known_ = false;
    saves_checking_ = true;
    saves_failed_ = false;
    saves_.clear();
    results_.run([url = base_ + "/api/saves?rom_id=" + std::to_string(game.id), headers = headers(), rom = game.id,
                  clock_set = config_.clock_set]() {
        Result result;
        result.rom_id = rom;
        const auto response = platform::net::http_request("GET", url, "", headers, kApiTimeoutMs);
        if (response.date > 0) platform::clock_sync::offer(clock_set, response.date, true, "RomM");
        if (!response.ok || response.status != 200) {
            result.what = Result::What::Saves;
            result.text = response.ok ? "HTTP " + std::to_string(response.status) : response.error;
            return result;
        }
        result.what = Result::What::Saves;
        result.saves = parse_saves(response.body);
        return result;
    });
}

void RetroChip::start(const Game& game) {
    // A newer save on RomM than on the PET is pulled first, so play picks up
    // where another device left off.
    const std::string path = local_save_path(game);
    const bool sync = config_.retro_sync_saves && !base_.empty();
    // RomM hasn't said yet which saves it has: an empty list here means "not
    // known", not "none". Wait for the answer rather than start on an old save.
    if (sync && saves_rom_ == game.id && saves_checking_) {
        start_pending_rom_ = game.id;
        status_ = "checking RomM for a newer save...";
        touch();
        return;
    }
    start_pending_rom_ = 0;
    launch_saves_known_ = !sync || (saves_rom_ == game.id && saves_known_);
    const Save* remote = config_.retro_sync_saves && saves_rom_ == game.id
                             ? pick_save(saves_, fs::path(path).filename().string()) : nullptr;
    if (remote && remote->updated > mtime_of(path) + 2 && !sync_paused(game)) {
        if (play_local_rom_ != game.id) {
            pull_save(game, *remote);
            return;
        }
        // The pull failed and the user chose to play the PET's save anyway.
        // Mark it so this session's save is never uploaded over RomM's newer one.
        std::error_code ec;
        fs::create_directories(fs::path(path).parent_path(), ec);
        std::ofstream(conflict_marker(path)) << remote->id << "\n";
    }
    play_local_rom_ = 0;
    play(game);
}

void RetroChip::pull_save(const Game& game, const Save& save) {
    view_ = View::Downloading;
    play_after_download_ = true;
    downloading_id_ = game.id;
    status_ = "syncing save from RomM...";
    const size_t gen = ++generation_;
    results_.run([url = base_ + "/api/saves/" + std::to_string(save.id) + "/content", headers = headers(),
                  path = local_save_path(game), updated = save.updated, gen]() {
        Result result;
        result.generation = gen;
        const auto response = platform::net::http_request("GET", url, "", headers, kApiTimeoutMs, 8 * 1024 * 1024);
        result.what = Result::What::SaveDown;  // on failure the user is asked before playing the PET's save
        if (!response.ok || response.status != 200) {
            result.text = response.ok ? "HTTP " + std::to_string(response.status) : response.error;
            return result;
        }
        if (response.body.empty()) {
            result.text = "RomM sent an empty save";
            return result;
        }
        std::error_code ec;
        fs::create_directories(fs::path(path).parent_path(), ec);
        // Write beside the save and swap it in, so a failed write never leaves
        // half a save where the good one was.
        const std::string tmp = path + ".part";
        {
            std::ofstream out(tmp, std::ios::binary);
            out.write(response.body.data(), static_cast<std::streamsize>(response.body.size()));
            out.flush();
            if (!out) {
                fs::remove(tmp, ec);
                result.text = "can't write the save";
                return result;
            }
        }
        if (fs::exists(path, ec)) fs::copy_file(path, path + ".bak", fs::copy_options::overwrite_existing, ec);
        fs::rename(tmp, path, ec);
        if (ec) {
            fs::remove(tmp, ec);
            result.text = "can't replace the save";
            return result;
        }
        fs::remove(conflict_marker(path), ec);  // RomM's copy won: nothing left to keep apart
        // Stamp it with RomM's time so it isn't uploaded straight back.
        fs::last_write_time(path, fs::file_time_type::clock::now() -
                                      std::chrono::seconds(std::max(0LL, static_cast<long long>(std::time(nullptr)) - updated)), ec);
        return result;
    });
    touch();
}

// After a play that started without RomM's save list: ask again, and upload only
// if RomM has nothing newer than the save the PET started from (on_result).
void RetroChip::check_then_push(const Game& game) {
    save_status_ = "checking RomM before uploading...";
    results_.run([url = base_ + "/api/saves?rom_id=" + std::to_string(game.id), headers = headers(), rom = game.id]() {
        Result result;
        result.what = Result::What::Saves;
        result.rom_id = rom;
        result.then_push = true;
        const auto response = platform::net::http_request("GET", url, "", headers, kApiTimeoutMs);
        if (!response.ok || response.status != 200) {
            result.text = response.ok ? "HTTP " + std::to_string(response.status) : response.error;
            return result;
        }
        result.saves = parse_saves(response.body);
        return result;
    });
    touch();
}

void RetroChip::push_save(const Game& game) {
    const std::string path = local_save_path(game);
    if (fs::exists(conflict_marker(path))) {
        status_ = "save kept on the PET: RomM has a different one";
        touch();
        return;
    }
    if (sync_paused(game)) {
        status_ = "save kept on the PET: its clock is wrong, sync paused";
        touch();
        return;
    }
    std::ifstream in(path, std::ios::binary);
    std::stringstream bytes;
    if (in) bytes << in.rdbuf();
    // Never replace RomM's save with nothing: an unreadable or empty file stays put.
    if (!in || bytes.str().empty()) return;
    save_status_ = "uploading save...";
    std::random_device rd;
    const std::string boundary = "alicepet" + std::to_string(rd()) + std::to_string(rd());
    const std::string name = fs::path(path).filename().string();
    const std::string url = base_ + "/api/saves?rom_id=" + std::to_string(game.id) + "&emulator=retroarch&overwrite=true";
    auto hdrs = headers();
    hdrs.push_back("Content-Type: multipart/form-data; boundary=" + boundary);
    results_.run([url, hdrs, body = multipart_body(boundary, "saveFile", name, bytes.str()), rom = game.id]() {
        Result result;
        result.what = Result::What::SaveUp;
        result.rom_id = rom;
        const auto response = platform::net::http_request("POST", url, body, hdrs, kApiTimeoutMs);
        if (!response.ok || response.status < 200 || response.status >= 300) {
            result.text = response.ok ? "HTTP " + std::to_string(response.status) : response.error;
            return result;
        }
        platform::json::Value v;
        if (platform::json::parse(response.body, v)) {
            Save save;
            save.id = static_cast<int>(v["id"].integer());
            save.file_name = v["file_name"].str();
            save.updated_at = v["updated_at"].str();
            save.updated = parse_iso_time(save.updated_at);
            if (save.id > 0) result.saves.push_back(save);
        }
        return result;
    });
    touch();
}

void RetroChip::download(const Game& game, bool then_play) {
    view_ = View::Downloading;
    play_after_download_ = then_play;
    downloading_id_ = game.id;
    status_ = "downloading " + size_text(game.size) + "...";
    const size_t gen = ++generation_;
    const std::string url = base_ + "/api/roms/" + std::to_string(game.id) + "/content/" + platform::net::url_encode(game.file);
    results_.run([url, headers = headers(), path = local_path(game), gen, clock_set = config_.clock_set]() {
        Result result;
        result.generation = gen;
        // Straight to disk: a 64 MB ROM held in memory is most of what a 512 MB PET has free.
        std::error_code ec;
        fs::create_directories(fs::path(path).parent_path(), ec);
        const std::string tmp = path + ".part";
        const auto response = platform::net::http_download(url, headers, tmp, kDownloadTimeoutMs, kMaxRomBytes);
        if (response.date > 0) platform::clock_sync::offer(clock_set, response.date, true, "RomM");
        if (!response.ok || response.status != 200) {
            result.text = response.ok ? "download refused (HTTP " + std::to_string(response.status) + ")"
                                      : "download failed: " + response.error;
            return result;
        }
        fs::rename(tmp, path, ec);
        if (ec) {
            result.text = "can't save " + path;
            return result;
        }
        result.what = Result::What::Download;
        return result;
    });
    touch();
}

void RetroChip::play(const Game& game) {
    const auto core = config_.retro_cores.find(game.platform);
    if (core == config_.retro_cores.end() || core->second.empty()) {
        view_ = View::Error;
        status_ = "No emulator core for " + game.platform + ". Set [retro] core_" + game.platform + "=.";
        touch();
        return;
    }
    const std::string save_path = local_save_path(game);
    const std::string save_dir = fs::path(save_path).parent_path().string();
    std::error_code ec;
    fs::create_directories(save_dir, ec);
    launch_save_mtime_ = mtime_of(save_path);
    playing_id_ = game.id;
    auto argv = platform::proc::split_command(config_.retro_player);
    for (auto& arg : argv) {
        arg = replace_all(arg, "{core}", core->second);
        arg = replace_all(arg, "{rom}", local_path(game));
        arg = replace_all(arg, "{savedir}", save_dir);
    }
    player_ = argv.empty() ? -1 : platform::proc::spawn(argv);
    if (player_ < 0) {
        view_ = View::Error;
        status_ = "Emulator failed to start. Check [retro] player=.";
    } else {
        view_ = View::Playing;
        play_ms_ = 0;
        status_ = "playing";
    }
    touch();
}

void RetroChip::play_on_pc(const Game& game) {
    if (base_.empty()) return;
    // RomM 5 web players: Ruffle for Flash, js-dos for DOS, EmulatorJS for the rest.
    std::string file = game.file;
    for (char& c : file) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    const bool swf = file.size() > 4 && file.compare(file.size() - 4, 4, ".swf") == 0;
    const std::string route = swf ? "ruffle" : game.platform == "dos" ? "jsdos" : "ejs";
    const std::string path = "/rom/" + std::to_string(game.id) + "/" + route;
    // The relay serves RomM's whole site on the link; base_ may carry a path prefix.
    const auto scheme = base_.find("://");
    const auto slash = scheme == std::string::npos ? std::string::npos : base_.find('/', scheme + 3);
    const std::string origin = slash == std::string::npos ? base_ : base_.substr(0, slash);
    const std::string prefix = slash == std::string::npos ? "" : base_.substr(slash);
    const std::string url = pc_link::relay_site(origin, headers(), prefix + path);
    if (url.empty()) {
        status_ = "couldn't start the web relay for the PC";
        touch();
        return;
    }
    pc_link::send("{\"t\":\"play\",\"kind\":\"game\",\"title\":\"" + platform::net::json_escape(game.name) +
                  "\",\"url\":\"" + platform::net::json_escape(url) + "\",\"direct\":\"" +
                  platform::net::json_escape(base_ + path) + "\"}");
    status_ = "playing in the browser on " + pc_link::host();
    touch();
}

void RetroChip::stop_game() {
    if (player_ > 0) {
        platform::proc::stop(player_, SIGTERM, 2000);  // RetroArch writes SRAM on SIGTERM
        player_ = -1;
    }
    if (view_ == View::Playing || view_ == View::Downloading) {
        ++generation_;
        view_ = View::Details;
        touch();
    }
}

const std::vector<const RetroChip::Game*>& RetroChip::games() const {
    // Rebuilt only when the list, the search text, or the list storage changed: keypresses and renders
    // call this several times, and lowercasing every title each time made menu navigation laggy.
    if (shown_valid_ && shown_search_ == search_ && shown_size_ == games_.size() && shown_data_ == games_.data()) {
        return shown_;
    }
    shown_.clear();
    const std::string needle = lower(search_);
    for (const auto& g : games_) {
        if (needle.empty() || lower(g.name).find(needle) != std::string::npos) shown_.push_back(&g);
    }
    shown_search_ = search_;
    shown_size_ = games_.size();
    shown_data_ = games_.data();
    shown_valid_ = true;
    return shown_;
}

size_t RetroChip::list_first(size_t count, size_t selected, size_t rows) {
    return media_list_first(count, selected, rows);
}

const RetroChip::Game* RetroChip::game() const {
    const auto& list = games();
    return selected_ < list.size() ? list[selected_] : nullptr;
}

void RetroChip::on_result(Result result) {
    shown_valid_ = false;  // results can merge into games_ in place
    if (result.what == Result::What::Saves && result.then_push) {
        save_status_.clear();
        const Game* played = nullptr;
        for (const auto& g : games_) {
            if (g.id == result.rom_id) played = &g;
        }
        if (!played) return;
        if (!result.text.empty()) {
            status_ = "save kept on the PET: couldn't check RomM";
            touch();
            return;
        }
        const std::string path = local_save_path(*played);
        const Save* remote = pick_save(result.saves, fs::path(path).filename().string());
        if (result.rom_id == saves_rom_) {
            saves_ = result.saves;
            saves_known_ = true;
            saves_failed_ = false;
            saves_checking_ = false;
        }
        if (remote && remote->updated > launch_save_mtime_ + 2) {
            // RomM's save is newer than the one this play started from: keep both.
            std::ofstream(conflict_marker(path)) << remote->id << "\n";
            status_ = "save kept on the PET: RomM has a different one";
            touch();
            return;
        }
        push_save(*played);
        return;
    }
    if (result.what == Result::What::Saves) {
        if (result.rom_id != saves_rom_) return;  // the user moved on to another game
        saves_checking_ = false;
        saves_failed_ = !result.text.empty();
        saves_known_ = !saves_failed_;
        saves_ = std::move(result.saves);
        touch();
        // ENTER was pressed while this check ran: start now that the answer is in.
        if (start_pending_rom_ == result.rom_id) {
            start_pending_rom_ = 0;
            status_.clear();
            const Game* g = game();
            if (g && g->id == result.rom_id && view_ == View::Details && downloaded(*g)) start(*g);
        }
        return;
    }
    if (result.what == Result::What::SaveUp) {
        if (result.text.empty()) {
            save_status_.clear();
            if (result.rom_id == saves_rom_) {
                for (auto& s : result.saves) {
                    auto it = std::find_if(saves_.begin(), saves_.end(), [&](const Save& x) { return x.id == s.id; });
                    if (it != saves_.end()) *it = s;
                    else saves_.push_back(s);
                }
            }
            status_ = "save uploaded to RomM";
        } else {
            save_status_.clear();
            status_ = "save kept on the PET (upload: " + result.text + ")";
        }
        touch();
        return;
    }
    if (result.what == Result::What::Games) {
        if (result.generation != list_generation_) return;  // another tab now
        loading_more_ = false;
        if (!result.text.empty()) {
            more_failed_ = true;
            if (games_.empty()) {
                view_ = View::Error;
                status_ = result.text;
            } else {
                status_ = (from_cache_ ? "offline, showing saved list (" : "more games didn't load (") + result.text + ")";
            }
            touch();
            return;
        }
        if (result.total >= 0) total_ = result.total;
        const int got = static_cast<int>(result.games.size());
        merge_page(games_, result.offset, std::move(result.games));
        next_offset_ = result.offset + got;
        if (got == 0 || next_offset_ >= total_) {
            // Everything is here: drop cached games RomM no longer has.
            if (static_cast<int>(games_.size()) > next_offset_) games_.resize(static_cast<size_t>(next_offset_));
            next_offset_ = total_ = static_cast<int>(games_.size());
            from_cache_ = false;
        }
        if (result.offset < static_cast<int>(kCacheGames)) {
            write_file_atomic(cache_path(current_tab()), cache_json(total_, games_, kCacheGames));
        }
        if (selected_ >= games().size()) selected_ = games().empty() ? 0 : games().size() - 1;
        if (view_ == View::Loading) {
            view_ = View::Library;
            status_.clear();
        } else if (view_ == View::Library && status_.rfind("offline", 0) == 0) {
            status_.clear();
        }
        maybe_load_more();
        touch();
        return;
    }
    if (result.what == Result::What::Platforms || result.offset == -2) {
        if (result.generation != generation_) return;
        if (result.offset == -2) {  // couldn't reach RomM for systems
            if (platforms_.empty()) {
                view_ = View::Error;
                status_ = result.text;
            } else if (view_ == View::Library) {
                status_ = "offline, showing saved list (" + result.text + ")";
            }
            touch();
            return;
        }
    }
    if (result.generation != generation_) return;
    switch (result.what) {
    case Result::What::Saves:
    case Result::What::SaveUp:
        break;
    case Result::What::SaveDown: {
        if (!result.text.empty()) {
            // Don't start on the older PET save behind the user's back: that
            // session's save would look newest and replace RomM's.
            play_local_rom_ = downloading_id_;
            status_ = "RomM's newer save didn't load. ENTER: PET save, no upload";
            view_ = View::Details;
            break;
        }
        status_ = "save synced from RomM";
        for (const auto& candidate : games_) {
            if (candidate.id == downloading_id_) {
                play(candidate);
                return;
            }
        }
        view_ = View::Details;
        break;
    }
    case Result::What::Error:
        view_ = View::Error;
        status_ = result.text;
        break;
    case Result::What::Platforms: {
        const bool from_cache = result.offset == -1;
        const bool first = platforms_.empty();
        const int current_id = platform_ < platforms_.size() ? platforms_[platform_].id : 0;
        if (!from_cache && !result.platforms.empty()) {
            write_file_atomic(cache_path("platforms"), platforms_cache_json(result.platforms));
        }
        // Only the [retro] systems= tabs (in that order); without the list, ALL then everything.
        auto shown = filter_platforms(std::move(result.platforms), config_.retro_systems);
        if (shown.empty() && !config_.retro_systems.empty()) {
            if (from_cache) break;  // RomM's own answer decides
            platforms_.clear();
            games_.clear();
            view_ = View::Error;
            status_ = "None of [retro] systems= is on RomM. Check the slugs in alicepet.local.conf.";
            break;
        }
        platforms_.clear();
        if (config_.retro_systems.empty()) {
            int total = 0;
            for (const auto& p : shown) total += p.rom_count;
            platforms_.push_back({0, "", "ALL", total});
        }
        for (auto& p : shown) platforms_.push_back(std::move(p));
        platform_ = 0;
        bool tab_kept = false;
        for (size_t i = 0; i < platforms_.size(); ++i) {
            if (platforms_[i].id == current_id) {
                platform_ = i;
                tab_kept = true;
            }
        }
        // Fresh systems after cached ones: the tab's list is already loading. If the
        // tab we were on is gone, games_ still holds its roms: reload the first tab.
        if (first || !tab_kept) {
            load_games();
            return;
        }
        break;
    }
    case Result::What::Games:
        break;  // handled above (own generation)
    case Result::What::Download: {
        const Game* g = nullptr;
        for (const auto& candidate : games_) {
            if (candidate.id == downloading_id_) g = &candidate;
        }
        status_ = "saved on the PET";
        view_ = View::Details;
        details_play_ = true;
        if (g && play_after_download_) {
            start(*g);
            return;
        }
        break;
    }
    }
    touch();
}

bool RetroChip::handle_key(Key key) {
    switch (view_) {
    case View::Loading:
        if (key == Key::Escape) return false;
        break;
    case View::Error:
        if (key == Key::Escape) {
            if (!games_.empty()) {
                view_ = View::Library;
                break;
            }
            return false;
        }
        if (key == Key::Enter) {
            if (platforms_.empty()) load_platforms();
            else load_games();
        }
        break;
    case View::Library: {
        const size_t count = games().size();
        if (searching_) {
            if (key == Key::Escape) {
                searching_ = false;
                search_.clear();
                selected_ = 0;
            } else if (key == Key::Enter) {
                searching_ = false;
            } else if (key == Key::Backspace && !search_.empty()) {
                utf8_drop_last(search_);
                selected_ = 0;
            } else if (key == Key::Up && selected_ > 0) {
                --selected_;
            } else if (key == Key::Down && selected_ + 1 < count) {
                ++selected_;
            } else if (key == Key::Left && count) {
                selected_ = selected_ > kPageStep ? selected_ - kPageStep : 0;
            } else if (key == Key::Right && count) {
                selected_ = std::min(selected_ + kPageStep, count - 1);
            }
            maybe_load_more();
            break;
        }
        if (key == Key::Escape) {
            if (!search_.empty()) {
                search_.clear();
                selected_ = 0;
                break;
            }
            return false;
        }
        // Up/Down step one game (and the PC's mouse wheel, one notch); Left/Right a page.
        if (key == Key::Up && selected_ > 0) --selected_;
        if (key == Key::Down && selected_ + 1 < count) ++selected_;
        if (key == Key::Left) selected_ = selected_ > kPageStep ? selected_ - kPageStep : 0;
        if (key == Key::Right && count > 0) selected_ = std::min(selected_ + kPageStep, count - 1);
        if (key == Key::Right || key == Key::Down) maybe_load_more();
        if (key == Key::Tab && !platforms_.empty()) {
            platform_ = (platform_ + 1) % platforms_.size();
            search_.clear();
            load_games();
        }
        if (key == Key::Enter && game()) {
            view_ = View::Details;
            details_play_ = true;
            status_.clear();
            fetch_saves(*game());
        }
        break;
    }
    case View::Details:
        if (key == Key::Escape) {
            view_ = View::Library;
            start_pending_rom_ = 0;
        }
        if ((key == Key::Left || key == Key::Right) && game() && !downloaded(*game())) details_play_ = !details_play_;
        if (key == Key::Enter) {
            if (const Game* g = game(); g && details_play_ && pc_link::connected()) {
                play_on_pc(*g);
            } else if (const Game* g = game()) {
                if (!details_play_) {
                    if (!downloaded(*g)) download(*g, false);
                } else if (downloaded(*g)) {
                    start(*g);
                } else {
                    download(*g, true);
                }
            }
        }
        break;
    case View::Downloading:
        if (key == Key::Escape) {
            ++generation_;
            view_ = View::Details;
            status_ = "download cancelled";
        }
        break;
    case View::Playing:
        // The emulator reads the keyboard too; only a held ESC forces it closed.
        if (key == Key::HoldEscape) stop_game();
        return true;
    }
    touch();
    return true;
}

void RetroChip::handle_char(const std::string& utf8) {
    if (view_ == View::Library && searching_) {
        if (!utf8.empty() && static_cast<unsigned char>(utf8[0]) >= 0x20 && search_.size() < 24) {
            search_ += utf8;
            selected_ = 0;
            maybe_load_more();
            touch();
        }
        return;
    }
    const char c = utf8.size() == 1 ? static_cast<char>(std::tolower(static_cast<unsigned char>(utf8[0]))) : 0;
    // The keyboard's arrows are Fn+F/Z/X/C; the plain letters navigate too, as on every
    // other screen (this chip takes text, so the app doesn't map them for it).
    if (view_ == View::Library || view_ == View::Details) {
        const Key nav = c == 'z' ? Key::Left : c == 'c' ? Key::Right : c == 'f' ? Key::Up : c == 'x' ? Key::Down : Key::Digit1;
        if (nav != Key::Digit1) {
            handle_key(nav);
            return;
        }
    }
    if (view_ == View::Library && c == '/') {
        searching_ = true;
        touch();
    } else if (view_ == View::Details && c == 'o') {
        if (const Game* g = game(); g && !downloaded(*g)) download(*g, false);
    } else if (view_ == View::Error && c == 'r') {
        if (platforms_.empty()) load_platforms();
        else load_games();
    }
}

void RetroChip::tick(uint32_t elapsed_ms) {
    Result result;
    while (results_.poll(result)) on_result(std::move(result));
    if (view_ == View::Playing && player_ > 0) {
        play_ms_ += elapsed_ms;
        int status = 0;
        if (!platform::proc::alive(player_, &status)) {
            player_ = -1;
            if (play_ms_ < kQuickExitMs && status != 0) {
                view_ = View::Error;
                status_ = "Emulator quit at once (code " + std::to_string(status) +
                          "). Is RetroArch and the core installed? See [retro].";
            } else {
                view_ = View::Details;
                status_.clear();
                const Game* played = nullptr;
                for (const auto& g : games_) {
                    if (g.id == playing_id_) played = &g;
                }
                if (played && config_.retro_sync_saves && !base_.empty() &&
                    mtime_of(local_save_path(*played)) > launch_save_mtime_) {
                    if (launch_saves_known_) push_save(*played);
                    else check_then_push(*played);
                }
            }
            touch();
        }
    }
}

} // namespace viewmodel::chips
