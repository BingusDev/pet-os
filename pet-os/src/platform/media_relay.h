/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <memory>
#include <string>
#include <vector>

namespace platform::jack {

// A small HTTP reverse proxy on the Jack In link (10.55.0.1 only), so the PC can
// play the PET's media without holding the PET's server credentials.
// Upstream requests are made by curl (http or https, Range passed through, body
// streamed back as is); the credentials are added on the PET and never leave it.
//
// Two kinds of route:
//  - files: GET/HEAD /m/<token>/<name> -> one upstream URL (Jellyfin streams).
//  - site:  a whole upstream origin (RomM's web player). The PC opens
//    /__pet/<token>?go=<path>, which sets a cookie and redirects to <path>;
//    every request carrying that cookie is forwarded to the origin.
// Tokens are 128-bit random and live until stop() (the link drops).
class MediaRelay {
public:
    MediaRelay();
    ~MediaRelay();
    MediaRelay(const MediaRelay&) = delete;
    MediaRelay& operator=(const MediaRelay&) = delete;

    // Listens on address:port. Returns "" or an error. Already running: "".
    std::string start(const std::string& address, int port);
    // Closes the listener, forgets every route and ends open streams.
    void stop();
    bool running() const;
    int port() const;
    int active() const;  // requests in flight

    // "/m/<token>/<name>" for the PC (prefix with http://address:port).
    std::string add_file(const std::string& upstream_url, const std::vector<std::string>& headers,
                         const std::string& name);
    // Sets the site origin (scheme://host[:port]) and headers; returns the gate
    // path "/__pet/<token>?go=" + url-encoded `path` for the PC.
    std::string set_site(const std::string& origin, const std::vector<std::string>& headers, const std::string& path);

    // Public for tests: rewrites one upstream response head for the PC
    // (Connection: close, cookies without Secure/Domain, Location made relative).
    static std::string rewrite_response_head(const std::string& head, const std::string& origin);

private:
    struct State;
    std::shared_ptr<State> state_;
};

} // namespace platform::jack
