/*
 * SPDX-License-Identifier: MIT
 */

#include "viewmodel/chips/retro_chip.h"

#include "platform/json.h"
#include "platform/net.h"
#include "platform/process.h"

#include <signal.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>

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
    auto argv = platform::proc::split_command(config_.retro_player);
    for (auto& arg : argv) {
        arg = replace_all(arg, "{core}", core->second);
        arg = replace_all(arg, "{rom}", local_path(game));
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
    if (result.generation != generation_) return;
    switch (result.what) {
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
            play(*g);
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
                    play(*g);
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
                status_ = "saves are kept on the PET";
            }
            touch();
        }
    }
}

} // namespace viewmodel::chips
