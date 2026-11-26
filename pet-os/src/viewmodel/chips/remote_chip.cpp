/*
 * SPDX-License-Identifier: MIT
 */

#include "viewmodel/chips/remote_chip.h"

#include "platform/process.h"

#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace viewmodel::chips {
namespace {

namespace fs = std::filesystem;

constexpr int kCaptureTimeoutMs = 8000;  // then listen again (ir-ctl can't be interrupted cleanly)
constexpr int kSendTimeoutMs = 5000;

std::string trim(const std::string& value) {
    const auto begin = value.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return "";
    const auto end = value.find_last_not_of(" \t\r\n");
    return value.substr(begin, end - begin + 1);
}

size_t button_index(const std::string& name) {
    for (size_t i = 0; i < RemoteChip::kButtonCount; ++i) {
        if (name == RemoteChip::kButtons[i]) return i;
    }
    return RemoteChip::kButtonCount;
}

// Runs ir-ctl with `args`; a missing binary is the usual desktop/first-boot problem.
RemoteChip::IrResult run_ir_ctl(std::vector<std::string> argv, RemoteChip::IrResult::Kind kind, int timeout_ms,
                                size_t generation) {
    RemoteChip::IrResult result;
    result.kind = kind;
    result.generation = generation;
    std::string out;
    const int rc = platform::proc::run_capture(argv, out, timeout_ms, true);
    if (rc == -1) {
        result.error = "ir-ctl not found (apt install v4l-utils)";
    } else if (rc == -2) {
        result.error = "timeout";
    } else if (kind == RemoteChip::IrResult::Kind::Capture) {
        result.raw = ir::parse_ir_ctl(out);
        result.ok = result.raw.size() >= 3;
        if (!result.ok) result.error = rc != 0 ? trim(out) : "nothing usable received";
    } else {
        result.ok = rc == 0;
        if (!result.ok) result.error = trim(out).empty() ? "ir-ctl failed (" + std::to_string(rc) + ")" : trim(out);
    }
    return result;
}

} // namespace

size_t RemoteChip::Device::learned() const {
    return static_cast<size_t>(std::count_if(codes.begin(), codes.end(), [](const ir::Raw& r) { return !r.empty(); }));
}

RemoteChip::RemoteChip(model::PetConfig config)
    : config_(std::move(config)), dir_(platform::proc::expand_home(config_.remote_dir)) {}

std::string RemoteChip::slug(const std::string& name) {
    std::string out;
    for (unsigned char c : name) {
        if (std::isalnum(c)) out += static_cast<char>(std::tolower(c));
        else if (!out.empty() && out.back() != '-') out += '-';
    }
    while (!out.empty() && out.back() == '-') out.pop_back();
    return out.empty() ? "remote" : out;
}

void RemoteChip::load() {
    devices_.clear();
    macros_.clear();
    std::error_code ec;
    std::vector<fs::path> files;
    for (const auto& entry : fs::directory_iterator(dir_, ec)) files.push_back(entry.path());
    std::sort(files.begin(), files.end());
    for (const auto& path : files) {
        std::ifstream in(path);
        std::string line;
        if (path.extension() == ".remote") {
            Device device;
            device.file = path.string();
            device.name = path.stem().string();
            while (std::getline(in, line)) {
                const auto eq = line.find('=');
                if (line.empty() || line[0] == '#' || eq == std::string::npos) continue;
                const std::string key = trim(line.substr(0, eq));
                const std::string value = trim(line.substr(eq + 1));
                if (key == "name") {
                    device.name = value;
                } else if (const size_t b = button_index(key); b < kButtonCount) {
                    device.codes[b] = ir::parse_ir_ctl(value);
                }
            }
            devices_.push_back(std::move(device));
        } else if (path.extension() == ".macro") {
            Macro macro;
            macro.name = path.stem().string();
            while (std::getline(in, line)) {
                const auto eq = line.find('=');
                if (line.empty() || line[0] == '#' || eq == std::string::npos) continue;
                const std::string key = trim(line.substr(0, eq));
                const std::string value = trim(line.substr(eq + 1));
                if (key == "name") {
                    macro.name = value;
                } else if (key == "step") {
                    const auto bar = value.find('|');
                    if (bar == std::string::npos) continue;
                    MacroStep step{trim(value.substr(0, bar)), trim(value.substr(bar + 1)), 0};
                    if (step.device == "wait") step.wait_s = std::max(0, std::atoi(step.button.c_str()));
                    macro.steps.push_back(std::move(step));
                }
            }
            if (!macro.steps.empty()) macros_.push_back(std::move(macro));
        }
    }
    loaded_ = true;
}

