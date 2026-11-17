/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "model/pet_config.h"
#include "platform/jack_link.h"
#include "viewmodel/chip_module.h"
#include "viewmodel/chips/bridge_client.h"

#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace viewmodel::chips {

// JACK IN (chip 002), mockups/comms.html screen 6 and pet.html screens 5-6.
// Slotting the chip switches the USB-C port to device mode ([jackin] gadget_up:
// a read-only drive with the Jack In app plus a USB network link). The PC app
// dials the PET and sends a read-only scan; Alice reads it and may suggest one
// PowerShell fix, which [↵] sends to the PC. Everything that can change the PC
// is decided on the PC: Full Access, admin (UAC) and Share PC Internet are
// clicked there. The PET can only ask ([F] [A] [N]), take access away again,
// or [X] JACK OUT, which closes the PC app. Protocol: alicepet/jackin/PROTOCOL.md.
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
    bool usb_ok() const { return usb_ok_; }
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
    int relay_active() const { return relay_.active(); }

    // One control line from the PC (public for tests and snapshots).
    void on_line(const std::string& line);
    // Alice's answer to the latest ask (an older one still in flight is then ignored).
    void on_alice_reply(const BridgeReply& reply);
    // Lines sent to the PC, kept for tests while `capture_sent` is on.
    bool capture_sent{false};
    std::vector<std::string> sent;

    // "CPU 78% · RAM 91% · C: 8% free"
    static std::string scan_summary(const Scan& scan);
    static std::string alice_prompt(const std::string& host, const std::string& os, const Scan& scan);
    // Splits Alice's answer into her words and a "CMD:" PowerShell line (if any).
    static void split_reply(const std::string& reply, std::string& words, std::string& command);

    static constexpr size_t kMaxLog = 200;

private:
    void send(const std::string& json);
    void note(const std::string& text, LogLine::Kind kind = LogLine::Kind::Note);
    void ask(const char* what);
    void drop(const char* what);
    void run_suggestion();
    void ask_alice();
    void jack_out(const std::string& why);
    void reset_session();
    void set_sharing(bool on);
    void try_listen();

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
    bool usb_busy_{false};
    std::string usb_status_;
    struct GadgetResult {
        bool up{false};
        int code{0};
        std::string text;
    };
    Mailbox<GadgetResult> gadget_;

    bool alice_waiting_{false};
    bool alice_asked_{false};
    std::string alice_text_;
    std::string suggestion_;
    size_t alice_generation_{0};  // a reply for an older ask is dropped
    Mailbox<std::pair<size_t, BridgeReply>> alice_;

    // net_open requests from relay threads, sent on the UI thread.
    struct Opens {
        std::mutex mutex;
        std::vector<std::string> lines;
    };
    std::shared_ptr<Opens> opens_;
};

} // namespace viewmodel::chips
