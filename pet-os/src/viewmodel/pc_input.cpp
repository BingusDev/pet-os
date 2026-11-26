/*
 * SPDX-License-Identifier: MIT
 */

#include "viewmodel/pc_input.h"

#include "model/chip_registry.h"
#include "logger/logger.h"

#include <chrono>

namespace viewmodel {

namespace {

// Runs a PC input handler and logs how long it took while the PC is linked.
template <typename Fn>
void timed_input(chips::JackInChip& jack, const char* what, std::chrono::steady_clock::time_point& next, Fn&& fn) {
    const auto start = std::chrono::steady_clock::now();
    fn();
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    if (jack.connected() && chips::JackInChip::timing_log_due(next, ms)) LOG_INFO("pc input: {} took {:.1f} ms", what, ms);
}

} // namespace

PcInputBatch drain_pc_requests(AliceViewModel& vm,
                               const std::function<void(const chips::JackInChip::Input&)>& pointer) {
    PcInputBatch batch;
    auto* jack = static_cast<chips::JackInChip*>(vm.chip_module(model::chip_index("jack-in")));
    if (!jack) return batch;
    // A linked PC is activity. If the PET napped before the PC linked (JackIn.cmd
    // can take minutes), wake it now without leaving the chip: otherwise the PC's
    // first key woke it the normal way, which closes the open chip, and every key
    // and click after that was dropped.
    if (jack->connected()) vm.hold_awake();
    std::string id;
    while (jack->take_open(id)) {
        const size_t index = model::chip_index(id.c_str());
        if (index >= model::kChipCount) continue;
        jack->set_switching(true);
        vm.open_chip(index);
        jack->set_switching(false);
        // HOME brings the PC back to the Jack In screen; any other chip runs for the PC.
        jack->set_pc_chip(id == jack->id() ? std::string() : id);
    }
    static std::chrono::steady_clock::time_point key_log_next{};
    static std::chrono::steady_clock::time_point char_log_next{};
    chips::JackInChip::Input input;
    while (jack->take_input(input)) {
        const auto* active = vm.active_chip();
        if (!active || !chips::JackInChip::pc_capable(active->id())) {
            ++batch.dropped;
            continue;
        }
        batch.chip = active->id();
        // The PC is waiting on this input: send the next mirror frame at once.
        jack->request_frame();
        jack->mark_input_applied();
        if (input.pointer) {
            ++batch.pointers;
            if (pointer) pointer(input);
        } else if (input.text) {
            ++batch.chars;
            timed_input(*jack, "handle_char", char_log_next, [&] { vm.handle_char(input.utf8); });
        } else {
            ++batch.keys;
            const size_t index = model::chip_index(active->id());
            timed_input(*jack, "handle_key", key_log_next, [&] { vm.handle_key(input.key); });
            // ESC (or right click) at the chip's top level leaves it for the PET's
            // folder, where the PC has nothing left to drive: keep the PC's chip up
            // instead (the PC app's HOME is how the PC leaves it).
            if (vm.active_chip() != vm.chip_module(index) && jack->connected() && !jack->pc_chip().empty()) {
                jack->set_switching(true);
                vm.open_chip(index);
                jack->set_switching(false);
            }
        }
    }
    return batch;
}

} // namespace viewmodel
