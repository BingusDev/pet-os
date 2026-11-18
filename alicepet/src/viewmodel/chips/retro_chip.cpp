/*
 * SPDX-License-Identifier: MIT
 */

#include "viewmodel/chips/retro_chip.h"

#include "platform/json.h"
#include "platform/net.h"
#include "platform/process.h"

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
    : config_(std::move(config)), dir_(platform::proc::expand_home(config_.retro_dir)) {
    base_ = config_.romm_url;
    while (!base_.empty() && base_.back() == '/') base_.pop_back();
}

RetroChip::~RetroChip() {
    if (player_ > 0) platform::proc::stop(player_, SIGTERM, 1500);
}

std::string RetroChip::short_label(const std::string& slug, const std::string& fallback) {
    if (slug == "genesis-slash-megadrive" || slug == "genesis") return "GEN";
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

std::string RetroChip::multipart_body(const std::string& boundary, const std::string& field,
                                      const std::string& file_name, const std::string& bytes) {
    std::string body = "--" + boundary + "\r\n";
    body += "Content-Disposition: form-data; name=\"" + field + "\"; filename=\"" + file_name + "\"\r\n";
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

std::vector<RetroChip::Game> RetroChip::parse_games(const std::string& text) {
    std::vector<Game> out;
    platform::json::Value root;
    if (!platform::json::parse(text, root)) return out;
    // RomM 3.5+ pages results ({"items": [...]}); older servers return the array.
    const auto& list = root.is_array() ? root : root["items"];
    for (const auto& r : list.items()) {
        Game game;
        game.id = static_cast<int>(r["id"].integer());
        game.name = r["name"].str(r["fs_name_no_ext"].str(r["fs_name"].str()));
        game.file = r["fs_name"].str(r["file_name"].str());
        game.platform = r["platform_slug"].str();
        game.size = r["fs_size_bytes"].integer(r["file_size_bytes"].integer());
        const long long released_ms = r["metadatum"]["first_release_date"].integer(r["first_release_date"].integer());
        if (released_ms > 0) game.year = 1970 + static_cast<int>(released_ms / 31556952000LL);
        if (game.id > 0 && !game.file.empty()) out.push_back(std::move(game));
    }
    std::sort(out.begin(), out.end(), [](const Game& a, const Game& b) { return lower(a.name) < lower(b.name); });
    return out;
}

std::vector<std::string> RetroChip::headers() const {
    if (config_.romm_token.empty()) return {};
    return {"Authorization: Bearer " + config_.romm_token};
}

void RetroChip::enter() {
    if (!loaded_) load_platforms();
}

void RetroChip::load_platforms() {
    loaded_ = true;
    if (base_.empty()) {
        view_ = View::Error;
        status_ = "No RomM server. Set [romm] url= (and token=) in alicepet.local.conf.";
        touch();
        return;
    }
    view_ = View::Loading;
    status_ = "loading systems...";
    const size_t gen = ++generation_;
    results_.run([url = base_ + "/api/platforms", headers = headers(), gen]() {
        Result result;
        result.generation = gen;
        const auto response = platform::net::http_request("GET", url, "", headers, kApiTimeoutMs);
        if (!response.ok || response.status != 200) {
            result.text = response.ok ? "RomM said HTTP " + std::to_string(response.status) : "RomM: " + response.error;
            return result;
        }
        result.what = Result::What::Platforms;
        result.platforms = parse_platforms(response.body);
        return result;
    });
    touch();
}

void RetroChip::load_games() {
    view_ = View::Loading;
    status_ = "loading games...";
    games_.clear();
    selected_ = 0;
    const int platform_id = platform_ < platforms_.size() ? platforms_[platform_].id : 0;
    std::string url = base_ + "/api/roms?limit=1000&order_by=name";
    if (platform_id > 0) url += "&platform_id=" + std::to_string(platform_id);
    const size_t gen = ++generation_;
    results_.run([url, headers = headers(), gen]() {
        Result result;
        result.generation = gen;
        const auto response = platform::net::http_request("GET", url, "", headers, kApiTimeoutMs, 16 * 1024 * 1024);
        if (!response.ok || response.status != 200) {
            result.text = response.ok ? "RomM said HTTP " + std::to_string(response.status) : "RomM: " + response.error;
            return result;
        }
        result.what = Result::What::Games;
        result.games = parse_games(response.body);
        return result;
    });
    touch();
}

std::string RetroChip::local_path(const Game& game) const {
    return dir_ + "/" + (game.platform.empty() ? "other" : game.platform) + "/" + game.file;
}

bool RetroChip::downloaded(const Game& game) const {
    std::error_code ec;
    return fs::exists(local_path(game), ec);
}

std::string RetroChip::local_save_path(const Game& game) const {
    const std::string stem = fs::path(game.file).stem().string();
    return dir_ + "/" + (game.platform.empty() ? "other" : game.platform) + "/saves/" + stem + ".srm";
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
    saves_known_ = false;
    saves_checking_ = true;
    saves_failed_ = false;
    saves_.clear();
    results_.run([url = base_ + "/api/saves?rom_id=" + std::to_string(game.id), headers = headers(), rom = game.id]() {
        Result result;
        result.rom_id = rom;
        const auto response = platform::net::http_request("GET", url, "", headers, kApiTimeoutMs);
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
    results_.run([url, headers = headers(), path = local_path(game), gen]() {
        Result result;
        result.generation = gen;
        const auto response = platform::net::http_request("GET", url, "", headers, kDownloadTimeoutMs, kMaxRomBytes);
        if (!response.ok || response.status != 200) {
            result.text = response.ok ? "download refused (HTTP " + std::to_string(response.status) + ")"
                                      : "download failed: " + response.error;
            return result;
        }
        std::error_code ec;
        fs::create_directories(fs::path(path).parent_path(), ec);
        const std::string tmp = path + ".part";
        {
            std::ofstream out(tmp, std::ios::binary);
            out.write(response.body.data(), static_cast<std::streamsize>(response.body.size()));
            if (!out) {
                result.text = "can't write " + tmp;
                return result;
            }
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

std::vector<const RetroChip::Game*> RetroChip::games() const {
    std::vector<const Game*> out;
    const std::string needle = lower(search_);
    for (const auto& g : games_) {
        if (needle.empty() || lower(g.name).find(needle) != std::string::npos) out.push_back(&g);
    }
    return out;
}

const RetroChip::Game* RetroChip::game() const {
    const auto list = games();
    return selected_ < list.size() ? list[selected_] : nullptr;
}

void RetroChip::on_result(Result result) {
    if (result.what == Result::What::Saves) {
        if (result.rom_id != saves_rom_) return;  // the user moved on to another game
        saves_checking_ = false;
        saves_failed_ = !result.text.empty();
        saves_known_ = !saves_failed_;
        saves_ = std::move(result.saves);
        touch();
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
        platforms_.clear();
        int total = 0;
        for (const auto& p : result.platforms) total += p.rom_count;
        platforms_.push_back({0, "", "ALL", total});
        for (auto& p : result.platforms) platforms_.push_back(std::move(p));
        platform_ = 0;
        load_games();
        return;
    }
    case Result::What::Games:
        games_ = std::move(result.games);
        selected_ = 0;
        view_ = View::Library;
        status_.clear();
        break;
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
        if (key == Key::Enter) load_platforms();
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
                search_.pop_back();
                selected_ = 0;
            } else if (key == Key::Left && selected_ > 0) {
                --selected_;
            } else if (key == Key::Right && selected_ + 1 < count) {
                ++selected_;
            }
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
        if (key == Key::Left && selected_ > 0) --selected_;
        if (key == Key::Right && selected_ + 1 < count) ++selected_;
        if ((key == Key::Tab || key == Key::Down) && !platforms_.empty()) {
            platform_ = (platform_ + 1) % platforms_.size();
            search_.clear();
            load_games();
        } else if (key == Key::Up && !platforms_.empty()) {
            platform_ = (platform_ + platforms_.size() - 1) % platforms_.size();
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
        if (key == Key::Escape) view_ = View::Library;
        if ((key == Key::Left || key == Key::Right) && game() && !downloaded(*game())) details_play_ = !details_play_;
        if (key == Key::Enter) {
            if (const Game* g = game()) {
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
            touch();
        }
        return;
    }
    const char c = utf8.size() == 1 ? static_cast<char>(std::tolower(static_cast<unsigned char>(utf8[0]))) : 0;
    if (view_ == View::Library && c == '/') {
        searching_ = true;
        touch();
    } else if (view_ == View::Details && c == 'o') {
        if (const Game* g = game(); g && !downloaded(*g)) download(*g, false);
    } else if (view_ == View::Error && c == 'r') {
        load_platforms();
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
                    push_save(*played);
                }
            }
            touch();
        }
    }
}

} // namespace viewmodel::chips
