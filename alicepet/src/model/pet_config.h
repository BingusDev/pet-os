/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <string>
#include <vector>

namespace model {

// How NET SCAN decides a host is up.
enum class ProbeKind { Tcp, Ps5 };

struct HostConfig {
    std::string name;
    std::string address;
    ProbeKind probe{ProbeKind::Tcp};
    int port{22};

    // "tcp:22" / "ps5", for the details view.
    std::string probe_label() const;
};

struct PetConfig {
    // Talk-to-Alice bridge (cardputer-pet-terminal protocol).
    std::string bridge_url;
    std::string bridge_token;
    int bridge_timeout_s{60};

    int netscan_timeout_ms{1500};
    std::vector<HostConfig> hosts;
};

// Parses the INI-style /etc/alicepet.conf. Unknown keys are ignored; malformed
// host lines are skipped and reported through `warnings`.
PetConfig parse_pet_config(const std::string& text, std::vector<std::string>* warnings = nullptr);

// Reads the file at `path`. Returns false (and leaves defaults) if it cannot be opened.
bool load_pet_config(const std::string& path, PetConfig& out, std::vector<std::string>* warnings = nullptr);

} // namespace model