bool RemoteChip::save(const Device& device) {
    std::error_code ec;
    fs::create_directories(dir_, ec);
    const std::string tmp = device.file + ".tmp";
    {
        std::ofstream out(tmp);
        if (!out) {
            status_ = "can't write " + dir_;
            status_error_ = true;
            return false;
        }
        out << "# PET-os remote: one line per learned button (microseconds, +pulse -space)\n";
        out << "name=" << device.name << "\n";
        for (size_t i = 0; i < kButtonCount; ++i) {
            if (!device.codes[i].empty()) out << kButtons[i] << "=" << ir::to_text(device.codes[i]) << "\n";
        }
    }
    fs::rename(tmp, device.file, ec);
    return !ec;
}

void RemoteChip::enter() {
    if (!loaded_) load();
    view_ = View::Devices;
    status_.clear();
    touch();
}

void RemoteChip::leave() {
    // A half-learned remote is kept: everything captured so far is saved.
    if (view_ == View::Learn) finish_learn();
    view_ = View::Devices;
    ++generation_;  // drop any capture still running
}

void RemoteChip::open_pad(size_t index) {
    device_ = index;
    button_ = 0;
    view_ = View::Pad;
    status_ = "IR ready";
    status_error_ = false;
    touch();
}

void RemoteChip::start_learn(size_t index) {
    device_ = index;
    button_ = 0;
    view_ = View::Learn;
    listen();
}

void RemoteChip::listen() {
    learn_state_ = LearnState::Listening;
    learn_detail_.clear();
    learn_ms_ = 0;
    ++generation_;
    // One ir-ctl at a time: if one is still running, its (now stale) result
    // starts the next capture when it comes back.
    if (!capturing_) launch_capture();
    touch();
}

void RemoteChip::launch_capture() {
    capturing_ = true;
    ir_.run([dev = config_.ir_recv_device, gen = generation_]() {
        return run_ir_ctl({"ir-ctl", "-d", dev, "--receive", "--one-shot"}, IrResult::Kind::Capture,
                          kCaptureTimeoutMs, gen);
    });
}

void RemoteChip::next_button() {
    if (button_ + 1 >= kButtonCount) {
        finish_learn();
        return;
    }
    ++button_;
    listen();
}

void RemoteChip::finish_learn() {
    ++generation_;
    if (Device* d = device_ < devices_.size() ? &devices_[device_] : nullptr) {
        if (save(*d)) {
            status_ = std::to_string(d->learned()) + " buttons saved";
            status_error_ = false;
        }
    }
    view_ = View::Pad;
    button_ = 0;
    touch();
}

void RemoteChip::send(const ir::Raw& raw, const std::string& what) {
    if (raw.empty()) {
        status_ = what + " not learned yet ([L] learn)";
        status_error_ = true;
        touch();
        return;
    }
    sending_ = true;
    status_ = "sending " + what;
    status_error_ = false;
    ir_.run([dev = config_.ir_send_device, raw]() {
        IrResult result;
        char path[] = "/tmp/pet-os-ir-XXXXXX";
        const int fd = mkstemp(path);
        if (fd < 0) {
            result.error = "no temp file";
            return result;
        }
        const std::string body = ir::to_send_file(raw);
        const bool wrote = ::write(fd, body.data(), body.size()) == static_cast<ssize_t>(body.size());
        ::close(fd);
        if (!wrote) {
            ::unlink(path);
            result.error = "no temp file";
            return result;
        }
        result = run_ir_ctl({"ir-ctl", "-d", dev, std::string("--send=") + path}, IrResult::Kind::Send,
                            kSendTimeoutMs, 0);
        ::unlink(path);
        return result;
    });
    touch();
}

