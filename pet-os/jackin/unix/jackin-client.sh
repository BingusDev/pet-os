#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# JACK IN client for macOS and Linux. Needs nothing installed: bash (3.2 or
# later), its /dev/tcp, sed, awk, ps, df, grep, and open (macOS) or xdg-open
# (Linux). It runs in Read mode: it never sends keys, never runs the PET's
# commands and never shares this computer's Internet. README.txt lists the gaps.
#
#   bash jackin-client.sh
#
# Environment (for tests; the defaults are the PET):
#   JACKIN_PET     the PET's address on the USB network   (10.55.0.1)
#   JACKIN_PORT    control port                           (7341)
#   JACKIN_OPENER  program that opens a URL               (open / xdg-open)
set -u
trap '' PIPE

PET_HOST=${JACKIN_PET:-10.55.0.1}
PET_PORT=${JACKIN_PORT:-7341}
APP_NAME="jackin-sh 1.0"
EXIT_CODE=0
READER_PID=

case "$(uname -s)" in
    Darwin) OS=macos; OPENER=${JACKIN_OPENER:-open} ;;
    Linux) OS=linux; OPENER=${JACKIN_OPENER:-xdg-open} ;;
    *) echo "JACK IN: this client is for macOS and Linux only." >&2; exit 1 ;;
esac

log() { printf '[%s] %s\n' "$(date +%H:%M:%S)" "$*"; }

# JSON string literal for $1.
jstr() {
    printf '"%s"' "$(printf '%s' "$1" |
        sed -e 's/\\/\\\\/g' -e 's/"/\\"/g' |
        tr '\t' ' ' |
        awk 'BEGIN { ORS = "" } NR > 1 { printf "\\n" } { printf "%s", $0 }')"
}

# String value of key $1 in the one-line JSON object $2. Stops at an escaped quote.
json_str() {
    printf '%s' "$2" | sed -n 's/.*"'"$1"'":"\([^"]*\)".*/\1/p' | head -1
}

# Raw value (string with its quotes, number, or bool) of key $1 in $2.
json_raw() {
    printf '%s' "$2" | sed -n 's/.*"'"$1"'":\([^,}]*\).*/\1/p' | head -1
}

send() {
    printf '%s\n' "$1" >&3 2>/dev/null || link_lost
}

# Asks the main shell to quit. Called from the reader, so it signals the main
# shell ($$ is the main PID in a background subshell too).
link_lost() {
    log "the link to the PET is gone (unplugged, or the PET left Jack In)."
    kill -USR1 $$ 2>/dev/null
    quit_for_sure
}

request_quit() {
    kill -TERM $$ 2>/dev/null
    quit_for_sure
}

# The main shell is usually sitting in `read` at the prompt, and an old bash
# (macOS ships 3.2) may not run its trap until Return is pressed. The trap
# handler kills this reader first, so getting here a second later means it never
# ran: end the main shell outright rather than leave a dead session open.
quit_for_sure() {
    sleep 1
    log "closed."
    kill -KILL $$ 2>/dev/null
}

host_name() {
    if [ "$OS" = macos ]; then
        scutil --get ComputerName 2>/dev/null || hostname -s
    else
        hostname -s 2>/dev/null || hostname
    fi
}

# [T] twice on the PET trusts this computer: the PET makes a token, this client
# keeps it (for this user only) and shows it back in every hello. The PET keeps
# the list and can forget this computer at any time.
TRUST_FILE=${JACKIN_TRUST_FILE:-${XDG_CONFIG_HOME:-$HOME/.config}/pet-os/jackin-trust}
# Before rev31 the app was called AlicePET: carry this computer's trust over once.
# The Navi's name; the PET sends its [navi] name after hello.
NAVI=Alice
OLD_TRUST_FILE=${XDG_CONFIG_HOME:-$HOME/.config}/alicepet/jackin-trust
if [ -z "${JACKIN_TRUST_FILE:-}" ] && [ ! -e "$TRUST_FILE" ] && [ -r "$OLD_TRUST_FILE" ]; then
    mkdir -p "$(dirname "$TRUST_FILE")" && (umask 077 && cp "$OLD_TRUST_FILE" "$TRUST_FILE") 2>/dev/null || true
fi

trust_token() {
    local token=""
    [ -r "$TRUST_FILE" ] && IFS= read -r token <"$TRUST_FILE"
    case "$token" in
        *[!0-9a-f]*) token="" ;;
    esac
    [ "${#token}" -eq 64 ] && printf '%s' "$token"
    return 0
}

save_trust() {
    local token=$1
    case "$token" in
        *[!0-9a-f]*) token="" ;;
    esac
    if [ "${#token}" -ne 64 ]; then
        rm -f "$TRUST_FILE"
        log "the PET no longer trusts this computer."
        return 0
    fi
    if mkdir -p "$(dirname "$TRUST_FILE")" && (umask 077 && printf '%s\n' "$token" >"$TRUST_FILE"); then
        log "the PET trusts this computer now: next time it needs no [K]."
    else
        log "the PET trusted this computer, but the token could not be saved here."
    fi
}

