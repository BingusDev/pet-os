/*
 * SPDX-License-Identifier: MIT
 */

#include "process.h"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <thread>
#include <unistd.h>

#if defined(__APPLE__)
#include <util.h>
#else
#include <pty.h>
#endif

namespace platform::proc {
namespace {

std::vector<char*> c_argv(const std::vector<std::string>& argv) {
    std::vector<char*> out;
    for (const auto& arg : argv) out.push_back(const_cast<char*>(arg.c_str()));
    out.push_back(nullptr);
    return out;
}

void redirect_to_null(int target) {
    const int null_fd = ::open("/dev/null", O_RDWR);
    if (null_fd >= 0) {
        dup2(null_fd, target);
        if (null_fd > 2) ::close(null_fd);
    }
}

} // namespace

std::vector<std::string> split_command(const std::string& line) {
    std::vector<std::string> out;
    std::string word;
    bool in_word = false;
    char quote = 0;
    for (const char c : line) {
        if (quote) {
            if (c == quote) quote = 0;
            else word += c;
        } else if (c == '\'' || c == '"') {
            quote = c;
            in_word = true;
        } else if (c == ' ' || c == '\t') {
            if (in_word) out.push_back(word);
            word.clear();
            in_word = false;
        } else {
            word += c;
            in_word = true;
        }
    }
    if (in_word) out.push_back(word);
    return out;
}

std::string expand_home(const std::string& path) {
    if (path.rfind("~/", 0) == 0) {
        const char* home = std::getenv("HOME");
        return std::string(home ? home : "") + path.substr(1);
    }
    return path;
}

pid_t spawn(const std::vector<std::string>& argv, const std::string& stdin_text) {
    if (argv.empty()) return -1;
    int in_pipe[2] = {-1, -1};
    if (!stdin_text.empty() && pipe(in_pipe) != 0) return -1;
    const pid_t pid = fork();
    if (pid < 0) {
        if (in_pipe[0] >= 0) {
            ::close(in_pipe[0]);
            ::close(in_pipe[1]);
        }
        return -1;
    }
    if (pid == 0) {
        setpgid(0, 0);
        if (in_pipe[0] >= 0) {
            dup2(in_pipe[0], 0);
            ::close(in_pipe[0]);
            ::close(in_pipe[1]);
        } else {
            redirect_to_null(0);
        }
        redirect_to_null(1);
        redirect_to_null(2);
        auto args = c_argv(argv);
        execvp(args[0], args.data());
        _exit(127);
    }
    if (in_pipe[0] >= 0) {
        ::close(in_pipe[0]);
        // Small payloads (a reply for TTS) fit the pipe buffer; a short write is acceptable.
        const ssize_t written = ::write(in_pipe[1], stdin_text.data(), stdin_text.size());
        (void)written;
        ::close(in_pipe[1]);
    }
    return pid;
}

bool alive(pid_t pid, int* exit_status) {
    if (pid <= 0) return false;
    int status = 0;
    const pid_t r = waitpid(pid, &status, WNOHANG);
    if (r == 0) return true;
    if (exit_status) *exit_status = r == pid && WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return false;
}

int stop(pid_t pid, int sig, int wait_ms) {
    if (pid <= 0) return -1;
    kill(pid, sig);
    int status = 0;
    for (int waited = 0; waited <= wait_ms; waited += 20) {
        const pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        if (r < 0) return -1;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    kill(pid, SIGKILL);
    waitpid(pid, &status, 0);
    return -1;
}

int run_capture(const std::vector<std::string>& argv, std::string& out, int timeout_ms, bool merge_stderr) {
    if (argv.empty()) return -1;
    int out_pipe[2];
    if (pipe(out_pipe) != 0) return -1;
    const pid_t pid = fork();
    if (pid < 0) {
        ::close(out_pipe[0]);
        ::close(out_pipe[1]);
        return -1;
    }
    if (pid == 0) {
        setpgid(0, 0);
        redirect_to_null(0);
        dup2(out_pipe[1], 1);
        if (merge_stderr) dup2(out_pipe[1], 2);
        else redirect_to_null(2);
        ::close(out_pipe[0]);
        ::close(out_pipe[1]);
        auto args = c_argv(argv);
        execvp(args[0], args.data());
        _exit(127);
    }
    ::close(out_pipe[1]);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    char buffer[4096];
    bool timed_out = false;
    while (true) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
        if (left <= 0) {
            timed_out = true;
            break;
        }
        pollfd pfd{out_pipe[0], POLLIN, 0};
        const int rc = poll(&pfd, 1, static_cast<int>(left));
        if (rc < 0 && errno == EINTR) continue;
        if (rc <= 0) {
            timed_out = rc == 0;
            break;
        }
        const ssize_t n = ::read(out_pipe[0], buffer, sizeof(buffer));
        if (n <= 0) break;
        if (out.size() < 1024 * 1024) out.append(buffer, static_cast<size_t>(n));
    }
    ::close(out_pipe[0]);
    if (timed_out) {
        kill(-pid, SIGKILL);
        stop(pid, SIGKILL, 200);
        return -2;
    }
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

PtySession::~PtySession() {
    close();
}

bool PtySession::start(const std::vector<std::string>& argv, int cols, int rows, std::string& error) {
    close();
    if (argv.empty()) {
        error = "no command";
        return false;
    }
    winsize ws{};
    ws.ws_col = static_cast<unsigned short>(cols);
    ws.ws_row = static_cast<unsigned short>(rows);
    int master = -1;
    const pid_t pid = forkpty(&master, nullptr, nullptr, &ws);
    if (pid < 0) {
        error = std::string("forkpty: ") + std::strerror(errno);
        return false;
    }
    if (pid == 0) {
        setenv("TERM", "vt100", 1);
        setenv("LANG", "C.UTF-8", 1);
        auto args = c_argv(argv);
        execvp(args[0], args.data());
        _exit(127);
    }
    fcntl(master, F_SETFL, fcntl(master, F_GETFL) | O_NONBLOCK);
    fd_ = master;
    pid_ = pid;
    exit_status_ = -1;
    return true;
}

bool PtySession::read(std::string& out) {
    if (fd_ < 0) return false;
    char buffer[4096];
    while (true) {
        const ssize_t n = ::read(fd_, buffer, sizeof(buffer));
        if (n > 0) {
            out.append(buffer, static_cast<size_t>(n));
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EINTR)) break;
        // EOF / EIO: the program exited (Linux reports EIO on the master).
        int status = -1;
        if (pid_ > 0 && !alive(pid_, &status)) {
            exit_status_ = status;
            pid_ = -1;
        }
        ::close(fd_);
        fd_ = -1;
        return false;
    }
    if (pid_ > 0) {
        int status = -1;
        if (!alive(pid_, &status)) {
            exit_status_ = status;
            pid_ = -1;
        }
    }
    return true;
}

void PtySession::write(const std::string& data) {
    if (fd_ < 0) return;
    size_t done = 0;
    while (done < data.size()) {
        const ssize_t n = ::write(fd_, data.data() + done, data.size() - done);
        if (n > 0) {
            done += static_cast<size_t>(n);
        } else if (n < 0 && errno == EAGAIN) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        } else {
            break;
        }
    }
}

void PtySession::resize(int cols, int rows) {
    if (fd_ < 0) return;
    winsize ws{};
    ws.ws_col = static_cast<unsigned short>(cols);
    ws.ws_row = static_cast<unsigned short>(rows);
    ioctl(fd_, TIOCSWINSZ, &ws);
}

void PtySession::close() {
    if (pid_ > 0) {
        exit_status_ = stop(pid_, SIGHUP, 300);
        pid_ = -1;
    }
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

} // namespace platform::proc