void RemoteChip::on_ir_result(const IrResult& result) {
    if (result.kind == IrResult::Kind::Send) {
        if (!sending_) return;  // a send we stopped caring about (macro stopped)
        sending_ = false;
        if (view_ == View::MacroRun && !macro_done_) {
            if (!result.ok) {
                status_ = "step " + std::to_string(step_ + 1) + ": " + result.error;
                status_error_ = true;
            }
            ++step_;
            run_step();
            return;
        }
        if (result.ok) {
            if (status_.rfind("sending ", 0) == 0) status_ = "sent " + status_.substr(8);
        } else {
            status_ = result.error;
            status_error_ = true;
        }
        touch();
        return;
    }
    // Capture.
    capturing_ = false;
    const bool listening = view_ == View::Learn && learn_state_ == LearnState::Listening;
    if (result.generation != generation_ || (!result.ok && result.error == "timeout")) {
        // Stale (the button changed) or nothing pressed yet: keep listening.
        if (listening) launch_capture();
        return;
    }
    if (!listening) return;
    if (result.ok) {
        devices_[device_].codes[button_] = result.raw;
        learn_state_ = LearnState::Captured;
        learn_detail_ = ir::describe(result.raw);
    } else {
        learn_state_ = LearnState::Failed;
        learn_detail_ = result.error;
    }
    touch();
}

void RemoteChip::start_macro(size_t index) {
    macro_ = index;
    step_ = 0;
    wait_left_ms_ = 0;
    macro_done_ = false;
    status_.clear();
    status_error_ = false;
    view_ = View::MacroRun;
    run_step();
}

void RemoteChip::run_step() {
    const Macro& macro = macros_[macro_];
    if (step_ >= macro.steps.size()) {
        macro_done_ = true;
        if (!status_error_) status_ = "done";
        touch();
        return;
    }
    const MacroStep& step = macro.steps[step_];
    if (step.device == "wait") {
        wait_left_ms_ = static_cast<uint32_t>(step.wait_s) * 1000u;
        touch();
        return;
    }
    const auto dev = std::find_if(devices_.begin(), devices_.end(), [&](const Device& d) { return d.name == step.device; });
    const size_t b = button_index(step.button);
    if (dev == devices_.end() || b >= kButtonCount || dev->codes[b].empty()) {
        status_ = "step " + std::to_string(step_ + 1) + ": no " + step.device + " " + step.button;
        status_error_ = true;
        ++step_;
        run_step();
        return;
    }
    send(dev->codes[b], step.device + " " + step.button);
}

void RemoteChip::move_pad(Key key) {
    // Row/column of the current button in the 4/5/6 grid.
    size_t row = 0, col = button_;
    while (col >= kRowSizes[row]) col -= kRowSizes[row++];
    if (key == Key::Left && col > 0) --col;
    if (key == Key::Right && col + 1 < kRowSizes[row]) ++col;
    if (key == Key::Up && row > 0) --row;
    if (key == Key::Down && row + 1 < kRowSizes.size()) ++row;
    col = std::min(col, kRowSizes[row] - 1);
    size_t index = col;
    for (size_t r = 0; r < row; ++r) index += kRowSizes[r];
    button_ = index;
}