# `time` is this computer's clock: the PET has none of its own and only takes it
# from a computer it trusts, or when its own is certainly wrong.
hello_line() {
    printf '{"t":"hello","host":%s,"os":"%s","user":%s,"admin":false,"full":false,"input":false,"mode":"read","app":%s,"time":%s,"trust":"%s"}' \
        "$(jstr "$(host_name)")" "$OS" "$(jstr "$(id -un)")" "$(jstr "$APP_NAME")" "$(date +%s)" "$(trust_token)"
}

# ---- scan: read-only checks, the same fields the Windows app sends -----------

scan_log() {
    local ok=true
    [ "$2" = 0 ] || ok=false
    if [ "$ok" = true ]; then log "> $1"; else log "> $1   (failed)"; fi
    send "{\"t\":\"log\",\"text\":$(jstr "$1"),\"ok\":$ok}"
}

scan_cpu() {
    if [ "$OS" = macos ]; then
        top -l 2 -n 0 -s 1 2>/dev/null | awk '
            /CPU usage/ { for (i = 1; i <= NF; i++) if ($i == "idle") { v = $(i - 1); gsub("%", "", v); busy = int(100 - v) } }
            END { if (busy == "") exit 1; print busy }'
    else
        vmstat 1 2 2>/dev/null | tail -1 | awk '{ print 100 - $(NF - 2) }'
    fi
}

scan_ram() {
    if [ "$OS" = macos ]; then
        total=$(sysctl -n hw.memsize 2>/dev/null) || return 1
        vm_stat 2>/dev/null | awk -v total="$total" '
            /page size of/ { ps = $8 }
            /Pages active:/ { gsub(/\./, "", $3); a = $3 }
            /Pages wired down:/ { gsub(/\./, "", $4); w = $4 }
            /Pages occupied by compressor:/ { gsub(/\./, "", $5); c = $5 }
            END { if (ps == "" || total == "") exit 1; printf "%d", int(100 * (a + w + c) * ps / total) }'
    else
        awk '/^MemTotal:/ { t = $2 } /^MemAvailable:/ { a = $2 }
            END { if (t > 0) printf "%d", int(100 * (t - a) / t); else exit 1 }' /proc/meminfo
    fi
}

scan_disk_free() {
    df -Pk / 2>/dev/null | awk 'NR == 2 && $2 > 0 { printf "%d", int(100 * $4 / $2); found = 1 } END { exit !found }'
}

scan_startup() {
    if [ "$OS" = macos ]; then
        ls "$HOME/Library/LaunchAgents" 2>/dev/null | wc -l | tr -d ' '
    else
        ls "$HOME/.config/autostart" 2>/dev/null | wc -l | tr -d ' '
    fi
}

# Top five processes by resident memory, as {name, mb} objects separated by commas.
scan_top() {
    local sep=
    ps -axo rss=,comm= 2>/dev/null | sort -rn | head -5 |
        awk '{ rss = $1; sub(/^ *[0-9]+ +/, ""); n = $0; sub(/.*\//, "", n); printf "%s\t%d\n", n, int(rss / 1024) }' |
        while IFS='	' read -r name mb; do
            printf '%s{"name":%s,"mb":%s}' "$sep" "$(jstr "$name")" "$mb"
            sep=,
        done
}

do_scan() {
    local cpu ram free startup top rc
    cpu=$(scan_cpu); rc=$?; scan_log "CPU load" "$rc"
    ram=$(scan_ram); rc=$?; scan_log "memory in use" "$rc"
    free=$(scan_disk_free); rc=$?; scan_log "disk free on /" "$rc"
    startup=$(scan_startup); rc=$?; scan_log "login items" "$rc"
    top=$(scan_top); rc=$?; scan_log "processes by memory" "$rc"
    send "{\"t\":\"scan\",\"cpu\":${cpu:-null},\"ram\":${ram:-null},\"disk\":\"/\",\"disk_free\":${free:-null},\"startup\":${startup:-null},\"defender\":\"?\",\"top\":[${top}]}"
    log "scan done (read-only). Defender is Windows-only, so it shows as ?."
}

# ---- PET messages ------------------------------------------------------------

handle_alice() {
    local line=$1 text heard cmd ok
    text=$(json_str text "$line")
    heard=$(json_str heard "$line")
    cmd=$(json_str cmd "$line")
    ok=$(json_raw ok "$line")
    [ -n "$heard" ] && log "you (voice): $heard"
    if [ "$ok" = true ]; then log "$NAVI: $text"; else log "$NAVI couldn't answer: $text"; fi
    [ -n "$cmd" ] && log "$NAVI wants to run: $cmd (not run: this client runs no PET commands)"
}

handle_run() {
    local id cmd
    id=$(json_raw id "$1")
    cmd=$(json_str cmd "$1")
    log "PET asked to run: $cmd (not run: this client stays in Read mode)"
    send "{\"t\":\"result\",\"id\":${id:-null},\"ok\":false,\"reason\":\"read-only\",\"out\":\"\"}"
}

