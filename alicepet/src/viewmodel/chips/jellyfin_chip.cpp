/*
 * SPDX-License-Identifier: MIT
 */

#include "viewmodel/chips/jellyfin_chip.h"

#include "platform/json.h"
#include "platform/mpv_ipc.h"
#include "platform/net.h"
#include "platform/process.h"
#include "viewmodel/chips/jack_in_chip.h"
#include "viewmodel/alice_viewmodel.h"

#include <thread>
#include <chrono>
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
constexpr const char* kPcKeyApp = "AlicePET-JackIn-";
}
namespace {

constexpr long long kTicksPerSecond = 10000000LL;
constexpr int kTimeoutMs = 10000;
constexpr uint32_t kPollMs = 2000;
constexpr uint32_t kReportMs = 30000;

std::string lower(std::string value) {
    for (char& c : value) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return value;
}

} // namespace

bool pc_direct_play(const std::string& container, const std::string& video, const std::string& audio) {
    // Windows Media Player plays MP4/M4V/MOV with H.264 video and AAC/MP3 (or no) audio as they are,
    // with seeking. MKV does not play reliably there, so it takes the H.264/AAC MP4 transcode.
    const bool mp4_container = container.find("mp4") != std::string::npos ||
                               container.find("m4v") != std::string::npos ||
                               container.find("mov") != std::string::npos;
    return mp4_container && video == "h264" && (audio.empty() || audio == "aac" || audio == "mp3");
}

namespace {

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
    if (!pc_key_.empty()) {
        platform::net::http_request("DELETE", base_ + "/Auth/Keys/" + pc_key_, "", {auth_header()}, 1500);
    }
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
    const std::string id = platform::net::url_encode(item.id);
    return base_ + "/Videos/" + id + "/master.m3u8?MediaSourceId=" + id + "&DeviceId=" + device_id_ +
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
    play_method_ = "Transcode";
    if (pc_link::connected()) {
        start_pc_playback(start_ticks);
        touch();
        return;
    }
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
                             ",\"PlayMethod\":\"" + play_method_ + "\",\"CanSeek\":true}";
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
    if (pc_playing_) {
        pc_link::send("{\"t\":\"stop_play\",\"item\":\"" + platform::net::json_escape(playing_.id) + "\"}");
        finish_pc_playback();
        return;
    }
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
                utf8_drop_last(search_);
                selected_ = 0;
            }
            touch();
            return true;
        }
        if (key == Key::Escape) {
            if (search_.empty()) return false;
            search_.clear();  // Esc clears a filter before it leaves the chip
            selected_ = 0;
        } else if (key == Key::Up && selected_ > 0) {
            --selected_;
        } else if (key == Key::Down && selected_ + 1 < count) {
            ++selected_;
        } else if (key == Key::Left && count) {
            selected_ = selected_ >= kMediaPageStep ? selected_ - kMediaPageStep : 0;
        } else if (key == Key::Right && count) {
            selected_ = std::min(selected_ + kMediaPageStep, count - 1);
        } else if (key == Key::Enter && count) open_series();
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
        } else if (key == Key::Up && episode_ > 0) {
            --episode_;
        } else if (key == Key::Down && episode_ + 1 < episodes_.size()) {
            ++episode_;
        } else if (key == Key::Left && !episodes_.empty()) {
            episode_ = episode_ >= kMediaPageStep ? episode_ - kMediaPageStep : 0;
        } else if (key == Key::Right && !episodes_.empty()) {
            episode_ = std::min(episode_ + kMediaPageStep, episodes_.size() - 1);
        } else if (key == Key::Tab && seasons_.size() > 1) {
            // Season switch lives on TAB (as RETRO's TAB switches system), not Left/Right.
            season_ = (season_ + 1) % seasons_.size();
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
        if (pc_playing_) {
            // Remote control for the PC app's player.
            const auto cmd = [&](const char* what, int by) {
                pc_link::send(std::string("{\"t\":\"player\",\"cmd\":\"") + what + "\",\"by\":" + std::to_string(by) + "}");
            };
            if (key == Key::Escape) stop_playback();
            else if (key == Key::Left) cmd("seek", -10);
            else if (key == Key::Right) cmd("seek", 10);
            else if (key == Key::Up) cmd("volume", 5);
            else if (key == Key::Down) cmd("volume", -5);
            touch();
            return true;
        }
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
        if (utf8 == " " && pc_playing_) {
            pc_link::send(R"({"t":"player","cmd":"pause","by":0})");
        } else if (utf8 == " ") {
            paused_ = !paused_;
            mpv(R"({"command":["cycle","pause"]})");
            touch();
        } else if (utf8 == "s" || utf8 == "S") {
            mpv(R"({"command":["cycle","sub"]})");
        }
    }
}