bool RemoteChip::handle_key(Key key) {
    switch (view_) {
    case View::Devices:
        if (key == Key::Escape) return false;
        if (key == Key::Up && selected_ > 0) --selected_;
        if (key == Key::Down && selected_ < devices_.size()) ++selected_;
        if (key == Key::Enter) {
            if (selected_ < devices_.size()) {
                open_pad(selected_);
            } else {
                name_input_.clear();
                view_ = View::Name;
            }
        }
        break;
    case View::Name:
        if (key == Key::Escape) view_ = View::Devices;
        if (key == Key::Backspace && !name_input_.empty()) {
            do name_input_.pop_back();
            while (!name_input_.empty() && (static_cast<unsigned char>(name_input_.back()) & 0xC0) == 0x80);
        }
        if (key == Key::Enter && !trim(name_input_).empty()) {
            Device device;
            device.name = trim(name_input_);
            std::string file = dir_ + "/" + slug(device.name);
            std::error_code ec;
            for (int n = 2; fs::exists(file + ".remote", ec); ++n) file = dir_ + "/" + slug(device.name) + "-" + std::to_string(n);
            device.file = file + ".remote";
            devices_.push_back(std::move(device));
            selected_ = devices_.size() - 1;
            start_learn(selected_);
            return true;
        }
        break;
    case View::Pad:
        if (key == Key::Escape) {
            view_ = View::Devices;
            status_.clear();
        } else if (key == Key::Enter) {
            if (!sending_) send(devices_[device_].codes[button_], kButtons[button_]);
        } else {
            move_pad(key);
        }
        break;
    case View::Learn:
        if (key == Key::Escape) finish_learn();
        else if (key == Key::Enter) next_button();
        return true;
    case View::Macros:
        if (key == Key::Escape) view_ = View::Devices;
        if (key == Key::Up && macro_ > 0) --macro_;
        if (key == Key::Down && macro_ + 1 < macros_.size()) ++macro_;
        if (key == Key::Enter && macro_ < macros_.size()) start_macro(macro_);
        break;
    case View::MacroRun:
        if (key == Key::Escape || (key == Key::Enter && macro_done_)) {
            macro_done_ = true;  // a send still in flight finishes, nothing more runs
            view_ = View::Macros;
        }
        break;
    }
    touch();
    return true;
}

void RemoteChip::handle_char(const std::string& utf8) {
    if (view_ == View::Name) {
        if (name_input_.size() < 32 && !utf8.empty() && static_cast<unsigned char>(utf8[0]) >= 0x20) name_input_ += utf8;
        touch();
        return;
    }
    const char c = utf8.size() == 1 ? static_cast<char>(std::tolower(static_cast<unsigned char>(utf8[0]))) : 0;
    if (view_ == View::Devices || view_ == View::Pad) {
        if (c == 'l') {
            if (view_ == View::Pad) start_learn(device_);
            else if (selected_ < devices_.size()) start_learn(selected_);
            else handle_key(Key::Enter);  // "Learn new device" row
        } else if (c == 'm') {
            view_ = View::Macros;
            macro_ = 0;
            touch();
        }
    } else if (view_ == View::Learn) {
        if (c == 's') {
            next_button();
        } else if (c == 'r' && learn_state_ != LearnState::Listening) {
            listen();
        } else if (c == 't' && !sending_) {
            send(devices_[device_].codes[button_], kButtons[button_]);
        }
    }
}

void RemoteChip::tick(uint32_t elapsed_ms) {
    IrResult result;
    while (ir_.poll(result)) on_ir_result(result);
    if (view_ == View::Learn && learn_state_ == LearnState::Listening) {
        const uint32_t before = learn_ms_ / 100;
        learn_ms_ += elapsed_ms;
        if (learn_ms_ / 100 != before) touch();  // animate the listening bar
    }
    if (view_ == View::MacroRun && !macro_done_ && wait_left_ms_ > 0) {
        const uint32_t before = macro_wait_left_s();
        wait_left_ms_ = elapsed_ms >= wait_left_ms_ ? 0 : wait_left_ms_ - elapsed_ms;
        if (static_cast<uint32_t>(macro_wait_left_s()) != before) touch();
        if (wait_left_ms_ == 0) {
            ++step_;
            run_step();
        }
    }
}

} // namespace viewmodel::chips
