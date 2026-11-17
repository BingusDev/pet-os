/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "model/pet_config.h"
#include "viewmodel/chip_module.h"

#include <sys/types.h>

#include <string>
#include <vector>

namespace viewmodel::chips {

// JELLYFIN (chip 001), mockups/media.html screens 1-4:
//   Library -> (series) seasons + episodes -> resume prompt -> playing.
// Browsing uses the Jellyfin REST API ([jellyfin] url + api_key from
// /etc/alicepet.conf). Playback asks the server for an H.264 HLS transcode
// that fits the 320x170 panel and hands the URL to [jellyfin] player (mpv by
// default; controlled over its IPC socket). Progress is reported back through
// /Sessions/Playing so resume points sync with every Jellyfin app.
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
    bool keep_awake() const override { return view_ == View::Playing || view_ == View::Loading; }
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

private:
    struct Result {
        enum class What { User, Library, Seasons, Episodes, Stream, Position, Error } what;
        std::vector<Item> items;
        std::vector<Season> seasons;
        std::string text;
        double seconds{0};
        size_t generation{0};
    };

    void load_library();
    void open_series();
    void load_episodes();
    void choose(const Item& item);
    void start_playback(long long start_ticks);
    void stop_playback();
    void report(const char* endpoint, long long ticks, bool paused);
    void mpv(const std::string& json);
    std::string auth_header() const;
    std::string stream_url(const Item& item, long long start_ticks) const;

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
    bool ipc_{false};
    uint32_t play_ms_{0};
    uint32_t poll_ms_{0};
    uint32_t report_ms_{0};
    std::string stream_info_;

    Mailbox<Result> results_;
};

} // namespace viewmodel::chips
