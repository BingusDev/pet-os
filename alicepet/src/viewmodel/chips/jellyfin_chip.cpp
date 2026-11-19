/*
 * SPDX-License-Identifier: MIT
 */

#include "viewmodel/chips/jellyfin_chip.h"

#include "platform/json.h"
#include "platform/mpv_ipc.h"
#include "platform/net.h"
#include "platform/process.h"
#include "viewmodel/alice_viewmodel.h"

#include <algorithm>
#include <cctype>
#include <csignal>
#include <cstdio>
#include <fcntl.h>
#include <unistd.h>

#ifndef APP_VERSION
#define APP_VERSION "0.0.0"
#endif

namespace viewmodel::chips {
namespace {

constexpr long long kTicksPerSecond = 10000000LL;
constexpr int kTimeoutMs = 10000;
constexpr uint32_t kPollMs = 2000;
constexpr uint32_t kReportMs = 30000;

std::string lower(std::string value) {
    for (char& c : value) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return value;
}

JellyfinChip::Item parse_item(const platform::json::Value& v) {
    JellyfinChip::Item item;
    item.id = v["Id"].str();
    item.name = AliceViewModel::screen_text(v["Name"].str());
    item.type = v["Type"].str();
    item.year = static_cast<int>(v["ProductionYear"].integer());
    item.child_count = static_cast<int>(v["ChildCount"].integer());
    item.index = static_cast<int>(v["IndexNumber"].integer());
    item.season = static_cast<int>(v["ParentIndexNumber"].integer());
    item.runtime_ticks = v["RunTimeTicks"].integer();
    item.played = v["UserData"]["Played"].boolean();
    item.position_ticks = v["UserData"]["PlaybackPositionTicks"].integer();
    return item;
}

std::string host_name() {
    char name[64] = {};
    gethostname(name, sizeof(name) - 1);
    return name[0] ? name : "pet";
}

} // namespace

JellyfinChip::JellyfinChip(model::PetConfig config) : config_(std::move(config)) {
    base_ = config_.jellyfin_url;
    while (!base_.empty() && base_.back() == '/') base_.pop_back();
    device_id_ = "alicepet-" + host_name();
    ipc_path_ = "/tmp/alicepet-mpv-" + std::to_string(getpid()) + ".sock";
    mpv_conf_path_ = "/tmp/alicepet-mpv-" + std::to_string(getpid()) + ".conf";
}

JellyfinChip::~JellyfinChip() {
    if (player_ > 0) platform::proc::stop(player_, SIGTERM, 500);
    std::remove(mpv_conf_path_.c_str());
}

std::string JellyfinChip::auth_header() const {
    return "Authorization: MediaBrowser Client=\"AlicePET\", Device=\"CardputerZero\", DeviceId=\"" + device_id_ +
           "\", Version=\"" APP_VERSION "\", Token=\"" + config_.jellyfin_api_key + "\"";
}

std::string JellyfinChip::clock_text(long long ticks) {
    const long long total = std::max(0LL, ticks / kTicksPerSecond);
    char out[24];
    if (total >= 3600) {
        std::snprintf(out, sizeof(out), "%lld:%02lld:%02lld", total / 3600, (total / 60) % 60, total % 60);
    } else {
        std::snprintf(out, sizeof(out), "%lld:%02lld", total / 60, total % 60);
    }
    return out;
}

void JellyfinChip::enter() {
    if (!loaded_) load_library();
}

void JellyfinChip::load_library() {
    loaded_ = true;
    if (base_.empty() || config_.jellyfin_api_key.empty()) {
        view_ = View::Error;
        status_ = "Set [jellyfin] url= and api_key= in /etc/alicepet.conf.";
        touch();
        return;
    }
    view_ = View::Loading;
    status_ = "Contacting Jellyfin...";
    const size_t gen = ++generation_;
    results_.run([base = base_, auth = auth_header(), want = config_.jellyfin_user, user = user_id_,
                  types = kind_ == Kind::Shows ? "Series" : "Movie", gen]() {
        Result result{Result::What::Error, {}, {}, "", 0, gen};
        std::string uid = user;
        if (uid.empty()) {
            const auto users = platform::net::http_get(base + "/Users", {auth}, kTimeoutMs);
            platform::json::Value list;
            if (!users.ok || users.status != 200 || !platform::json::parse(users.body, list) || list.size() == 0) {
                result.text = users.ok ? "Jellyfin said HTTP " + std::to_string(users.status) + " (check api_key)."
                                       : "Can't reach Jellyfin (" + users.error + ").";
                return result;
            }
            uid = list[0]["Id"].str();
            for (const auto& u : list.items()) {
                if (!want.empty() && (u["Name"].str() == want || u["Id"].str() == want)) uid = u["Id"].str();
            }
        }
        const auto response = platform::net::http_get(
            base + "/Users/" + uid + "/Items?IncludeItemTypes=" + types +
                "&Recursive=true&SortBy=SortName&Fields=ProductionYear,ChildCount&EnableImages=false",
            {auth}, kTimeoutMs);
        platform::json::Value json;
        if (!response.ok || response.status != 200 || !platform::json::parse(response.body, json)) {
            result.text = response.ok ? "Library request failed (HTTP " + std::to_string(response.status) + ")."
                                      : "Can't reach Jellyfin (" + response.error + ").";
            return result;
        }
        result.what = Result::What::Library;
        result.text = uid;
        for (const auto& v : json["Items"].items()) result.items.push_back(parse_item(v));
        return result;
    });
    touch();
}

std::vector<const JellyfinChip::Item*> JellyfinChip::library() const {
    std::vector<const Item*> out;
    const std::string needle = lower(search_);
    for (const auto& item : library_) {
        if (needle.empty() || lower(item.name).find(needle) != std::string::npos) out.push_back(&item);
    }
    return out;
}

void JellyfinChip::open_series() {
    const auto visible = library();
    if (selected_ >= visible.size()) return;
    const Item* item = visible[selected_];
    series_index_ = static_cast<size_t>(item - library_.data());
    if (item->type != "Series") {
        return_view_ = View::Library;
        choose(*item);
        return;
    }
    seasons_.clear();
    episodes_.clear();
    season_ = 0;
    episode_ = 0;
    view_ = View::Loading;
    status_ = "Loading " + item->name + "...";
    const size_t gen = ++generation_;
    results_.run([base = base_, auth = auth_header(), uid = user_id_, id = item->id, gen]() {
        Result result{Result::What::Error, {}, {}, "", 0, gen};
        const auto response = platform::net::http_get(base + "/Shows/" + id + "/Seasons?userId=" + uid, {auth}, kTimeoutMs);
        platform::json::Value json;
        if (!response.ok || response.status != 200 || !platform::json::parse(response.body, json)) {
            result.text = "Couldn't load seasons.";
            return result;
        }
        result.what = Result::What::Seasons;
        for (const auto& v : json["Items"].items()) {
            result.seasons.push_back({v["Id"].str(), AliceViewModel::screen_text(v["Name"].str()),
                                      static_cast<int>(v["IndexNumber"].integer())});
        }
        return result;
    });
    touch();
}

void JellyfinChip::load_episodes() {
    if (season_ >= seasons_.size() || !series()) return;
    episodes_.clear();
    episode_ = 0;
    status_ = "Loading episodes...";
    const size_t gen = ++generation_;
    results_.run([base = base_, auth = auth_header(), uid = user_id_, id = series()->id,
                  season = seasons_[season_].id, gen]() {
        Result result{Result::What::Error, {}, {}, "", 0, gen};
        const auto response = platform::net::http_get(
            base + "/Shows/" + id + "/Episodes?seasonId=" + season + "&userId=" + uid + "&EnableImages=false", {auth},
            kTimeoutMs);
        platform::json::Value json;
        if (!response.ok || response.status != 200 || !platform::json::parse(response.body, json)) {
            result.text = "Couldn't load episodes.";
            return result;
        }
        result.what = Result::What::Episodes;
        for (const auto& v : json["Items"].items()) result.items.push_back(parse_item(v));
        return result;
    });
    touch();
}

void JellyfinChip::choose(const Item& item) {
    playing_ = item;
    if (item.position_ticks > 0) {
        resume_choice_ = true;
        view_ = View::Resume;
        touch();
    } else {
        start_playback(0);
    }
}

std::string JellyfinChip::stream_url(const Item& item, long long start_ticks, bool with_key) const {
    // Server-side transcode to the panel: H.264 + AAC stereo HLS, <= width x 170.
    return base_ + "/Videos/" + item.id + "/master.m3u8?MediaSourceId=" + item.id + "&DeviceId=" + device_id_ +
           "&PlaySessionId=" + play_session_ + "&VideoCodec=h264&AudioCodec=aac&MaxWidth=" +
           std::to_string(config_.jellyfin_width) + "&MaxHeight=170&VideoBitrate=" +
           std::to_string(config_.jellyfin_max_bitrate) +
           "&AudioBitrate=64000&MaxAudioChannels=2&TranscodingMaxAudioChannels=2&SegmentContainer=ts&StartTimeTicks=" +
           std::to_string(start_ticks) +
           (with_key ? "&ApiKey=" + platform::net::url_encode(config_.jellyfin_api_key) : std::string());
}

std::string JellyfinChip::write_mpv_auth_config() const {
    // mpv applies http-header-fields to the playlist and every HLS segment request.
    const int fd = ::open(mpv_conf_path_.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return "";
    // Jellyfin rejects X-Emby-Token (401); send the MediaBrowser Authorization header.
    // %len% quoting keeps mpv from parsing the quotes inside the value.
    const std::string header = "Authorization: MediaBrowser Token=\"" + config_.jellyfin_api_key + "\"";
    const std::string body = "http-header-fields=%" + std::to_string(header.size()) + "%" + header + "\n";
    const bool ok = ::write(fd, body.data(), body.size()) == static_cast<ssize_t>(body.size());
    ::close(fd);
    return ok ? mpv_conf_path_ : "";
}

void JellyfinChip::start_playback(long long start_ticks) {
    play_session_ = "pet" + std::to_string(getpid()) + "x" + std::to_string(++generation_);
    start_ticks_ = start_ticks;
    position_ticks_ = start_ticks;
    paused_ = false;
    play_ms_ = poll_ms_ = report_ms_ = 0;
    stream_info_ = "requesting transcode...";
    view_ = View::Playing;
#if USE_DESKTOP
    const std::string url = stream_url(playing_, start_ticks, false);
    const std::string token_header = "X-Emby-Token: " + config_.jellyfin_api_key;
    // STUB: the simulator has no video output. Fetch the HLS master playlist to
    // prove the server accepted the transcode, then show the playing screen.
    results_.run([url, token_header, gen = generation_]() {
        Result result{Result::What::Stream, {}, {}, "", 0, gen};
        const auto response = platform::net::http_get(url, {token_header}, kTimeoutMs);
        if (!response.ok || response.status != 200) {
            result.text = response.ok ? "transcode refused (HTTP " + std::to_string(response.status) + ")"
                                      : "stream error: " + response.error;
            return result;
        }
        const size_t res = response.body.find("RESOLUTION=");
        const size_t codecs = response.body.find("CODECS=\"");
        std::string info = "HLS OK";
        if (res != std::string::npos) info += " " + response.body.substr(res + 11, response.body.find_first_of(",\n", res) - res - 11);
        if (codecs != std::string::npos && response.body.compare(codecs + 8, 4, "avc1") == 0) info += " H.264";
        result.text = info + " · desktop stub";
        return result;
    });
#else
    auto argv = platform::proc::split_command(config_.jellyfin_player);
    ipc_ = !argv.empty() && argv[0].find("mpv") != std::string::npos;
    // mpv gets the API key as a header from a 0600 file, so it never shows in `ps`.
    // Other players only accept the key in the URL.
    const std::string auth_conf = ipc_ ? write_mpv_auth_config() : std::string();
    if (ipc_) argv.push_back("--input-ipc-server=" + ipc_path_);
    if (!auth_conf.empty()) argv.push_back("--include=" + auth_conf);
    argv.push_back(stream_url(playing_, start_ticks, auth_conf.empty()));
    player_ = platform::proc::spawn(argv);
    if (player_ < 0) {
        view_ = View::Error;
        status_ = "Player failed to start. Check [jellyfin] player=.";
    } else {
        stream_info_ = "playing";
        report("/Sessions/Playing", start_ticks, false);
    }
#endif
    touch();
}

void JellyfinChip::report(const char* endpoint, long long ticks, bool paused) {
    const std::string body = "{\"ItemId\":\"" + playing_.id + "\",\"MediaSourceId\":\"" + playing_.id +
                             "\",\"PlaySessionId\":\"" + play_session_ + "\",\"PositionTicks\":" + std::to_string(ticks) +
                             ",\"IsPaused\":" + (paused ? "true" : "false") +
                             ",\"PlayMethod\":\"Transcode\",\"CanSeek\":true}";
    const std::string url = base_ + endpoint;
    const std::string auth = auth_header();
    const bool stopped = std::string(endpoint) == "/Sessions/Playing/Stopped";
    const std::string cleanup = base_ + "/Videos/ActiveEncodings?DeviceId=" + device_id_ + "&PlaySessionId=" + play_session_;
    results_.run([url, body, auth, stopped, cleanup]() {
        platform::net::http_request("POST", url, body, {auth, "Content-Type: application/json"}, kTimeoutMs);
        if (stopped) platform::net::http_request("DELETE", cleanup, "", {auth}, kTimeoutMs);
        return Result{Result::What::Position, {}, {}, "", -1, 0};
    });
}

void JellyfinChip::stop_playback() {
    if (view_ != View::Playing) return;
    if (player_ > 0) {
        if (ipc_) mpv(R"({"command":["quit"]})");
        platform::proc::stop(player_, SIGTERM, 1500);
        player_ = -1;
        report("/Sessions/Playing/Stopped", position_ticks_, false);
        playing_.position_ticks = position_ticks_;
    } else {
#if USE_DESKTOP
        // Stub never started a session; just release the server-side transcode.
        const std::string cleanup = base_ + "/Videos/ActiveEncodings?DeviceId=" + device_id_ + "&PlaySessionId=" + play_session_;
        results_.run([cleanup, auth = auth_header()]() {
            platform::net::http_request("DELETE", cleanup, "", {auth}, kTimeoutMs);
            return Result{Result::What::Position, {}, {}, "", -1, 0};
        });
#endif
    }
    view_ = return_view_;
    touch();
}

void JellyfinChip::mpv(const std::string& json) {
    if (ipc_ && player_ > 0) platform::mpv::command(ipc_path_, json, nullptr, 200);
}

bool JellyfinChip::handle_key(Key key) {
    switch (view_) {
    case View::Loading:
    case View::Error:
        if (key == Key::Escape) {
            if (!library_.empty()) {
                ++generation_;  // drop whatever is still loading
                view_ = View::Library;
                touch();
                return true;
            }
            return false;
        }
        if (key == Key::Enter && view_ == View::Error) load_library();
        return true;

    case View::Library: {
        const size_t count = library().size();
        if (searching_) {
            if (key == Key::Escape) {
                search_.clear();
                searching_ = false;
                selected_ = 0;
            } else if (key == Key::Enter) {
                searching_ = false;
            } else if (key == Key::Backspace && !search_.empty()) {
                search_.pop_back();
                selected_ = 0;
            }
            touch();
            return true;
        }
        if (key == Key::Escape) return false;
        if (key == Key::Left && count) selected_ = (selected_ + count - 1) % count;
        else if (key == Key::Right && count) selected_ = (selected_ + 1) % count;
        else if (key == Key::Enter && count) open_series();
        else if (key == Key::Tab) {
            kind_ = kind_ == Kind::Shows ? Kind::Movies : Kind::Shows;
            library_.clear();
            selected_ = 0;
            search_.clear();
            load_library();
        }
        touch();
        return true;
    }

    case View::Episodes:
        if (key == Key::Escape) {
            view_ = View::Library;
        } else if (key == Key::Up && !episodes_.empty()) {
            episode_ = (episode_ + episodes_.size() - 1) % episodes_.size();
        } else if (key == Key::Down && !episodes_.empty()) {
            episode_ = (episode_ + 1) % episodes_.size();
        } else if ((key == Key::Left || key == Key::Right) && seasons_.size() > 1) {
            season_ = key == Key::Left ? (season_ + seasons_.size() - 1) % seasons_.size() : (season_ + 1) % seasons_.size();
            load_episodes();
        } else if (key == Key::Enter && episode_ < episodes_.size()) {
            return_view_ = View::Episodes;
            choose(episodes_[episode_]);
        }
        touch();
        return true;

    case View::Resume:
        if (key == Key::Escape) view_ = return_view_;
        else if (key == Key::Left || key == Key::Right || key == Key::Up || key == Key::Down) resume_choice_ = !resume_choice_;
        else if (key == Key::Enter) start_playback(resume_choice_ ? playing_.position_ticks : 0);
        touch();
        return true;

    case View::Playing:
        if (key == Key::Escape) stop_playback();
        else if (key == Key::Left) mpv(R"({"command":["seek",-10]})");
        else if (key == Key::Right) mpv(R"({"command":["seek",10]})");
        else if (key == Key::Up) mpv(R"({"command":["add","volume",5]})");
        else if (key == Key::Down) mpv(R"({"command":["add","volume",-5]})");
        touch();
        return true;
    }
    return true;
}

void JellyfinChip::handle_char(const std::string& utf8) {
    if (view_ == View::Library) {
        if (searching_) {
            if (search_.size() < 24) search_ += utf8;
            selected_ = 0;
            touch();
        } else if (utf8 == "/") {
            searching_ = true;
            touch();
        }
    } else if (view_ == View::Playing) {
        if (utf8 == " ") {
            paused_ = !paused_;
            mpv(R"({"command":["cycle","pause"]})");
            touch();
        } else if (utf8 == "s" || utf8 == "S") {
            mpv(R"({"command":["cycle","sub"]})");
        }
    }
}

void JellyfinChip::tick(uint32_t elapsed_ms) {
    Result result;
    while (results_.poll(result)) {
        if (result.what == Result::What::Position) {
            if (result.seconds >= 0 && view_ == View::Playing) {
                position_ticks_ = start_ticks_ + static_cast<long long>(result.seconds * kTicksPerSecond);
                touch();
            }
            continue;
        }
        if (result.generation != generation_) continue;  // stale
        switch (result.what) {
        case Result::What::Library:
            user_id_ = result.text;
            library_ = std::move(result.items);
            selected_ = std::min(selected_, library_.empty() ? 0 : library_.size() - 1);
            view_ = View::Library;
            break;
        case Result::What::Seasons:
            seasons_ = std::move(result.seasons);
            // Start on Season 1 rather than Specials when both exist.
            season_ = 0;
            for (size_t i = 0; i < seasons_.size(); ++i) {
                if (seasons_[i].index >= 1) {
                    season_ = i;
                    break;
                }
            }
            view_ = View::Episodes;
            load_episodes();
            break;
        case Result::What::Episodes:
            episodes_ = std::move(result.items);
            // Jump to the first unwatched episode.
            episode_ = 0;
            for (size_t i = 0; i < episodes_.size(); ++i) {
                if (!episodes_[i].played) {
                    episode_ = i;
                    break;
                }
            }
            status_.clear();
            break;
        case Result::What::Stream:
            stream_info_ = result.text;
            break;
        case Result::What::Error:
            view_ = View::Error;
            status_ = result.text;
            break;
        case Result::What::User:
        case Result::What::Position:
            break;
        }
        touch();
    }

    if (view_ == View::Playing && player_ > 0) {
        play_ms_ += elapsed_ms;
        poll_ms_ += elapsed_ms;
        report_ms_ += elapsed_ms;
        if (!platform::proc::alive(player_)) {
            player_ = -1;
            // Player quit by itself (end of file / error): report where it stopped.
            report("/Sessions/Playing/Stopped", position_ticks_, false);
            view_ = return_view_;
            touch();
            return;
        }
        if (poll_ms_ >= kPollMs) {
            poll_ms_ = 0;
            if (ipc_) {
                results_.run([path = ipc_path_]() {
                    double seconds = -1;
                    platform::mpv::time_pos(path, seconds);
                    return Result{Result::What::Position, {}, {}, "", seconds, 0};
                });
            } else if (!paused_) {
                position_ticks_ = start_ticks_ + static_cast<long long>(play_ms_) * (kTicksPerSecond / 1000);
            }
        }
        if (report_ms_ >= kReportMs) {
            report_ms_ = 0;
            report("/Sessions/Playing/Progress", position_ticks_, paused_);
        }
    }
}

} // namespace viewmodel::chips
