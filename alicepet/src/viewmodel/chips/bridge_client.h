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

// POST /api/pet/ask {"input": text}
BridgeReply bridge_ask(const model::PetConfig& config, const std::string& text);
// POST /api/pet/voice with WAV bytes (local whisper on the Mac, then the same session).
BridgeReply bridge_voice(const model::PetConfig& config, const std::string& wav);

} // namespace viewmodel::chips