void JellyfinChip::on_result(Result result) {
    if (result.what == Result::What::Position) {
        if (result.seconds >= 0 && view_ == View::Playing) {
            position_ticks_ = start_ticks_ + static_cast<long long>(result.seconds * kTicksPerSecond);
            touch();
        }
        return;
    }
    if (result.generation != generation_) {
        revoke_key(result.key);  // a PC key made for a play that is already over
        return;                  // stale
    }
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
        case Result::What::PcStream: {
            if (!pc_playing_ || view_ != View::Playing) {
                revoke_key(result.key);  // stopped before the stream was ready
                break;
            }
            const std::string url = pc_link::relay_file(result.text, {auth_header()}, result.name);
            if (url.empty()) {
                revoke_key(result.key);
                pc_playing_ = false;
                view_ = View::Error;
                status_ = "Couldn't start the media relay for the PC.";
                break;
            }
            pc_direct_ = result.direct;
            pc_key_ = result.key;
            const std::string direct_url =
                result.key.empty() ? std::string() : result.text + "&api_key=" + platform::net::url_encode(result.key);
            play_method_ = result.direct ? "DirectPlay" : "Transcode";
            const double start = static_cast<double>(start_ticks_) / kTicksPerSecond;
            char nums[96];
            // `start`: where the PC seeks to; `base`: the stream's own 0 in show time.
            std::snprintf(nums, sizeof(nums), "\"start\":%.1f,\"base\":%.1f", result.direct ? start : 0.0,
                          result.direct ? 0.0 : start);
            const double length = static_cast<double>(playing_.runtime_ticks) / kTicksPerSecond;
            char len[40];
            std::snprintf(len, sizeof(len), "\"length\":%.1f", length);
            pc_link::send("{\"t\":\"play\",\"kind\":\"video\",\"title\":\"" + platform::net::json_escape(pc_title()) +
                          "\",\"url\":\"" + platform::net::json_escape(url) + "\",\"direct_url\":\"" +
                          platform::net::json_escape(direct_url) + "\"," + nums + "," + len +
                          ",\"seekable\":" + (result.direct ? "true" : "false") + ",\"item\":\"" +
                          platform::net::json_escape(playing_.id) + "\"}");
            stream_info_ = std::string(result.direct ? "direct play" : "transcoding") + " on " + pc_link::host();
            report("/Sessions/Playing", start_ticks_, false);
            break;
        }
        case Result::What::Error:
            pc_playing_ = false;
            view_ = View::Error;
            status_ = result.text;
            break;
        case Result::What::User:
        case Result::What::Position:
            break;
        }
    touch();
}

