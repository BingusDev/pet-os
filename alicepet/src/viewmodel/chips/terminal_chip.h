/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "model/pet_config.h"
#include "platform/process.h"
#include "viewmodel/chip_module.h"
#include "viewmodel/chips/bridge_client.h"
#include "viewmodel/chips/vt_screen.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace viewmodel::chips {

// TERMINAL (chip 007), mockups/comms.html screens 3-5.
//   Hosts: SSH bookmarks ([terminal] host= lines + Host entries from ssh_config),
//          each probed on its SSH port. ENTER connects, A asks Alice, D hangs up.
//   Shell: `ssh -tt <target>` on a pty, drawn by VtScreen (45x11). Every key goes
//          to the remote side; hold ESC returns to the host list (session stays up).
//   Ask:   a question goes to the bridge with the host as context; Alice's
//          suggested command is shown in full and only runs when the user presses
//          ENTER (E types it without running). A command too long to show whole
//          can only be typed (E), never run blind. Nothing is executed
//          automatically: into a fresh session the command is typed (without
//          Enter) only once a shell prompt is on screen.
class TerminalChip : public ChipModule {
public:
    static constexpr size_t kCols = 45;
    static constexpr size_t kRows = 11;
    // The suggestion box shows the command hard-wrapped to this many columns and lines.
    static constexpr size_t kCommandCols = 41;
    static constexpr size_t kCommandLines = 3;
    // A fresh session gives up on typing a suggestion after this long without a prompt.
    static constexpr uint32_t kPromptWaitMs = 20000;

    enum class Mode { Hosts, Shell, Ask };
    enum class AskState { Typing, Waiting, Suggestion, Error };
    enum class Reach { Unknown, Checking, Up, Down };

    struct Host {
        std::string label;
        std::string target;   // passed to ssh
        std::string note;
        std::string probe_host;
        int port{22};
        Reach reach{Reach::Unknown};
        int latency_ms{-1};
    };

    struct Session {
        platform::proc::PtySession pty;
        VtScreen screen{kCols, kRows};
        bool ended{false};
        std::string pending_input;  // typed (never run) once a shell prompt shows
        uint32_t pending_ms{0};
        uint32_t quiet_ms{0};       // time since the last output
        bool had_output{false};
    };

    explicit TerminalChip(model::PetConfig config);

    const char* id() const override { return "terminal"; }
    void enter() override;
    bool handle_key(Key key) override;
    void handle_char(const std::string& utf8) override;
    void handle_ctrl(char letter) override;
    void tick(uint32_t elapsed_ms) override;
    // Awake while Alice is answering or the open shell printed something in the
    // last kShellAwakeMs (a running build, a log tail). An idle shell may nap.
    bool keep_awake() const override;
    static constexpr uint32_t kShellAwakeMs = 30000;
    bool wants_text() const override { return mode_ != Mode::Hosts; }

    Mode mode() const { return mode_; }
    const std::vector<Host>& hosts() const { return hosts_; }
    size_t selected() const { return selected_; }
    bool connected(size_t host) const;
    // Shell of the selected host (nullptr if none).
    const Session* session() const;
    const Host* current_host() const { return selected_ < hosts_.size() ? &hosts_[selected_] : nullptr; }
    bool cursor_blink_on() const { return (blink_ms_ / 500) % 2 == 0; }

    AskState ask_state() const { return ask_state_; }
    const std::string& ask_input() const { return ask_input_; }
    const std::string& ask_question() const { return ask_question_; }
    const std::string& ask_why() const { return ask_why_; }
    const std::string& ask_command() const { return ask_command_; }
    // The command split into kCommandCols-wide lines, exactly as the view shows it.
    std::vector<std::string> ask_command_lines() const;
    // True when every character of the command fits in the suggestion box, so ENTER may run it.
    bool ask_command_fits() const { return ask_command_lines().size() <= kCommandLines; }
    const std::string& ask_error() const { return ask_error_; }
    int thinking_dots() const { return static_cast<int>((wait_ms_ / 400) % 4); }

    // Parses Alice's answer into a suggestion (public for snapshot tests).
    void on_ask_reply(const BridgeReply& reply);
    void connect(size_t host);

private:
    void probe_all();
    void send_to_shell(const std::string& bytes);
    void run_suggestion(bool execute);
    // True when the session's screen ends in what looks like an idle shell prompt.
    static bool at_shell_prompt(const Session& session);

    model::PetConfig config_;
    std::vector<Host> hosts_;
    size_t selected_{0};
    Mode mode_{Mode::Hosts};
    std::map<size_t, std::unique_ptr<Session>> sessions_;
    uint32_t blink_ms_{0};
    bool probed_{false};

    struct ProbeResult {
        size_t index;
        bool up;
        int latency_ms;
    };
    Mailbox<ProbeResult> probes_;

    AskState ask_state_{AskState::Typing};
    std::string ask_input_;
    std::string ask_question_;
    std::string ask_why_;
    std::string ask_command_;
    std::string ask_error_;
    uint32_t wait_ms_{0};
    Mailbox<BridgeReply> asks_;
};

// Host entries from an ssh_config file (aliases without wildcards), first alias as label.
struct SshConfigHost {
    std::string alias;
    std::string hostname;
    std::string user;
    int port{0};
};
std::vector<SshConfigHost> read_ssh_config(const std::string& path);

// Builds the Ask-Alice prompt (instructions first, so a length cap on the bridge can
// only shorten the question). Never longer than kBridgeMaxInput.
std::string build_ask_prompt(const std::string& host_desc, const std::string& question);
// Drops control characters (ESC, CR, ...) so a suggestion can't smuggle keystrokes.
std::string sanitize_command(const std::string& command);

} // namespace viewmodel::chips
