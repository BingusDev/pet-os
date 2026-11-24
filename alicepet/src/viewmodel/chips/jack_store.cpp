/*
 * SPDX-License-Identifier: MIT
 */

#include "viewmodel/chips/jack_store.h"

#include "platform/process.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace viewmodel::chips::jack_store {
namespace fs = std::filesystem;
namespace {

std::string root(const std::string& dir) {
    return platform::proc::expand_home(dir);
}

std::string one_line(const std::string& text) {
    std::string out;
    for (const unsigned char c : text) {
        if (c == '\t' || c == '\n' || c == '\r') out += ' ';
        else if (c >= 0x20) out += static_cast<char>(c);
    }
    return out;
}

bool is_token(const std::string& token) {
    return token.size() == 64 &&
           std::all_of(token.begin(), token.end(), [](unsigned char c) { return std::isxdigit(c) != 0; });
}

// Writes `text` to `path` through a 0600 temp file, so a half-written list never replaces a good one.
bool write_private(const std::string& path, const std::string& text) {
    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);
    const std::string tmp = path + ".part";
    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) return false;
    const bool ok = ::write(fd, text.data(), text.size()) == static_cast<ssize_t>(text.size());
    ::close(fd);
    if (!ok) {
        fs::remove(tmp, ec);
        return false;
    }
    fs::rename(tmp, path, ec);
    return !ec;
}

std::string stamp(const char* format) {
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
    localtime_r(&now, &tm);
    char buf[32];
    std::strftime(buf, sizeof(buf), format, &tm);
    return buf;
}

} // namespace

std::string today() { return stamp("%Y-%m-%d"); }

std::string new_token() {
    unsigned char bytes[32];
    std::ifstream random("/dev/urandom", std::ios::binary);
    if (!random.read(reinterpret_cast<char*>(bytes), sizeof(bytes))) return "";
    static const char* kHex = "0123456789abcdef";
    std::string out;
    for (const unsigned char b : bytes) {
        out += kHex[b >> 4];
        out += kHex[b & 15];
    }
    return out;
}

std::vector<TrustedPc> load_trusted(const std::string& dir) {
    std::vector<TrustedPc> list;
    std::ifstream in(root(dir) + "/jack-trusted.conf");
    std::string line;
    while (std::getline(in, line) && list.size() < kMaxTrusted) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream fields(line);
        TrustedPc pc;
        std::getline(fields, pc.token, '\t');
        std::getline(fields, pc.host, '\t');
        std::getline(fields, pc.added, '\t');
        if (is_token(pc.token)) list.push_back(std::move(pc));
    }
    return list;
}

bool save_trusted(const std::string& dir, const std::vector<TrustedPc>& list) {
    std::string text = "# PCs trusted with [T] on the PET's JACK IN screen: token, PC name, date.\n"
                       "# Delete a line (or press [T] again while that PC is linked) to forget it.\n";
    for (const auto& pc : list) {
        if (is_token(pc.token)) text += pc.token + "\t" + one_line(pc.host) + "\t" + one_line(pc.added) + "\n";
    }
    return write_private(root(dir) + "/jack-trusted.conf", text);
}

int find_trusted(const std::vector<TrustedPc>& list, const std::string& token) {
    if (!is_token(token)) return -1;
    int found = -1;
    for (size_t i = 0; i < list.size(); ++i) {
        if (list[i].token.size() != token.size()) continue;
        unsigned char diff = 0;
        for (size_t c = 0; c < token.size(); ++c) {
            diff |= static_cast<unsigned char>(std::tolower(static_cast<unsigned char>(list[i].token[c])) ^
                                               std::tolower(static_cast<unsigned char>(token[c])));
        }
        if (diff == 0) found = static_cast<int>(i);
    }
    return found;
}

std::string safe_host(const std::string& host) {
    std::string out;
    for (const unsigned char c : host) {
        if (std::isalnum(c) || c == '-' || c == '_') out += static_cast<char>(c);
        if (out.size() >= 48) break;
    }
    return out.empty() ? "pc" : out;
}

LastVisit load_last(const std::string& dir, const std::string& host) {
    LastVisit visit;
    std::ifstream in(root(dir) + "/jack-reports/" + safe_host(host) + ".last");
    std::string line;
    if (!std::getline(in, line)) return visit;
    std::istringstream fields(line);
    std::string cpu, ram, disk, startup;
    std::getline(fields, visit.when, '\t');
    std::getline(fields, cpu, '\t');
    std::getline(fields, ram, '\t');
    std::getline(fields, disk, '\t');
    std::getline(fields, startup, '\t');
    std::getline(fields, visit.defender, '\t');
    if (visit.when.empty()) return visit;
    const auto num = [](const std::string& v) { return v.empty() ? -1 : std::atoi(v.c_str()); };
    visit.cpu = num(cpu);
    visit.ram = num(ram);
    visit.disk_free = num(disk);
    visit.startup = num(startup);
    visit.have = true;
    return visit;
}

bool save_last(const std::string& dir, const std::string& host, const LastVisit& visit) {
    const std::string line = one_line(visit.when) + "\t" + std::to_string(visit.cpu) + "\t" + std::to_string(visit.ram) +
                             "\t" + std::to_string(visit.disk_free) + "\t" + std::to_string(visit.startup) + "\t" +
                             one_line(visit.defender) + "\n";
    return write_private(root(dir) + "/jack-reports/" + safe_host(host) + ".last", line);
}

std::string last_text(const LastVisit& visit, const std::string& disk) {
    if (!visit.have) return "";
    std::string out = "last visit " + visit.when + ":";
    std::string sep = " ";
    const auto part = [&](const std::string& p) {
        out += sep + p;
        sep = ", ";
    };
    if (visit.ram >= 0) part("RAM " + std::to_string(visit.ram) + "%");
    if (visit.disk_free >= 0) part((disk.empty() ? "disk" : disk) + " " + std::to_string(visit.disk_free) + "% free");
    if (visit.startup >= 0) part(std::to_string(visit.startup) + " startup items");
    if (!visit.defender.empty() && visit.defender != "?") part("Defender " + visit.defender);
    return out;
}

std::string write_report(const std::string& dir, const std::string& host, const std::string& text) {
    const std::string folder = root(dir) + "/jack-reports";
    std::string path = folder + "/" + safe_host(host) + "-" + stamp("%Y%m%d-%H%M%S") + ".txt";
    std::error_code ec;
    // Two sessions in the same second (a relink): keep both.
    for (int n = 2; fs::exists(path, ec) && n < 100; ++n) {
        path = folder + "/" + safe_host(host) + "-" + stamp("%Y%m%d-%H%M%S") + "-" + std::to_string(n) + ".txt";
    }
    if (!write_private(path, text)) return "";
    std::vector<fs::path> reports;
    for (const auto& entry : fs::directory_iterator(folder, ec)) {
        if (entry.path().extension() == ".txt") reports.push_back(entry.path());
    }
    if (reports.size() > kMaxReports) {
        // Names end in the time, but start with the host: order by time written.
        std::sort(reports.begin(), reports.end(), [](const fs::path& a, const fs::path& b) {
            std::error_code e1, e2;
            return fs::last_write_time(a, e1) < fs::last_write_time(b, e2);
        });
        for (size_t i = 0; i + kMaxReports < reports.size(); ++i) fs::remove(reports[i], ec);
    }
    return path;
}

} // namespace viewmodel::chips::jack_store
