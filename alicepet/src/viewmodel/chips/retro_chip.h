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

// RETRO (chip 006), mockups/media.html RETRO screens 1-4:
//   Library (system tabs + game carousel) -> game details -> playing.
// Games come from a RomM server ([romm] url + token, REST API). A game is
// downloaded to [retro] dir once ("Keep offline", or on first Play) and run
// with [retro] player (RetroArch by default) using the libretro core set for
// its platform. While the emulator runs it owns the panel; quitting it brings
// the PET back.
// Save sync ([retro] sync_saves): before a game starts, a newer save on RomM
// (GET /api/saves?rom_id=) is pulled into [retro] dir/<platform>/saves/; after
// the emulator quits, a save it changed is uploaded (POST /api/saves, field
// saveFile, overwrite=true), so it carries on from any RomM-synced device.
class RetroChip : public ChipModule {
public:
    enum class View { Loading, Library, Details, Downloading, Playing, Error };

    struct Platform {
        int id{0};  // 0 = ALL
        std::string slug;
        std::string label;  // "GBA"
        int rom_count{0};
    };
    struct Save {
        int id{0};
        std::string file_name;
        std::string updated_at;  // ISO 8601 from RomM
        long long updated{0};    // unix seconds
    };
    struct Game {
        int id{0};
        std::string name;
        std::string file;  // fs_name on the server
        std::string platform;  // slug
        long long size{0};
        int year{0};
    };

    explicit RetroChip(model::PetConfig config);
    ~RetroChip() override;

    const char* id() const override { return "retro"; }
    void enter() override;
    void leave() override { stop_game(); }
    bool handle_key(Key key) override;
    void handle_char(const std::string& utf8) override;
    void tick(uint32_t elapsed_ms) override;
    bool keep_awake() const override {
        return view_ == View::Loading || view_ == View::Downloading || view_ == View::Playing;
    }
    bool wants_text() const override { return true; }  // O / search letters
    bool owns_display() const override { return view_ == View::Playing && player_ > 0; }

    View view() const { return view_; }
    const std::string& status() const { return status_; }
    const std::vector<Platform>& platforms() const { return platforms_; }
    size_t platform() const { return platform_; }
    // Games of the current tab, filtered by the search text.
    std::vector<const Game*> games() const;
    size_t selected() const { return selected_; }
    const Game* game() const;
    bool searching() const { return searching_; }
    const std::string& search() const { return search_; }
    bool details_play() const { return details_play_; }
    bool downloaded(const Game& game) const;
    // "1:1 pixel-perfect" / "scaled 76%" for a platform on the 320x170 panel.
    static std::string fit_text(const std::string& platform_slug);
    static std::string short_label(const std::string& platform_slug, const std::string& fallback);
    static std::string size_text(long long bytes);
    // "synced 2 days ago" / "none yet" / ...; `good` = shown green.
    std::string save_text(bool& good) const;
    // ISO 8601 ("2026-11-15T10:20:30.123+00:00" / "...Z") -> unix seconds, 0 if unparsable.
    static long long parse_iso_time(const std::string& text);
    static std::string ago_text(long long then, long long now);
    // The save RetroArch writes for `game` ([retro] dir/<platform>/saves/<rom name>.srm).
    std::string local_save_path(const Game& game) const;
    static std::vector<Save> parse_saves(const std::string& json);
    // Picks the save to sync: same file name as ours, else the newest.
    static const Save* pick_save(const std::vector<Save>& saves, const std::string& file_name);
    static std::string multipart_body(const std::string& boundary, const std::string& field,
                                      const std::string& file_name, const std::string& bytes);

    // Results from the RomM API (called from tick(); public for snapshots and tests).
    struct Result {
        enum class What { Platforms, Games, Download, Saves, SaveDown, SaveUp, Error } what{What::Error};
        std::vector<Platform> platforms;
        std::vector<Game> games;
        std::vector<Save> saves;
        int rom_id{0};
        std::string text;
        size_t generation{0};
    };
    void on_result(Result result);
    static std::vector<Platform> parse_platforms(const std::string& json);
    static std::vector<Game> parse_games(const std::string& json);

private:
    void load_platforms();
    void load_games();
    void download(const Game& game, bool then_play);
    void play(const Game& game);
    void stop_game();
    void start(const Game& game);  // pull a newer save if any, then play
    void fetch_saves(const Game& game);
    void pull_save(const Game& game, const Save& save);
    void push_save(const Game& game);
    std::string local_path(const Game& game) const;
    std::vector<std::string> headers() const;

    model::PetConfig config_;
    std::string base_;
    std::string dir_;
    View view_{View::Loading};
    std::string status_;
    std::vector<Platform> platforms_;
    size_t platform_{0};
    std::vector<Game> games_;
    size_t selected_{0};
    bool searching_{false};
    std::string search_;
    bool details_play_{true};
    bool loaded_{false};
    bool play_after_download_{false};
    int downloading_id_{0};
    size_t generation_{0};

    pid_t player_{-1};
    uint32_t play_ms_{0};
    int playing_id_{0};

    // Save sync for the game on the details screen.
    int saves_rom_{0};
    bool saves_known_{false};
    bool saves_checking_{false};
    bool saves_failed_{false};
    std::vector<Save> saves_;
    long long launch_save_mtime_{0};
    std::string save_status_;

    Mailbox<Result> results_;
};

} // namespace viewmodel::chips
