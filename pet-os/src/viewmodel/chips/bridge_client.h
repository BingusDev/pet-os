/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "model/pet_config.h"

#include <string>

namespace viewmodel::chips {

// Blocking calls to the Talk-to-Alice bridge (cardputer-pet-terminal protocol +
// bin/voice-route.mjs). Worker threads only.
struct BridgeReply {
    bool ok{false};
    std::string reply;       // Alice's answer
    std::string transcript;  // voice only: what whisper heard
    std::string error;       // short, screen-ready
};

// The bridge keeps only the first 600 characters of an ask (server.mjs
// `input.slice(0, 600)`). Prompts built on the device must fit, or put what matters first.
constexpr size_t kBridgeMaxInput = 600;
// A newer bridge says how much it takes (`max_input` in every ask reply, 4000 by
// default). Until one has answered, this is kBridgeMaxInput. Thread-safe.
size_t bridge_max_input();
void set_bridge_max_input(size_t chars);  // clamped to 600..16000

// Cuts `text` to at most `max_chars` characters as JavaScript counts them (UTF-16
// units), never inside a code point. 0 = bridge_max_input().
std::string clip_for_bridge(const std::string& text, size_t max_chars = 0);

// POST /api/pet/ask {"input": text}
BridgeReply bridge_ask(const model::PetConfig& config, const std::string& text);
// POST /api/pet/voice with WAV bytes (local whisper on the Mac, then the same session).
// `prefix` (optional) goes in front of the transcript on the bridge (X-Pet-Prefix).
BridgeReply bridge_voice(const model::PetConfig& config, const std::string& wav, const std::string& prefix = "");

} // namespace viewmodel::chips
