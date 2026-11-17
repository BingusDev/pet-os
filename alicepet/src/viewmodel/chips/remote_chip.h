/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "model/pet_config.h"
#include "viewmodel/chip_module.h"
#include "viewmodel/chips/ir_codec.h"

#include <array>
#include <string>
#include <vector>

namespace viewmodel::chips {

// REMOTE (chip 003), mockups/tools.html screens 1-4:
//   Devices -> remote pad (send) / learn mode (record each button from the real
//   remote) / macros (several devices and waits on one key).
// IR goes through LIRC's `ir-ctl` (v4l-utils): [remote] recv_device to record,
// send_device to transmit. Learned remotes are text files in [remote] dir
// (<name>.remote); macros are <name>.macro files there:
//   name=Tournament mode
//   step=Projector|POWER
//   step=wait|20
//   step=Projector|HDMI2
class RemoteChip : public ChipModule {
public:
    enum class View { Devices, Name, Pad, Learn, Macros, MacroRun };
    enum class LearnState { Listening, Captured, Failed };

    // The pad, in mockup order: 4 + 5 + 6 buttons.
    static constexpr size_t kButtonCount = 15;
    static constexpr std::array<const char*, kButtonCount> kButtons = {
        "POWER", "INPUT", "HDMI1", "HDMI2", "VOL-", "VOL+", "MUTE", "CH-", "CH+",
        "UP", "LEFT", "OK", "RIGHT", "DOWN", "BACK"};
    static constexpr std::array<size_t, 3> kRowSizes = {4, 5, 6};

    struct Device {
        std::string name;
        std::string file;  // full path of its .remote file
        std::array<ir::Raw, kButtonCount> codes{};
        size_t learned() const;
    };
    struct MacroStep {
        std::string device;  // "wait" for a pause
        std::string button;
        int wait_s{0};
    };
    struct Macro {
        std::string name;
        std::vector<MacroStep> steps;
    };

    explicit RemoteChip(model::PetConfig config);

    const char* id() const override { return "remote"; }
    void enter() override;
    void leave() override;
    bool handle_key(Key key) override;
    void handle_char(const std::string& utf8) override;
    void tick(uint32_t elapsed_ms) override;
    bool keep_awake() const override { return view_ == View::Learn || view_ == View::MacroRun; }
    bool wants_text() const override { return true; }  // L/M/S/T hotkeys and the name field

    View view() const { return view_; }
    const std::vector<Device>& devices() const { return devices_; }
    size_t selected() const { return selected_; }  // devices_.size() = "Learn new device"
    const Device* device() const { return device_ < devices_.size() ? &devices_[device_] : nullptr; }
    size_t button() const { return button_; }
    const std::string& name_input() const { return name_input_; }
    const std::string& status() const { return status_; }
    bool status_error() const { return status_error_; }
    bool sending() const { return sending_; }

    LearnState learn_state() const { return learn_state_; }
    const std::string& learn_detail() const { return learn_detail_; }
    uint32_t learn_ms() const { return learn_ms_; }

    const std::vector<Macro>& macros() const { return macros_; }
    size_t macro() const { return macro_; }
    size_t macro_step() const { return step_; }
    int macro_wait_left_s() const { return static_cast<int>((wait_left_ms_ + 999) / 1000); }
    bool macro_done() const { return macro_done_; }
    const std::string& dir() const { return dir_; }

    // A capture or send finished (called from tick(); public for snapshots and tests).
    struct IrResult {
        enum class Kind { Capture, Send } kind{Kind::Send};
        bool ok{false};
        std::string error;
        ir::Raw raw;
        size_t generation{0};
    };
    void on_ir_result(const IrResult& result);
    static std::string slug(const std::string& name);

private:
    void load();
    bool save(const Device& device);
    void open_pad(size_t index);
    void start_learn(size_t index);
    void listen();
    void launch_capture();
    void next_button();
    void finish_learn();
    void send(const ir::Raw& raw, const std::string& what);
    void start_macro(size_t index);
    void run_step();
    void move_pad(Key key);

    model::PetConfig config_;
    std::string dir_;
    View view_{View::Devices};
    std::vector<Device> devices_;
    std::vector<Macro> macros_;
    size_t selected_{0};
    size_t device_{0};
    size_t button_{0};
    std::string name_input_;
    std::string status_;
    bool status_error_{false};
    bool sending_{false};
    bool loaded_{false};

    LearnState learn_state_{LearnState::Listening};
    std::string learn_detail_;
    uint32_t learn_ms_{0};
    bool capturing_{false};
    size_t generation_{0};

    size_t macro_{0};
    size_t step_{0};
    uint32_t wait_left_ms_{0};
    bool macro_done_{false};

    Mailbox<IrResult> ir_;
};

} // namespace viewmodel::chips
