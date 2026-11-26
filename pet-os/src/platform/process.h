/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <string>
#include <sys/types.h>
#include <vector>

// Child processes for chips: the mic recorder, TTS, ssh in a pty, the video
// player, and short capture runs (ip/ping) on worker threads.
namespace platform::proc {

// Splits a config command line on whitespace; '...' and "..." group words.
std::vector<std::string> split_command(const std::string& line);
// Expands a leading "~/" with $HOME.
std::string expand_home(const std::string& path);

// Starts argv[0] (PATH lookup) with stdout/stderr to /dev/null. If stdin_text is
// non-empty it is written to the child's stdin (TTS). Returns -1 on failure.
pid_t spawn(const std::vector<std::string>& argv, const std::string& stdin_text = "");
// Sends `sig`, waits up to wait_ms for exit, then SIGKILLs. Reaps the child.
// Returns the exit status (or -1).
int stop(pid_t pid, int sig, int wait_ms);
// Non-blocking: true while the child has not exited (reaps it once it has).
bool alive(pid_t pid, int* exit_status = nullptr);

// Runs to completion and captures stdout (+stderr if merge_stderr). Blocking:
// worker threads only. Returns the exit status, -1 if it could not start, or
// -2 on timeout (the child is killed).
int run_capture(const std::vector<std::string>& argv, std::string& out, int timeout_ms, bool merge_stderr = true,
                size_t max_bytes = 1024 * 1024);

// A program running on a pseudo-terminal (ssh). Reads never block.
class PtySession {
public:
    PtySession() = default;
    ~PtySession();
    PtySession(const PtySession&) = delete;
    PtySession& operator=(const PtySession&) = delete;

    bool start(const std::vector<std::string>& argv, int cols, int rows, std::string& error);
    // Appends whatever output is ready; returns false once the program exited and output is drained.
    bool read(std::string& out);
    void write(const std::string& data);
    void resize(int cols, int rows);
    bool running() const { return pid_ > 0; }
    // Hangs up (SIGHUP, then SIGKILL) and closes the pty.
    void close();
    int exit_status() const { return exit_status_; }

private:
    int fd_{-1};
    pid_t pid_{-1};
    int exit_status_{-1};
};

} // namespace platform::proc
