/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <string>

namespace platform::mpv {

// Sends one JSON IPC command line to mpv's --input-ipc-server socket and reads
// the first reply line that carries "error". Blocking (short timeout): worker
// threads or quick UI-thread control commands only.
bool command(const std::string& socket_path, const std::string& json, std::string* reply = nullptr,
             int timeout_ms = 300);

// Current playback position in seconds via get_property time-pos (false if unknown).
bool time_pos(const std::string& socket_path, double& seconds);

} // namespace platform::mpv
