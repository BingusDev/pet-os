/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "model/pet_config.h"

#include <chrono>
#include "platform/jack_link.h"
#include "platform/media_relay.h"
#include "viewmodel/chip_module.h"
#include "viewmodel/chips/bridge_client.h"

#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace viewmodel::chips {

// The live Jack In link as other chips see it (Jellyfin and Retro play on the PC
// while a PC is linked). All calls are for the UI thread; with no Jack In chip
// or no PC everything is a no-op and connected() is false.
namespace pc_link {
bool connected();
const std::string& host();
void send(const std::string& json);
// Puts an upstream file (a Jellyfin stream) behind the PET's media relay on the
// link; returns the full http://10.55.0.1:<port>/m/<token>/<name> URL, or "".
std::string relay_file(const std::string& upstream_url, const std::vector<std::string>& headers,
                       const std::string& name);
// Puts a whole site (RomM) behind the relay; returns the URL that logs the PC's
// browser into the relay and lands on `path`, or "".
std::string relay_site(const std::string& origin, const std::vector<std::string>& headers, const std::string& path);
// Playback reports from the PC ({"t":"progress"}), oldest first.
struct Progress {
    std::string item;
    double pos{0};
    bool paused{false};
    // "playing", "stopped", "ended", "error", or "handoff": the PC gave the stream
    // to a player that reports nothing back (playback goes on, unwatched).
    std::string state;
    std::string error;
};
bool take_progress(Progress& progress);
}  // namespace pc_link

// JACK IN (chip 002), mockups/comms.html screen 6 and pet.html screens 5-6.
// Slotting the chip switches the USB-C port to device mode ([jackin] gadget_up:
// a read-only drive with the Jack In app plus a USB network link). The PC app
// dials the PET and sends a read-only scan; Alice reads it and may suggest one
// PowerShell fix, which [↵] sends to the PC. Everything that can change the PC
// is decided on the PC: Full Access, admin (UAC) and Share PC Internet are
// clicked there. The PET can only ask ([F] [A] [N]), take access away again,
// or [X] JACK OUT, which closes the PC app. Everything that lets the PC use the
// PET is decided on the PET: [K] lets the PC open and drive chips, and Terminal
// (the PET's SSH keys) needs its own [K] in every session.
// Protocol: alicepet/jackin/PROTOCOL.md.
class JackInChip : public ChipModule {
public:
    enum class View { Waiting, Session, Log };

    struct Scan {
        bool have{false};
        int cpu{-1};          // percent
        int ram{-1};          // percent used
        std::string disk;     // "C:"
        int disk_free{-1};    // percent free
        int startup{-1};      // startup items
        std::string defender; // "on" / "off" / "?"
        std::string defender_since;
        struct Hog {
            std::string name;
            int mb{0};
        };
        std::vector<Hog> top;
    };

    struct LogLine {
        enum class Kind { Pending, Ok, Fail, Note } kind{Kind::Note};
        std::string text;
        int id{0};  // PET-sent command id
    };

    explicit JackInChip(model::PetConfig config);
    ~JackInChip() override;

    const char* id() const override { return "jack-in"; }
    void enter() override;
    void leave() override;
    bool handle_key(Key key) override;
    void handle_char(const std::string& utf8) override;
    void tick(uint32_t elapsed_ms) override;
    bool keep_awake() const override { return connected_ || alice_waiting_ || sharing_; }
    bool wants_text() const override { return true; }  // F/A/N/L/X/R hotkeys

    View view() const { return view_; }
    bool connected() const { return connected_; }
    const std::string& usb_status() const { return usb_status_; }
    bool usb_ok() const { return usb_ok_ && !usb_no_pc_; }
    const std::string& listen_address() const { return config_.jack_listen; }
    const std::string& host() const { return host_; }
    const std::string& os() const { return os_; }
    const std::string& user() const { return user_; }
    bool full() const { return full_; }
    bool admin() const { return admin_; }
    bool sharing() const { return sharing_; }
    bool asked_full() const { return asked_full_; }
    bool asked_admin() const { return asked_admin_; }
    bool asked_net() const { return asked_net_; }
    const Scan& scan() const { return scan_; }
    const std::deque<LogLine>& log() const { return log_; }
    size_t log_scroll() const { return log_scroll_; }
    const std::string& status() const { return status_; }
    bool alice_waiting() const { return alice_waiting_; }
    const std::string& alice_text() const { return alice_text_; }
    const std::string& suggestion() const { return suggestion_; }
    bool chat_waiting() const { return chat_waiting_; }
    // PC access mode from hello/state: "read", "ask" or "full".
    const std::string& mode() const { return mode_; }
    // Alice running PowerShell on the PC for one chat message: her commands go
    // to the PC (which decides per mode), their output comes back to her.
    bool agent_running() const { return agent_running_; }
    int agent_steps() const { return agent_steps_; }
    // Commands per chat message before the PC asks Holden "Keep going?" (Yes: as many again).
    static constexpr int kAgentMaxSteps = 8;
    static constexpr int kAgentMaxStepsFull = 25;
    static int agent_cap(const std::string& mode) { return mode == "full" ? kAgentMaxStepsFull : kAgentMaxSteps; }
    bool agent_holding() const { return !agent_held_.empty(); }
    int relay_active() const { return relay_.active(); }
    // PET-side grants (never taken from the PC's word): the PC may open and
    // drive chips / may open Terminal. Both end with the session.
    bool pc_control() const { return pc_control_; }
    bool pc_terminal() const { return pc_terminal_; }
    // What the PC is waiting for Holden to allow with [K]: "", "control" or "terminal".
    const std::string& pc_wants() const { return pc_wants_; }
    // True once after something on the PET screen needs Holden (the app lights the panel).
    bool take_attention() {
        const bool a = attention_;
        attention_ = false;
        return a;
    }

