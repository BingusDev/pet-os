/*
 * SPDX-License-Identifier: MIT
 */

#include "viewmodel/chips/voice_chip.h"

#include "platform/process.h"
#include "viewmodel/alice_viewmodel.h"

#include <algorithm>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <unistd.h>

namespace viewmodel::chips {
namespace {

constexpr uint32_t kLevelMs = 80;
constexpr uint32_t kMinListenMs = 400;
constexpr size_t kWavHeader = 44;
constexpr size_t kBytesPerMs = 32;  // 16 kHz, mono, s16

std::string read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// RMS of `bytes` s16le samples starting at `offset`, scaled to 0..1 for the wave bars.
float rms_level(const std::string& path, size_t offset, size_t bytes) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return 0.0f;
    in.seekg(static_cast<std::streamoff>(offset));
    std::string chunk(bytes, '\0');
    in.read(chunk.data(), static_cast<std::streamsize>(bytes));
    const size_t got = static_cast<size_t>(in.gcount()) / 2;
    if (got == 0) return 0.0f;
    double sum = 0;
    for (size_t i = 0; i < got; ++i) {
        int16_t sample = 0;
        std::memcpy(&sample, chunk.data() + i * 2, 2);
        sum += static_cast<double>(sample) * sample;
    }
    const double rms = std::sqrt(sum / static_cast<double>(got)) / 32768.0;
    return static_cast<float>(std::clamp(rms * 6.0, 0.0, 1.0));
}

} // namespace

VoiceChip::VoiceChip(model::PetConfig config) : config_(std::move(config)) {
#if USE_DESKTOP
    // The simulator has no mic: ALICEPET_VOICE_WAV or [voice] desktop_wav stands in for it.
    if (const char* wav = std::getenv("ALICEPET_VOICE_WAV")) desktop_wav_ = wav;
    else desktop_wav_ = platform::proc::expand_home(config_.voice_desktop_wav);
#endif
    record_path_ = "/tmp/alicepet-ptt-" + std::to_string(getpid()) + ".wav";
}

VoiceChip::~VoiceChip() {
    if (recorder_ > 0) platform::proc::stop(recorder_, SIGKILL, 200);
    if (tts_ > 0) platform::proc::stop(tts_, SIGKILL, 200);
    std::remove(record_path_.c_str());
}

void VoiceChip::leave() {
    // The mic is never left on behind the user's back.
    if (state_ == State::Listening) {
        if (recorder_ > 0) platform::proc::stop(recorder_, SIGINT, 500);
        recorder_ = -1;
        state_ = State::Idle;
        touch();
    }
}

bool VoiceChip::take_go_home() {
    const bool home = go_home_;
    go_home_ = false;
    return home;
}

bool VoiceChip::handle_key(Key key) {
    switch (key) {
    case Key::PushToTalk:
        start_listening();
        return true;
    case Key::PushToTalkRelease:
        stop_listening();
        return true;
    case Key::Escape:
        if (state_ == State::Listening) {
            leave();  // cancel without sending
            return true;
        }
        return false;
    case Key::Up:
    case Key::Left:
        if (reply_page_ > 0) {
            --reply_page_;
            touch();
        }
        return true;
    case Key::Down:
    case Key::Right:
        if (reply_page_ + 1 < reply_pages()) {
            ++reply_page_;
            touch();
        }
        return true;
    default:
        return true;
    }
}

void VoiceChip::handle_char(const std::string& utf8) {
    if (utf8 == " ") {
        start_listening();
    } else if (utf8 == "r" || utf8 == "R") {
        if (state_ == State::Answer) speak(reply_);
    } else if (utf8 == "m" || utf8 == "M") {
        muted_ = !muted_;
        if (muted_ && tts_ > 0) {
            platform::proc::stop(tts_, SIGTERM, 200);
            tts_ = -1;
        }
        touch();
    } else if (utf8 == "t" || utf8 == "T") {
        go_home_ = true;
        touch();
    }
}

void VoiceChip::start_listening() {
    if (state_ == State::Listening || state_ == State::Sending) {
        return;
    }
    if (tts_ > 0) {
        platform::proc::stop(tts_, SIGTERM, 200);
        tts_ = -1;
    }
    error_.clear();
    levels_.assign(kWaveBars, 0.0f);
    listen_ms_ = 0;
    level_ms_ = 0;
    if (desktop_wav_.empty()) {
        std::remove(record_path_.c_str());
        auto argv = platform::proc::split_command(config_.voice_record_cmd);
        argv.push_back(record_path_);
        recorder_ = platform::proc::spawn(argv);
        if (recorder_ < 0) {
            fail("Mic recorder failed to start. Check [voice] record_cmd.");
            return;
        }
    }
    state_ = State::Listening;
    touch();
}

