/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "model/pet_config.h"
#include "viewmodel/chip_module.h"
#include "viewmodel/chips/list_nav.h"

#include <sys/types.h>

#include <string>
#include <vector>

namespace viewmodel::chips {

// Whether the PC (Jack In) can play this file as it is: H.264 video in an MP4/M4V/MOV
// container with AAC/MP3 or no audio. Anything else (MKV included) is transcoded.
bool pc_direct_play(const std::string& container, const std::string& video, const std::string& audio);

// JELLYFIN (chip 001), mockups/media.html screens 1-4:
//   Library -> (series) seasons + episodes -> resume prompt -> playing.
// Browsing uses the Jellyfin REST API ([jellyfin] url + api_key from
// /etc/pet-os.conf). Playback asks the server for an H.264 HLS transcode
// that fits the 320x170 panel and hands the URL to [jellyfin] player (mpv by
// default; controlled over its IPC socket). Progress is reported back through
// /Sessions/Playing so resume points sync with every Jellyfin app.
// Jacked in (a PC app linked through JACK IN): playback happens in the PC app
// instead. The PET asks Jellyfin for a stream Windows can play (the file itself
// for H.264 MP4/MKV, else an H.264/AAC MP4 transcode), puts it behind the PET's
// media relay on the USB link (the API key stays on the PET) and sends the PC
// {"t":"play"}; the PC reports {"t":"progress"} and the PET reports it on.
// Desktop: browsing is real; playback is a STUB that only fetches the HLS
// playlist to prove the transcode works (no player in the simulator).
class JellyfinChip : public ChipModule {
public:
    enum class View { Loading, Library, Episodes, Resume, Playing, Error };
    enum class Kind { Shows, Movies };

    struct Item {
        std::string id;
        std::string name;
        std::string type;  // "Series", "Movie", "Episode"
        int year{0};
        int child_count{0};  // seasons for a series
        int index{0};        // episode number
        int season{0};
        bool played{false};
        long long position_ticks{0};
        long long runtime_ticks{0};
    };

    struct Season {
        std::string id;
        std::string name;
        int index{0};
    };

    explicit JellyfinChip(model::PetConfig config);
    ~JellyfinChip() override;

    const char* id() const override { return "jellyfin"; }
    void enter() override;
    bool handle_key(Key key) override;
    void handle_char(const std::string& utf8) override;
    void tick(uint32_t elapsed_ms) override;
    // A paused video doesn't hold the PET awake: after the idle timeout it naps,
    // and waking stops the player (leave()) with the resume point saved.
    bool keep_awake() const override { return (view_ == View::Playing && !paused_) || view_ == View::Loading; }
    // Playing in the PC app (jacked in), not on the PET.
    bool on_pc() const { return pc_playing_; }
    // Leaving the chip (including a wake from nap) never leaves mpv running behind the UI.
    void leave() override { stop_playback(); }
    bool wants_text() const override { return searching_; }
    bool owns_display() const override { return view_ == View::Playing && player_ > 0; }

    View view() const { return view_; }
    Kind kind() const { return kind_; }
    const std::string& status() const { return status_; }
    // Library (already filtered by the search text).
    std::vector<const Item*> library() const;
    size_t library_total() const { return library_.size(); }
    size_t selected() const { return selected_; }
    bool searching() const { return searching_; }
    const std::string& search() const { return search_; }

    const Item* series() const { return series_index_ < library_.size() ? &library_[series_index_] : nullptr; }
    const std::vector<Season>& seasons() const { return seasons_; }
    size_t season() const { return season_; }
    const std::vector<Item>& episodes() const { return episodes_; }
    size_t episode() const { return episode_; }

    const Item& playing() const { return playing_; }
    bool resume_choice() const { return resume_choice_; }
    long long position_ticks() const { return position_ticks_; }
    bool paused() const { return paused_; }
    bool has_player() const { return player_ > 0; }
    // Desktop stub / diagnostics: what the server offered for the transcode.
    const std::string& stream_info() const { return stream_info_; }

    static std::string clock_text(long long ticks);  // "8:41" / "1:02:03"

public:
    // Worker results; on_result() applies one (tests feed them directly).
    struct Result {
        enum class What { User, Library, Seasons, Episodes, Stream, Position, PcStream, Error } what;
        std::vector<Item> items;
        std::vector<Season> seasons;
        std::string text;
        double seconds{0};
        size_t generation{0};
        bool direct{false};  // PcStream: the file itself (seekable) rather than a transcode
        std::string name{};  // PcStream: file name for the relay URL
        std::string key{};   // PcStream: short-lived Jellyfin key for the PC's direct URL ("" = relay only)
        bool key_login{false};  // ... it is the [jellyfin] pc_user login's token, not an API key
        std::string note{};     // PcStream: why the PC got no direct URL (shown in the status line)
    };

    // Applies a worker result; results from before the latest request are dropped.
    void on_result(Result result);

private:
    void load_library();
    void open_series();
    void load_episodes();
    void choose(const Item& item);
    void start_playback(long long start_ticks);
    void stop_playback();
    void start_pc_playback(long long start_ticks);
    void finish_pc_playback();
    void revoke_pc_key();
    void revoke_key(const std::string& key, bool login);
    // The Authorization header of the PC-playback login ([jellyfin] pc_user); `token` "" before it logs in.
    static std::string pc_auth_header(const std::string& device_id, const std::string& token);
    void pc_tick();
    std::string pc_title() const;
    void report(const char* endpoint, long long ticks, bool paused);
    void mpv(const std::string& json);
    std::string auth_header() const;
    // `with_key`: put the API key in the query string (only for players we can't
    // hand a header to; it is then visible in `ps`).
    std::string stream_url(const Item& item, long long start_ticks, bool with_key) const;
    // Writes a 0600 mpv config carrying the API key as an HTTP header; "" on failure.
    std::string write_mpv_auth_config() const;

    model::PetConfig config_;
    std::string base_;
    std::string user_id_;
    std::string device_id_;
    View view_{View::Loading};
    View return_view_{View::Library};
    Kind kind_{Kind::Shows};
    std::string status_;
    std::vector<Item> library_;
    size_t selected_{0};
    bool searching_{false};
    std::string search_;
    bool loaded_{false};

    size_t series_index_{0};
    std::vector<Season> seasons_;
    size_t season_{0};
    std::vector<Item> episodes_;
    size_t episode_{0};
    size_t generation_{0};

    Item playing_;
    bool resume_choice_{true};
    std::string play_session_;
    long long start_ticks_{0};
    long long position_ticks_{0};
    bool paused_{false};
    pid_t player_{-1};
    std::string ipc_path_;
    std::string mpv_conf_path_;
    bool ipc_{false};
    uint32_t play_ms_{0};
    uint32_t poll_ms_{0};
    uint32_t report_ms_{0};
    std::string stream_info_;
    std::string play_method_{"Transcode"};
    std::string pc_key_;  // Jellyfin key the PC streams with directly; revoked when the play ends
    bool pc_key_login_{false};
    bool pc_playing_{false};
    bool pc_direct_{false};

    Mailbox<Result> results_;
};

} // namespace viewmodel::chips