    // One control line from the PC (public for tests and snapshots).
    void on_line(const std::string& line);
    // Alice's answer to the latest ask (an older one still in flight is then ignored).
    void on_alice_reply(const BridgeReply& reply);
    // Alice's answer to a chat typed or spoken in the PC app (sent back to the PC too).
    void on_chat_reply(const BridgeReply& reply);
    // Lines sent to the PC, kept for tests while `capture_sent` is on.
    bool capture_sent{false};
    std::vector<std::string> sent;

    // "CPU 78% · RAM 91% · C: 8% free"
    static std::string scan_summary(const Scan& scan);
    static std::string alice_prompt(const std::string& host, const std::string& os, const Scan& scan);
    // Splits Alice's answer into her words and a "CMD:" PowerShell line (if any).
    static void split_reply(const std::string& reply, std::string& words, std::string& command);
    // The bridge prompt for a chat message typed on the PC, telling Alice the mode and the CMD: rule.
    static std::string chat_prompt(const std::string& host, const std::string& mode, const std::string& text);
    // The next agent turn: a command's output (or why it didn't run) back to Alice.
    static std::string output_prompt(const std::string& mode, const std::string& cmd, bool ok,
                                     const std::string& reason, const std::string& out);

    static constexpr size_t kMaxLog = 200;

    // --- PC mirror (PROTOCOL.md: chips, open, frame, input) ---
    // Chips the PC can open (Voice, Remote and Jack In stay on the PET).
    static bool pc_capable(const std::string& chip_id);
    // Where chip art lives (<id>-small.png, <id>-big.png).
    void set_art_dir(std::filesystem::path dir) { art_dir_ = std::move(dir); }
    // The app sets this around AliceViewModel::open_chip for a PC request, so
    // leaving Jack In for the requested chip keeps the PC link and USB up.
    void set_switching(bool on) { switching_ = on; }
    // A chip the PC asked to open; the app opens it on the UI thread.
    bool take_open(std::string& chip_id);
    // A key or character from the PC; the app forwards it to the view model.
    struct Input {
        bool text{false};   // false: key, true: utf8
        Key key{Key::Enter};
        std::string utf8;
        bool pointer{false};  // a mouse button event at (x, y) in PET pixels
        bool down{false};
        int x{0};
        int y{0};
    };
    bool take_input(Input& input);
    // True when a frame should be captured now (about 12 fps while the PC is mirroring).
    bool wants_frame(uint32_t now_ms);
    // A PC key, character or pointer changed the chip: the next wants_frame() is
    // true once, skipping the rate limit, so the PC sees the result at once.
    void request_frame() { frame_asap_ = true; }
    // Diagnostics (logged only while linked, rate-limited): the time from a PC input
    // being applied to the next frame offered, and the timing check shared by callers.
    void mark_input_applied();
    static bool timing_log_due(std::chrono::steady_clock::time_point& next, double ms);
    // One LVGL frame (ARGB8888, `stride` bytes per row). `screen` names the PET screen.
    void offer_frame(const uint8_t* argb, uint32_t stride, int w, int h, const std::string& screen);
    bool mirroring() const { return mirror_on_ && connected_; }
    // The chip the PC is operating (it runs offscreen; the PET panel keeps the
    // Jack In view): its id, or "" when the PC is HOME or not linked.
    const std::string& pc_chip() const { return pc_chip_; }
    void set_pc_chip(const std::string& chip_id) { pc_chip_ = chip_id; touch(); }

