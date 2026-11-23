/*
 * SPDX-License-Identifier: MIT
 */

#include "viewmodel/chips/bridge_client.h"

#include "platform/json.h"
#include "platform/net.h"

#include <algorithm>
#include <atomic>

namespace viewmodel::chips {
namespace {

std::string base_url(const model::PetConfig& config) {
    std::string url = config.bridge_url;
    while (!url.empty() && url.back() == '/') url.pop_back();
    return url;
}

BridgeReply decode(const platform::net::HttpResponse& response) {
    BridgeReply out;
    if (!response.ok) {
        out.error = "Can't reach Navi (" + response.error + ").";
        return out;
    }
    if (response.status == 401) {
        out.error = "Navi bridge rejected my token. Check [bridge] token=.";
        return out;
    }
    platform::json::Value json;
    if (!platform::json::parse(response.body, json)) {
        out.error = "Navi bridge answered HTTP " + std::to_string(response.status) + " (not JSON).";
        return out;
    }
    if (const long long limit = json["max_input"].integer(); limit > 0) set_bridge_max_input(static_cast<size_t>(limit));
    out.transcript = json["transcript"].str();
    out.reply = json["reply"].str();
    out.ok = json["ok"].boolean() && !out.reply.empty();
    if (!out.ok) {
        out.error = json["error"].str();
        if (out.error.empty()) out.error = "Navi bridge answered HTTP " + std::to_string(response.status) + " with no reply.";
        if (response.status == 404) out.error = "This bridge has no voice route. Start it with bin/alicepet-bridge.sh.";
    }
    return out;
}

std::atomic<size_t> g_max_input{kBridgeMaxInput};

} // namespace

size_t bridge_max_input() { return g_max_input.load(); }

void set_bridge_max_input(size_t chars) {
    g_max_input.store(std::min<size_t>(std::max<size_t>(chars, kBridgeMaxInput), 16000));
}

std::string clip_for_bridge(const std::string& text, size_t max_chars) {
    if (max_chars == 0) max_chars = bridge_max_input();
    // JavaScript counts UTF-16 units; a code point above U+FFFF is two of them.
    size_t units = 0;
    size_t i = 0;
    while (i < text.size()) {
        const auto lead = static_cast<unsigned char>(text[i]);
        const size_t len = lead < 0x80 ? 1 : (lead >> 5) == 0x6 ? 2 : (lead >> 4) == 0xE ? 3 : (lead >> 3) == 0x1E ? 4 : 1;
        const size_t cost = len == 4 ? 2 : 1;
        if (units + cost > max_chars || i + len > text.size()) break;
        units += cost;
        i += len;
    }
    return text.substr(0, i);
}

BridgeReply bridge_ask(const model::PetConfig& config, const std::string& text) {
    if (config.bridge_url.empty()) {
        return {false, "", "", "No bridge set. Add [bridge] url= to /etc/alicepet.conf."};
    }
    return decode(platform::net::http_post_json(base_url(config) + "/api/pet/ask",
                                                "{\"input\":\"" + platform::net::json_escape(clip_for_bridge(text)) + "\"}",
                                                config.bridge_token, config.bridge_timeout_s * 1000));
}

BridgeReply bridge_voice(const model::PetConfig& config, const std::string& wav, const std::string& prefix) {
    if (config.bridge_url.empty()) {
        return {false, "", "", "No bridge set. Add [bridge] url= to /etc/alicepet.conf."};
    }
    std::vector<std::string> headers{"Content-Type: audio/wav"};
    if (!config.bridge_token.empty()) headers.push_back("Authorization: Bearer " + config.bridge_token);
    if (!prefix.empty()) headers.push_back("X-Pet-Prefix: " + platform::net::url_encode(prefix));
    // Transcription adds up to ~30 s on top of the normal ask timeout.
    return decode(platform::net::http_request("POST", base_url(config) + "/api/pet/voice", wav, headers,
                                              (config.bridge_timeout_s + 60) * 1000));
}

} // namespace viewmodel::chips