open_url() {
    if ! command -v "$OPENER" >/dev/null 2>&1; then
        log "no $OPENER on this computer; can't open $1"
        return 1
    fi
    "$OPENER" "$1" </dev/null >/dev/null 2>&1 || log "$OPENER failed for $1"
}

handle_play() {
    local line=$1 kind title url direct item
    kind=$(json_str kind "$line")
    title=$(json_str title "$line")
    url=$(json_str url "$line")
    direct=$(json_str direct "$line")
    item=$(json_str item "$line")
    [ -n "$url" ] || return 0
    if [ "$kind" = game ]; then
        log "opening $title in your browser (RomM web player via the PET)"
        open_url "$url"
        [ -n "$direct" ] && log "if it doesn't load, RomM itself: $direct"
        return 0
    fi
    log "playing $title in your default player. Pause, seek and volume aren't available from this client."
    open_url "$url"
    # Nothing reports back from the external player. "handoff" sends the PET back
    # to its list without telling Jellyfin the play stopped ("stopped" ended the
    # transcode the player had just started reading).
    send "{\"t\":\"progress\",\"item\":$(jstr "$item"),\"pos\":0,\"paused\":false,\"state\":\"handoff\"}"
}

handle() {
    local line=$1 t
    t=$(json_str t "$line")
    case "$t" in
        pet) log "operator: $(json_str operator "$line")" ;;
        navi) NAVI=$(json_str name "$line" | tr -cd '[:print:]' | cut -c1-12); [ -n "$NAVI" ] || NAVI=Alice; log "Navi: $NAVI" ;;
        trust) save_trust "$(json_str token "$line")" ;;
        denied) log "the PET wants [K] pressed on it before a chip opens for this computer." ;;
        chips)
            log "PET chips: $(printf '%s' "$line" | grep -o '"name":"[^"]*"' | sed 's/"name":"//; s/"$//' | paste -sd, -)" ;;
        alice) handle_alice "$line" ;;
        run) handle_run "$line" ;;
        scan) do_scan ;;
        ask) log "PET asks for $(json_str what "$line") access. This client can't grant it and stays in Read mode (see README.txt)." ;;
        agent_more)
            send '{"t":"agent_more","yes":false}'
            log "$NAVI wanted more steps; declined (no commands run here)." ;;
        agent_stop) log "$NAVI stopped early: $(json_str why "$line")" ;;
        net_open)
            send "{\"t\":\"net_fail\",\"id\":$(json_raw id "$line")}"
            log "PET asked to share this computer's Internet; the Mac/Linux client doesn't do that (see README.txt)." ;;
        play) handle_play "$line" ;;
        jackout)
            log "PET says jack out: $(json_str why "$line")"
            request_quit ;;
        *) ;;
    esac
}

reader_loop() {
    local line
    while IFS= read -r line; do
        handle "$line"
    done <&3
    link_lost
}

# ---- keyboard ----------------------------------------------------------------

show_help() {
    log "keys: c = say something to $NAVI   s = scan again   h = this help   q = jack out"
}

shutdown_now() {
    trap '' TERM INT HUP USR1
    [ -n "$READER_PID" ] && kill "$READER_PID" 2>/dev/null
    printf '{"t":"bye","why":"closed"}\n' >&3 2>/dev/null
    exec 3>&- 2>/dev/null
    log "closed."
    exit "$EXIT_CODE"
}

keyboard_loop() {
    local key text
    while printf '> '; IFS= read -r key; do
        case "$key" in
            c|C)
                printf 'to %s: ' "$NAVI"
                IFS= read -r text || break
                if [ -n "$text" ]; then
                    send "{\"t\":\"chat\",\"text\":$(jstr "$text")}"
                    log "you: $text"
                fi ;;
            s|S) do_scan ;;
            h|H|'?') show_help ;;
            q|Q) shutdown_now ;;
            '') ;;
            *) log "unknown key: $key (h for help)" ;;
        esac
    done
}

main() {
    # Open once: a probe connection would use up the PET's only accept.
    if ! exec 3<>"/dev/tcp/$PET_HOST/$PET_PORT" 2>/dev/null; then
        log "can't reach the PET at $PET_HOST:$PET_PORT."
        log "Plug the PET's USB-C cable into this computer, wait a few seconds, and run this again."
        exit 1
    fi
    if ! { ifconfig 2>/dev/null || ip -4 addr 2>/dev/null; } | grep -q '10\.55\.0\.'; then
        log "no 10.55.0.x address on this computer yet; the PET's network link may still be coming up."
    fi

    trap 'EXIT_CODE=1; shutdown_now' USR1
    trap 'shutdown_now' TERM INT HUP

    send "$(hello_line)"
    reader_loop &
    READER_PID=$!
    log "connected to the PET at $PET_HOST. Type h for keys."
    do_scan
    keyboard_loop
    # stdin closed (not a terminal): stay until the PET or the link ends.
    wait "$READER_PID" 2>/dev/null
    shutdown_now
}

main
