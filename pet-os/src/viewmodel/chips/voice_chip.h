/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "model/pet_config.h"
#include "viewmodel/chip_module.h"
#include "viewmodel/chips/bridge_client.h"

#include <sys/types.h>

#include <string>
#include <vector>

namespace viewmodel::chips {

// VOICE (chip 005): push-to-talk with Alice, mockups/comms.html screens 1-2.
// Hold SPACE -> record (mic only while held) -> release -> WAV to the bridge's
// /api/pet/voice (local whisper on the Mac) -> "You: ..." + Alice's reply.
// Optional reply TTS through [voice] tts_cmd (off by default).
class VoiceChip : public ChipModule {
public:
    enum class State { Idle, Listening, Sending, Answer, Error };

    static constexpr size_t kBubbleCols = 26;
    static constexpr size_t kBubbleRows = 6;
    static constexpr size_t kWaveBars = 24;

    explicit VoiceChip(model::PetConfig config);
    ~VoiceChip() override;

    const char* id() const override { return "voice"; }
    void leave() override;
    bool handle_key(Key key) override;
    void handle_char(const std::string& utf8) override;
    void tick(uint32_t elapsed_ms) override;
    bool keep_awake() const override { return state_ == State::Listening || state_ == State::Sending; }

    // Leaves to Home (the [T]type key) instead of the folder.
    bool take_go_home() override;

    State state() const { return state_; }
    uint32_t listen_ms() const { return listen_ms_; }
    // 0..1 mic levels, newest last (live from the WAV being written).
    const std::vector<float>& levels() const { return levels_; }
    const std::string& transcript() const { return transcript_; }
    std::vector<std::string> reply_page_lines() const;
    size_t reply_page() const { return reply_page_; }
    size_t reply_pages() const;
    const std::string& error() const { return error_; }
    int thinking_dots() const { return static_cast<int>((sending_ms_ / 400) % 4); }
    bool muted() const { return muted_; }
    bool tts_enabled() const { return !config_.voice_tts_cmd.empty(); }
    bool speaking() const;
    // True when audio comes from [voice] desktop_wav instead of a mic.
    bool simulated_mic() const { return !desktop_wav_.empty(); }

    // Starts / stops push-to-talk (also used by snapshot mode).
    void start_listening();
    void stop_listening();

private:
    void speak(const std::string& text);
    void sample_level();
    void fail(const std::string& message);

    model::PetConfig config_;
    std::string desktop_wav_;
    std::string record_path_;
    pid_t recorder_{-1};
    pid_t tts_{-1};
    State state_{State::Idle};
    uint32_t listen_ms_{0};
    uint32_t sending_ms_{0};
    uint32_t level_ms_{0};
    std::vector<float> levels_;
    std::string transcript_;
    std::string reply_;
    std::vector<std::string> reply_lines_;
    size_t reply_page_{0};
    std::string error_;
    bool muted_{false};
    bool go_home_{false};
    Mailbox<BridgeReply> replies_;
};

} // namespace viewmodel::chips
