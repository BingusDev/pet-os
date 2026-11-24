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
    // [jellyfin] pc_direct = off | on. Jack In video normally reaches the PC through
    // the PET's relay, so no Jellyfin credential leaves the PET. "on" also offers
    // the PC a direct URL with a Jellyfin API key made for that play. Jellyfin
    // API keys are full administrator keys: only for PCs you own.
    bool jellyfin_pc_direct{false};
    // [jellyfin] pc_user / pc_password: an ordinary (non-administrator) Jellyfin
    // user made for PC playback. When set, each Jack In play logs that user in,
    // hands the PC a direct URL with that login's token, and logs it out when the
    // play ends. The password is a secret (alicepet.local.conf only). An
    // administrator account is refused.
    std::string jellyfin_pc_user;
    std::string jellyfin_pc_password;

    // [romm]: url + API token (Bearer). https URLs go through the system curl.
    std::string romm_url;
    std::string romm_token;

    // [retro]: emulator command ({core}, {rom} and {savedir} are replaced), where downloaded
    // ROMs are kept, and the libretro core for each RomM platform slug (core_<slug>=).
    std::string retro_player{"retroarch --fullscreen -L {core} -s {savedir} {rom}"};
    bool retro_sync_saves{true};
    std::string retro_dir{"~/.local/share/alicepet/roms"};
    // [retro] systems=: the only RomM platforms shown, as tabs in this order (slugs,
    // matched ignoring case and punctuation: "wonderswancolor" = "wonderswan-color").
    // Empty = every platform RomM has, after an ALL tab.
    std::vector<std::string> retro_systems;
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
    // Speed test after a good jack test (uses ~25 MB down, ~10 MB up per run).
    bool lan_speed_test{true};
    std::string lan_speed_down_url{"https://speed.cloudflare.com/__down?bytes=25000000"};
    std::string lan_speed_up_url{"https://speed.cloudflare.com/__up"};
    // [S] reports and the "who's new" memory of devices seen per network.
    std::string lan_report_dir{"~/lan-reports"};
    std::string lan_known_dir{"~/.config/alicepet/lan-known"};
    // [V] static-IP fallback when DHCP fails: tried in order ("cidr|gateway").
    std::vector<std::string> lan_static{"192.168.1.250/24|192.168.1.1", "192.168.0.250/24|192.168.0.1",
                                        "10.0.0.250/24|10.0.0.1"};
    bool lan_static_from_config{false};

    // [jackin]: the PC app dials the PET at listen:port over the USB network link
    // (and listen:tunnel_port for "Share PC Internet"; PET programs use the
    // proxy at 127.0.0.1:proxy_port). gadget_up/down switch the USB port to
    // device mode (drive + network) and back; empty = not managed (desktop).
    std::string jack_listen{"10.55.0.1"};
    int jack_port{7341};
    int jack_tunnel_port{7342};
    // PC playback relay on listen: media_port (video files), media_port + 1 (RomM web).
    int jack_media_port{7344};
    int jack_proxy_port{3128};
    std::string jack_gadget_up{"sudo -n /usr/libexec/alicepet/usb-gadget.sh up"};
    std::string jack_gadget_down{"sudo -n /usr/libexec/alicepet/usb-gadget.sh down"};
    // Who runs the PET: the PC app shows "OPERATOR: <name>" and labels chat with it
    // (never the Windows user name). [jackin] operator= (or owner=).
    std::string jack_operator{"Holden"};
    // [jackin] pc_control = ask | on. "ask" (default): a linked PC can't open or
    // drive chips until [K] is pressed on the PET. "on": chips are allowed from
    // the start of every session. Terminal always needs its own [K].
    bool jack_pc_control{false};
    // Where the PET keeps what it remembers about PCs: the trusted list
    // (jack-trusted.conf) and one report per session (jack-reports/).
    std::string jack_state_dir{"~/.local/share/alicepet"};
    // [jackin] reports = on | off: write a text report of each session at jack out.
    bool jack_reports{true};
    // [clock] set=: run with a unix time to set the PET's clock (no battery clock).
    // Used with the time of a trusted Jack In PC or of the RomM server. Empty = never.
    std::string clock_set{"sudo -n /usr/libexec/alicepet/set-clock.sh"};
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
