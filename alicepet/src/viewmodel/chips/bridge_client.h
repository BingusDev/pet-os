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

// Cuts `text` to at most `max_chars` characters as JavaScript counts them (UTF-16
// units), never inside a code point.
std::string clip_for_bridge(const std::string& text, size_t max_chars = kBridgeMaxInput);

// POST /api/pet/ask {"input": text}
BridgeReply bridge_ask(const model::PetConfig& config, const std::string& text);
// POST /api/pet/voice with WAV bytes (local whisper on the Mac, then the same session).
BridgeReply bridge_voice(const model::PetConfig& config, const std::string& wav);

} // namespace viewmodel::chips