void JellyfinChip::tick(uint32_t elapsed_ms) {
    Result result;
    while (results_.poll(result)) on_result(std::move(result));

    if (pc_playing_) {
        pc_tick();
        report_ms_ += elapsed_ms;
        if (pc_playing_ && report_ms_ >= kReportMs) {
            report_ms_ = 0;
            report("/Sessions/Playing/Progress", position_ticks_, paused_);
        }
        return;
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

// --- playing in the PC app (jacked in) ---------------------------------------------

std::string JellyfinChip::pc_title() const {
    if (playing_.type != "Episode") return playing_.name;
    std::string title = series() ? series()->name + " " : std::string();
    title += "S" + std::to_string(playing_.season) + "E" + std::to_string(playing_.index) + " " + playing_.name;
    return title;
}

void JellyfinChip::start_pc_playback(long long start_ticks) {
    pc_playing_ = true;
    stream_info_ = "asking Jellyfin for a PC stream...";
    report_ms_ = 0;
    results_.run([base = base_, auth = auth_header(), uid = user_id_, item = playing_.id, device = device_id_,
                  session = play_session_, start_ticks, gen = generation_, offer_key = config_.jellyfin_pc_direct]() {
        Result result{Result::What::Error, {}, {}, "", 0, gen};
        const auto response = platform::net::http_get(
            base + "/Users/" + platform::net::url_encode(uid) + "/Items/" + platform::net::url_encode(item), {auth}, kTimeoutMs);
        platform::json::Value json;
        if (!response.ok || response.status != 200 || !platform::json::parse(response.body, json)) {
            result.text = response.ok ? "Jellyfin said HTTP " + std::to_string(response.status) + " for the item."
                                      : "Can't reach Jellyfin (" + response.error + ").";
            return result;
        }
        const auto& source = json["MediaSources"][0];
        const std::string source_id = source["Id"].str(item);
        const std::string container = lower(source["Container"].str());
        std::string video, audio;
        for (const auto& stream : source["MediaStreams"].items()) {
            if (stream["Type"].str() == "Video" && video.empty()) video = lower(stream["Codec"].str());
            if (stream["Type"].str() == "Audio" && audio.empty()) audio = lower(stream["Codec"].str());
        }
        const bool direct = pc_direct_play(container, video, audio);
        const std::string enc_item = platform::net::url_encode(item);
        const std::string ids = "MediaSourceId=" + platform::net::url_encode(source_id) + "&DeviceId=" +
                                platform::net::url_encode(device) + "&PlaySessionId=" + platform::net::url_encode(session);
        result.what = Result::What::PcStream;
        result.direct = direct;
        // [jellyfin] pc_direct=on only: a key just for this play, so the PC can
        // stream from Jellyfin itself. Jellyfin keys can't be scoped (each one is
        // an administrator key), so by default none is made and the PC only gets
        // the PET's relay URL. A key lives only while the video plays: the PET
        // deletes it on stop, and any left over from a crash or a power loss
        // (same app-name prefix) is deleted here first, whatever the setting.
        const auto keys = platform::net::http_get(base + "/Auth/Keys", {auth}, kTimeoutMs);
        platform::json::Value list;
        if (keys.ok && keys.status == 200 && platform::json::parse(keys.body, list)) {
            for (const auto& k : list["Items"].items()) {
                if (k["AppName"].str().rfind(kPcKeyApp, 0) == 0 && !k["AccessToken"].str().empty()) {
                    platform::net::http_request("DELETE", base + "/Auth/Keys/" + platform::net::url_encode(k["AccessToken"].str()), "", {auth},
                                                kTimeoutMs);
                }
            }
            const std::string app = std::string(kPcKeyApp) + session;
            const auto made = offer_key ? platform::net::http_request("POST", base + "/Auth/Keys?app=" + platform::net::url_encode(app),
                                                                      "", {auth}, kTimeoutMs)
                                        : platform::net::HttpResponse{};
            const auto again =
                made.ok && made.status / 100 == 2 ? platform::net::http_get(base + "/Auth/Keys", {auth}, kTimeoutMs)
                                                  : platform::net::HttpResponse{};
            platform::json::Value now;
            if (again.ok && again.status == 200 && platform::json::parse(again.body, now)) {
                for (const auto& k : now["Items"].items()) {
                    if (k["AppName"].str() == app) result.key = k["AccessToken"].str();
                }
            }
        }
        if (direct) {
            result.text = base + "/Videos/" + enc_item + "/stream.mp4?Static=true&" + ids;
            result.name = "video.mp4";
        } else {
            result.text = base + "/Videos/" + enc_item + "/stream.mp4?" + ids +
                          "&VideoCodec=h264&AudioCodec=aac&VideoBitrate=8000000&AudioBitrate=192000"
                          "&MaxAudioChannels=2&TranscodingMaxAudioChannels=2&MaxWidth=1920&StartTimeTicks=" +
                          std::to_string(start_ticks);
            result.name = "video.mp4";
        }
        return result;
    });
}

void JellyfinChip::pc_tick() {
    pc_link::Progress p;
    while (pc_link::take_progress(p)) {
        if (!pc_playing_ || p.item != playing_.id) continue;
        if (p.state == "seek" && !pc_direct_) {
            // A transcode can't seek: start a new one at the wanted time.
            report("/Sessions/Playing/Stopped", position_ticks_, false);
            start_playback(static_cast<long long>(std::max(0.0, p.pos) * kTicksPerSecond));
            return;
        }
        if (p.state != "seek") {
            position_ticks_ = static_cast<long long>(std::max(0.0, p.pos) * kTicksPerSecond);
            paused_ = p.paused;
        }
        if (p.state == "handoff") {
            // The PC gave the stream to a player that reports nothing back. It is
            // still playing: leave the Jellyfin session and its transcode alone
            // (a Stopped report would end the stream it is reading) and go back
            // to the list. Only the relay URL was handed over, so a PC key is unused.
            pc_playing_ = false;
            revoke_pc_key();
            status_ = "Playing in " + (pc_link::host().empty() ? std::string("the PC") : pc_link::host()) + "'s own player";
            view_ = return_view_;
            touch();
            return;
        }
        if (p.state == "stopped" || p.state == "ended" || p.state == "error") {
            if (p.state == "error") status_ = "The PC couldn't play it: " + p.error;
            finish_pc_playback();
            return;
        }
        touch();
    }
    if (pc_playing_ && !pc_link::connected()) finish_pc_playback();  // the PC went away
}

// Deletes a per-play key on the server, trying a few times: a key that outlives
// its play is an administrator login nobody is watching.
void JellyfinChip::revoke_key(const std::string& key) {
    if (key.empty()) return;
    results_.run([url = base_ + "/Auth/Keys/" + platform::net::url_encode(key), auth = auth_header()]() {
        for (int attempt = 0; attempt < 3; ++attempt) {
            const auto r = platform::net::http_request("DELETE", url, "", {auth}, kTimeoutMs);
            if (r.ok && (r.status / 100 == 2 || r.status == 404)) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(500 * (attempt + 1)));
        }
        return Result{Result::What::Position, {}, {}, "", -1, 0};  // ignored
    });
}

void JellyfinChip::revoke_pc_key() {
    revoke_key(pc_key_);
    pc_key_.clear();
}

void JellyfinChip::finish_pc_playback() {
    pc_playing_ = false;
    revoke_pc_key();
    report("/Sessions/Playing/Stopped", position_ticks_, false);
    playing_.position_ticks = position_ticks_;
    for (auto& e : episodes_) {
        if (e.id == playing_.id) e.position_ticks = position_ticks_;
    }
    for (auto& e : library_) {
        if (e.id == playing_.id) e.position_ticks = position_ticks_;
    }
    view_ = return_view_;
    touch();
}

} // namespace viewmodel::chips