    // pc_link backing (see above).
    std::string relay_file(const std::string& upstream_url, const std::vector<std::string>& headers,
                           const std::string& name);
    std::string relay_site(const std::string& origin, const std::vector<std::string>& headers, const std::string& path);
    bool take_progress(pc_link::Progress& progress);
    void send_line(const std::string& json) { send(json); }

private:
    bool send(const std::string& json, bool droppable = false);
    void send_pet();
    void grant_key();
    void deny_open(const std::string& chip, const char* what);
    void submit_gadget(bool up);
    void note(const std::string& text, LogLine::Kind kind = LogLine::Kind::Note);
    void ask(const char* what);
    void drop(const char* what);
    void run_suggestion();
    void ask_alice();
    void ask_chat(const std::string& prompt);
    void agent_stop(const std::string& why);
    void agent_run(const std::string& command);
    void jack_out(const std::string& why);
    void reset_session();
    void set_sharing(bool on);
    void try_listen();
    void send_chips();
    bool send_frame_band(const std::string& screen, int w, int h, const std::vector<uint16_t>& px, int y0, int y1);

    model::PetConfig config_;
    View view_{View::Waiting};
    platform::jack::LineServer server_;
    platform::jack::Relay relay_;
    uint32_t listen_retry_ms_{0};
    std::string listen_error_;

    bool connected_{false};
    std::string host_;
    std::string os_;
    std::string user_;
    bool full_{false};
    bool admin_{false};
    bool sharing_{false};
    bool asked_full_{false};
    bool asked_admin_{false};
    bool asked_net_{false};
    Scan scan_;
    std::deque<LogLine> log_;
    size_t log_scroll_{0};
    int next_id_{1};
    std::string status_;

    bool usb_ok_{false};
    bool usb_want_up_{false};      // the last gadget job asked for: up (the chip is slotted) or down
    size_t usb_up_seq_{0};         // the newest "up" job; older results are ignored
    bool usb_no_pc_{false};        // gadget up but no PC on the cable for a while
    uint32_t udc_check_ms_{0};
    uint32_t unattached_ms_{0};
    void check_cable(uint32_t elapsed_ms);
    std::string usb_status_;
    struct GadgetResult {
        bool up{false};
        size_t seq{0};
        int code{0};
        std::string text;
    };
    // usb-gadget.sh up/down run one at a time, in the order asked, on one worker.
    // (One detached thread per job let a "down" overtake the "up" before it.)
    struct GadgetQueue {
        struct Job {
            bool up{false};
            size_t seq{0};
            std::string cmd;
        };
        std::mutex mutex;
        std::deque<Job> jobs;
        std::deque<GadgetResult> done;
        bool running{false};
    };
    std::shared_ptr<GadgetQueue> gadget_;

    bool alice_waiting_{false};
    bool alice_asked_{false};
    std::string alice_text_;
    std::string suggestion_;
    size_t alice_generation_{0};  // a reply for an older ask is dropped
    Mailbox<std::pair<size_t, BridgeReply>> alice_;

    // Chat from the PC app ({"t":"chat"} / {"t":"voice"}), relayed to Alice by the PET.
    bool chat_waiting_{false};
    size_t chat_generation_{0};  // a reply asked in an earlier session is dropped
    Mailbox<std::pair<size_t, BridgeReply>> chat_;
    std::string mode_{"read"};
    bool agent_running_{false};   // a chat message's command loop is going
    int agent_steps_{0};          // commands sent for the current chat message
    int agent_id_{0};             // run id the loop waits on (0 = none)
    std::string agent_cmd_;
    int agent_limit_{kAgentMaxSteps};  // steps allowed before asking to keep going
    std::string agent_held_;           // her next command, waiting for "Keep going?"

    // net_open requests from relay threads, sent on the UI thread.
    struct Opens {
        std::mutex mutex;
        std::vector<std::string> lines;
    };
    std::shared_ptr<Opens> opens_;

    std::filesystem::path art_dir_;
    bool switching_{false};
    bool input_ok_{false};          // PC mode is Ask or Full (never Read); the PC's own word
    bool pc_control_{false};        // Holden allowed this PC to open and drive chips ([K])
    bool pc_terminal_{false};       // ... and to open Terminal ([K] again, every session)
    std::string pc_wants_;          // "control" / "terminal": the PC asked, [K] decides
    std::string pc_wants_chip_;     // the chip that request was for
    bool attention_{false};
    bool mirror_on_{false};         // the PC window is visible and wants frames
    std::deque<std::string> opening_;
    std::deque<Input> inputs_;
    uint32_t last_frame_ms_{0};
    bool frame_asap_{false};
    // Set by mark_input_applied(); offer_frame() logs the wait for the next frame.
    bool input_pending_{false};
    std::chrono::steady_clock::time_point input_at_{};
    std::chrono::steady_clock::time_point input_log_next_{};
    std::vector<uint16_t> last_px_;  // RGB565 of the last frame sent
    std::string last_screen_;
    std::string pc_chip_;

    // Media relay for PC playback (files: jack_media_port, site: jack_media_port + 1).
    platform::jack::MediaRelay media_;
    platform::jack::MediaRelay site_;
    std::deque<pc_link::Progress> progress_;
};

} // namespace viewmodel::chips
