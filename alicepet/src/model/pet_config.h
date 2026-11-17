/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <map>
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

// TERMINAL bookmark: `ssh <target>`; target may be an ~/.ssh/config alias or user@host.
struct SshBookmark {
    std::string label;   // "grandworld-pi"
    std::string target;  // "holden@100.103.43.8" or "grandworld-pi"
    std::string note;    // "Pi · home"
    int port{0};         // 0 = ssh default / ssh_config
};

struct PetConfig {
    // Talk-to-Alice bridge (cardputer-pet-terminal protocol).
    std::string bridge_url;
    std::string bridge_token;
    int bridge_timeout_s{60};

    int netscan_timeout_ms{1500};
    std::vector<HostConfig> hosts;

    // [power]: idle seconds before the PET naps (0 = never); backlight sysfs dir ("auto" = first in /sys/class/backlight).
    int sleep_after_s{300};
    std::string backlight{"auto"};
    // Dim the backlight to dim_percent of max after this many idle seconds (0 = never dim).
    int dim_after_s{45};
    int dim_percent{15};
    // Per-chip power profiles (platform/power.h): cores, CPU clock, Wi-Fi power
    // save, Bluetooth and the ACT LED. Off until checked on the real board.
    bool power_profiles{false};

    // [voice]: recorder writes a WAV to the path appended as the last argument and stops on SIGINT.
    std::string voice_record_cmd{"arecord -q -D default -f S16_LE -r 16000 -c 1 -t wav"};
    int voice_max_s{30};
    // Optional reply TTS: the reply text is piped to this command's stdin. Empty = off.
    std::string voice_tts_cmd;
    // Desktop only: send this WAV instead of recording (no mic in the simulator).
    std::string voice_desktop_wav;

    // [terminal]
    std::string ssh_cmd{"ssh"};
    std::vector<SshBookmark> bookmarks;
    // Also list Host entries from this ssh_config that are not bookmarked ("" = off).
    std::string ssh_config_import{"~/.ssh/config"};

    // [jellyfin]: api_key is a secret; it lives only in /etc/alicepet.conf on the device.
    std::string jellyfin_url;
    std::string jellyfin_api_key;
    std::string jellyfin_user;  // user name or id; empty = first user
    std::string jellyfin_player{"mpv --really-quiet --fs --vo=drm --hwdec=auto"};
    int jellyfin_max_bitrate{450000};
    int jellyfin_width{320};

    // [romm]: url + API token (Bearer). https URLs go through the system curl.
    std::string romm_url;
    std::string romm_token;

    // [retro]: emulator command ({core}, {rom} and {savedir} are replaced), where downloaded
    // ROMs are kept, and the libretro core for each RomM platform slug (core_<slug>=).
    std::string retro_player{"retroarch --fullscreen -L {core} -s {savedir} {rom}"};
    bool retro_sync_saves{true};
    std::string retro_dir{"~/.local/share/alicepet/roms"};
    std::map<std::string, std::string> retro_cores{
        {"gb", "/usr/lib/libretro/gambatte_libretro.so"},
        {"gbc", "/usr/lib/libretro/gambatte_libretro.so"},
        {"gba", "/usr/lib/libretro/mgba_libretro.so"},
        {"nes", "/usr/lib/libretro/fceumm_libretro.so"},
        {"snes", "/usr/lib/libretro/snes9x_libretro.so"},
        {"genesis-slash-megadrive", "/usr/lib/libretro/genesis_plus_gx_libretro.so"},
    };

    // [remote]: IR via LIRC's ir-ctl (v4l-utils). Learned codes live in remote_dir.
    std::string ir_send_device{"/dev/lirc0"};
    std::string ir_recv_device{"/dev/lirc1"};
    std::string remote_dir{"~/.config/alicepet/remotes"};

    // [lantech]: interface to test (empty = first non-loopback with a carrier).
    std::string lan_interface;
};

// Parses the INI-style /etc/alicepet.conf. Unknown keys are ignored; malformed
// host lines are skipped and reported through `warnings`.
PetConfig parse_pet_config(const std::string& text, std::vector<std::string>* warnings = nullptr);

// Reads the file at `path`. Returns false (and leaves defaults) if it cannot be opened.
// Also reads the optional personal overlay local_config_path(path) after it.
bool load_pet_config(const std::string& path, PetConfig& out, std::vector<std::string>* warnings = nullptr);
// "/etc/alicepet.conf" -> "/etc/alicepet.local.conf".
std::string local_config_path(const std::string& path);

} // namespace model