void VoiceChip::stop_listening() {
    if (state_ != State::Listening) {
        return;
    }
    std::string path = desktop_wav_;
    if (recorder_ > 0) {
        // arecord finalises the WAV header on SIGINT.
        int exit_status = platform::proc::stop(recorder_, SIGINT, 1500);
        (void)exit_status;
        recorder_ = -1;
        path = record_path_;
    }
    if (listen_ms_ < kMinListenMs && desktop_wav_.empty()) {
        fail("Too short. Hold SPACE while you talk.");
        return;
    }
    std::string wav = read_file(path);
    if (wav.size() <= kWavHeader) {
        fail(desktop_wav_.empty() ? "No audio from the mic." : "Can't read " + desktop_wav_);
        return;
    }
    state_ = State::Sending;
    sending_ms_ = 0;
    transcript_.clear();
    replies_.run([config = config_, wav = std::move(wav)]() { return bridge_voice(config, wav); });
    touch();
}

void VoiceChip::fail(const std::string& message) {
    error_ = message;
    state_ = State::Error;
    touch();
}

void VoiceChip::sample_level() {
    size_t offset = 0;
    std::string path;
    if (!desktop_wav_.empty()) {
        path = desktop_wav_;
        offset = kWavHeader + static_cast<size_t>(listen_ms_) * kBytesPerMs;
    } else {
        path = record_path_;
        std::ifstream in(path, std::ios::binary | std::ios::ate);
        const auto size = static_cast<size_t>(std::max<std::streamoff>(0, in.tellg()));
        offset = size > kWavHeader + kLevelMs * kBytesPerMs ? size - kLevelMs * kBytesPerMs : kWavHeader;
    }
    levels_.erase(levels_.begin());
    levels_.push_back(rms_level(path, offset, kLevelMs * kBytesPerMs));
}

void VoiceChip::tick(uint32_t elapsed_ms) {
    if (state_ == State::Listening) {
        listen_ms_ += elapsed_ms;
        level_ms_ += elapsed_ms;
        if (level_ms_ >= kLevelMs) {
            level_ms_ = 0;
            sample_level();
            touch();
        }
        if (recorder_ > 0 && !platform::proc::alive(recorder_)) {
            recorder_ = -1;
            fail("Mic recorder stopped. Check [voice] record_cmd.");
        } else if (listen_ms_ >= static_cast<uint32_t>(std::max(1, config_.voice_max_s)) * 1000u) {
            stop_listening();
        }
    } else if (state_ == State::Sending) {
        const int before = thinking_dots();
        sending_ms_ += elapsed_ms;
        if (thinking_dots() != before) touch();
    }
    if (tts_ > 0 && !platform::proc::alive(tts_)) {
        tts_ = -1;
        touch();
    }

    BridgeReply reply;
    while (replies_.poll(reply)) {
        transcript_ = AliceViewModel::screen_text(reply.transcript);
        if (!reply.ok) {
            fail(reply.error);
            continue;
        }
        reply_ = reply.reply;
        reply_lines_ = AliceViewModel::wrap(AliceViewModel::screen_text(reply.reply), kBubbleCols);
        reply_page_ = 0;
        state_ = State::Answer;
        speak(reply_);
        touch();
    }
}

void VoiceChip::speak(const std::string& text) {
    if (config_.voice_tts_cmd.empty() || muted_ || text.empty()) {
        return;
    }
    if (tts_ > 0) platform::proc::stop(tts_, SIGTERM, 200);
    tts_ = platform::proc::spawn(platform::proc::split_command(config_.voice_tts_cmd),
                                 AliceViewModel::screen_text(text) + "\n");
    touch();
}

bool VoiceChip::speaking() const {
    return tts_ > 0;
}

std::vector<std::string> VoiceChip::reply_page_lines() const {
    std::vector<std::string> page;
    for (size_t i = reply_page_ * kBubbleRows; i < reply_lines_.size() && i < (reply_page_ + 1) * kBubbleRows; ++i) {
        page.push_back(reply_lines_[i]);
    }
    return page;
}

size_t VoiceChip::reply_pages() const {
    return std::max<size_t>(1, (reply_lines_.size() + kBubbleRows - 1) / kBubbleRows);
}

} // namespace viewmodel::chips
