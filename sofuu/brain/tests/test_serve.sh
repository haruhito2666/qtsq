#!/usr/bin/env bash
# tests/test_serve.sh — `sfb serve` (W2.4 loopback broker) end-to-end.
#
# Covers: ready JSON on stdout, writes through the socket, count/flush over the
# socket, SIGINT shutdown (exit 0 + socket unlinked + brain flushed), refusal to
# steal a live socket, taking over a stale one, and --socket/--dim.
#
# Requires: ./sfb and ./tests/chaos_worker already built (make all
# tests/chaos_worker). The only thing this script compiles is its own socket
# client, into a /tmp work dir, with the module's include/link flags.
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT" || { echo "cannot cd to $ROOT"; exit 2; }

BIN="${BIN:-./sfb}"
WORKER="${WORKER:-./tests/chaos_worker}"
TMP="$(mktemp -d /tmp/sfb_serve.XXXXXX)"
PASS=0; FAIL=0; SKIP=0
SPIDS=()

say()  { printf '\n\033[1m── %s\033[0m\n' "$*"; }
ok()   { PASS=$((PASS+1)); printf '  \033[32mPASS\033[0m %s\n' "$*"; }
bad()  { FAIL=$((FAIL+1)); printf '  \033[31mFAIL\033[0m %s\n' "$*"; }
skip() { SKIP=$((SKIP+1)); printf '  \033[33mSKIP\033[0m %s\n' "$*"; }

cleanup() {
    local p
    for p in ${SPIDS[@]+"${SPIDS[@]}"}; do
        [ -n "$p" ] || continue
        kill -9 "$p" 2>/dev/null
    done
    for p in ${SPIDS[@]+"${SPIDS[@]}"}; do
        [ -n "$p" ] || continue
        wait "$p" 2>/dev/null
    done
    if [ "$FAIL" -eq 0 ]; then
        rm -rf "$TMP"
    else
        printf 'artifacts: %s\n' "$TMP"
    fi
}
trap cleanup EXIT

[ -x "$BIN" ]    || { echo "missing $BIN (run make first)"; exit 2; }
[ -x "$WORKER" ] || { echo "missing $WORKER (run make first)"; exit 2; }

# ── tiny socket client (the only thing this script builds; /tmp only) ─────
cat >"$TMP/serve_client.c" <<'EOF'
/* serve_client.c — throwaway client for tests/test_serve.sh (never installed). */
#include "sfb.h"
#include "sfb_broker.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static void fill(float *v, uint32_t dim, uint32_t seed)
{
    uint32_t s = seed * 2654435761u + 12345u;
    uint32_t i;
    for (i = 0; i < dim; i++) {
        s = s * 1103515245u + 12345u;
        v[i] = ((float)((s >> 8) & 0xFFFF) / 32768.0f) - 1.0f;
    }
}

int main(int argc, char **argv)
{
    const char *mode, *sock;
    uint32_t dim, n, i;

    if (argc < 3) {
        fprintf(stderr, "usage: %s put|count|flush|stale <socket> [dim] [n]\n", argv[0]);
        return 2;
    }
    mode = argv[1];
    sock = argv[2];
    dim = argc > 3 ? (uint32_t)atoi(argv[3]) : 64u;
    n = argc > 4 ? (uint32_t)atoi(argv[4]) : 3u;

    if (strcmp(mode, "stale") == 0) {
        /* Leave a bound-then-closed socket file: a stale path, no listener. */
        struct sockaddr_un a;
        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0 || strlen(sock) >= sizeof a.sun_path) return 3;
        memset(&a, 0, sizeof a);
        a.sun_family = AF_UNIX;
        snprintf(a.sun_path, sizeof a.sun_path, "%s", sock);
        unlink(sock);
        if (bind(fd, (struct sockaddr *)&a, sizeof a) != 0) return 4;
        close(fd);
        printf("{\"ok\":true,\"stale\":\"%s\"}\n", sock);
        return 0;
    }

    if (strcmp(mode, "put") == 0) {
        float *v = malloc((size_t)dim * sizeof *v);
        if (v == NULL) return 3;
        for (i = 0; i < n; i++) {
            sfb_remember_result_t r;
            char text[64];
            int rc;
            fill(v, dim, i + 1u);
            snprintf(text, sizeof text, "serve record %u", i);
            rc = sfb_broker_remember(sock, v, dim, text, &r);
            if (rc != SFB_OK) {
                printf("{\"ok\":false,\"error\":\"sfb_broker_remember\",\"rc\":%d,"
                       "\"name\":\"%s\",\"at\":%u}\n", rc, sfb_strerror(rc), i);
                free(v);
                return 1;
            }
            printf("{\"ok\":true,\"id\":%u,\"merged\":%d,\"text\":\"serve record %u\"}\n",
                   r.id, r.merged, i);
        }
        free(v);
        return 0;
    }

    if (strcmp(mode, "count") == 0) {
        uint64_t live = 0;
        int rc = sfb_broker_count(sock, &live);
        if (rc != SFB_OK) {
            printf("{\"ok\":false,\"error\":\"sfb_broker_count\",\"rc\":%d,\"name\":\"%s\"}\n",
                   rc, sfb_strerror(rc));
            return 1;
        }
        printf("{\"ok\":true,\"count\":%llu}\n", (unsigned long long)live);
        return 0;
    }

    if (strcmp(mode, "flush") == 0) {
        uint64_t written = 0;
        int rc = sfb_broker_flush(sock, &written);
        if (rc != SFB_OK) {
            printf("{\"ok\":false,\"error\":\"sfb_broker_flush\",\"rc\":%d,\"name\":\"%s\"}\n",
                   rc, sfb_strerror(rc));
            return 1;
        }
        printf("{\"ok\":true,\"written\":%llu}\n", (unsigned long long)written);
        return 0;
    }

    fprintf(stderr, "unknown mode %s\n", mode);
    return 2;
}
EOF

CC_BIN="${CC:-cc}"
CFLAGS="-I. -I../../include"
LIBS="-L. -lsfb -L../.. -lqtsq -lz -lpthread -lm"
if [ "$(uname -s)" = "Darwin" ]; then
    LIBS="$LIBS -lc++ -framework CoreFoundation -framework Security"
fi
if ! "$CC_BIN" $CFLAGS "$TMP/serve_client.c" $LIBS -o "$TMP/serve_client" \
        2>"$TMP/serve_client.cc.log"; then
    echo "helper build failed:"
    cat "$TMP/serve_client.cc.log"
    exit 2
fi
CLIENT="$TMP/serve_client"

# ── helpers ───────────────────────────────────────────────────────────────
json_num() { sed -n "s/.*\"$2\":\([0-9][0-9]*\).*/\1/p" "$1" | tail -1; }
json_str() { sed -n "s/.*\"$2\":\"\([^\"]*\)\".*/\1/p" "$1" | tail -1; }
lines_of() { wc -l <"$1" | tr -d ' '; }

# Bounded wait for the ready line; fails early if the server died.
wait_ready() { # $1 pid, $2 stdout file
    local i
    for i in $(seq 1 100); do
        if grep -q '"ok":true' "$2" 2>/dev/null; then return 0; fi
        kill -0 "$1" 2>/dev/null || return 1
        sleep 0.1
    done
    return 1
}

# Bounded wait for a process to exit without reaping it.
wait_exit() { # $1 pid
    local i
    for i in $(seq 1 100); do
        kill -0 "$1" 2>/dev/null || return 0
        sleep 0.1
    done
    return 1
}

start_server() { # $1 out, $2 err, rest: args
    local out=$1 err=$2
    shift 2
    "$BIN" serve "$@" >"$out" 2>"$err" &
    SERVER_PID=$!
    SPIDS+=("$SERVER_PID")
}

# ── 1. existing brain, default <brain-dir>/brain.sock ────────────────────
say "1. serve an existing brain on the default <brain-dir>/brain.sock"
BRAIN="$TMP/brain"
SOCK="$BRAIN/brain.sock"
if "$WORKER" ingest "$BRAIN" 64 8 4 >"$TMP/seed.json" 2>&1 &&
   grep -q '"done":true' "$TMP/seed.json"; then
    ok "seeded an existing brain (chaos_worker ingest, 8 records, dim 64)"
else
    bad "seed failed: $(head -1 "$TMP/seed.json")"
fi

start_server "$TMP/s1.out" "$TMP/s1.err" "$BRAIN"
S1=$SERVER_PID
if wait_ready "$S1" "$TMP/s1.out"; then
    ok "ready JSON on stdout: $(head -1 "$TMP/s1.out")"
else
    bad "no ready JSON within 10s; stderr: $(head -2 "$TMP/s1.err" 2>/dev/null)"
fi

if [ "$(lines_of "$TMP/s1.out")" = "1" ] && grep -q '"ok":true' "$TMP/s1.out"; then
    ok "exactly one JSON line on stdout"
else
    bad "stdout is not exactly one ready line: $(cat "$TMP/s1.out" 2>/dev/null)"
fi
served="$(json_str "$TMP/s1.out" serving)"
if [ "$served" = "$SOCK" ]; then
    ok "serving path is the default socket ($SOCK)"
else
    bad "serving=\"$served\", want \"$SOCK\""
fi
rpid="$(json_num "$TMP/s1.out" pid)"
if [ "$rpid" = "$S1" ]; then
    ok "ready JSON pid matches the server ($S1)"
else
    bad "ready pid=$rpid, server pid=$S1"
fi
if [ -S "$SOCK" ]; then ok "socket file exists"; else bad "socket file missing ($SOCK)"; fi

# baseline count through the socket (the 8 seeded records)
"$CLIENT" count "$SOCK" >"$TMP/count0.out" 2>&1
BASE="$(json_num "$TMP/count0.out" count)"
if [ -n "$BASE" ] && [ "$BASE" -ge 8 ]; then
    ok "count over the socket: $BASE (seeded 8)"
else
    bad "baseline count over the socket: $(cat "$TMP/count0.out")"
    BASE=0
fi

# 3 records through the socket
"$CLIENT" put "$SOCK" 64 3 >"$TMP/put.out" 2>&1
put_rc=$?
put_ok="$(grep -c '"ok":true' "$TMP/put.out" 2>/dev/null || true)"
if [ "$put_rc" -eq 0 ] && [ "${put_ok:-0}" -eq 3 ]; then
    ok "3 records remembered through the socket"
else
    bad "put rc=$put_rc ok_lines=${put_ok:-0}: $(head -2 "$TMP/put.out")"
fi

"$CLIENT" count "$SOCK" >"$TMP/count1.out" 2>&1
C1="$(json_num "$TMP/count1.out" count)"
if [ -n "$C1" ] && [ "$C1" -ge $((BASE + 3)) ]; then
    ok "count after put: $C1 >= $((BASE + 3))"
else
    bad "count after put: ${C1:-?} (want >= $((BASE + 3)))"
fi

"$CLIENT" flush "$SOCK" >"$TMP/flush.out" 2>&1
flush_rc=$?
if [ "$flush_rc" -eq 0 ] && grep -q '"ok":true' "$TMP/flush.out"; then
    ok "flush over the socket: $(cat "$TMP/flush.out")"
else
    bad "flush rc=$flush_rc: $(cat "$TMP/flush.out")"
fi

"$CLIENT" count "$SOCK" >"$TMP/count2.out" 2>&1
C2="$(json_num "$TMP/count2.out" count)"
if [ -n "$C2" ] && [ "$C2" -ge $((BASE + 3)) ]; then
    ok "records still visible after flush: $C2"
else
    bad "count after flush: ${C2:-?} (want >= $((BASE + 3)))"
fi

# ── 2. a second server on the same live socket must be refused ───────────
say "2. second 'sfb serve' on the live socket is refused"
"$BIN" serve "$BRAIN" >"$TMP/s2.out" 2>"$TMP/s2.err" &
S2=$!
SPIDS+=("$S2")
if wait_exit "$S2"; then
    wait "$S2" 2>/dev/null
    s2_rc=$?
    if [ "$s2_rc" -ne 0 ]; then
        ok "second serve exited non-zero ($s2_rc)"
    else
        bad "second serve exited 0 — it should have been refused"
    fi
else
    bad "second serve never exited (it may have stolen the socket)"
    kill -9 "$S2" 2>/dev/null
    wait "$S2" 2>/dev/null
    s2_rc=0
fi
if grep -q '"ok":false' "$TMP/s2.err" &&
   grep -Eqi 'already listening|already serve' "$TMP/s2.err"; then
    ok "clear stderr error: $(head -1 "$TMP/s2.err")"
else
    bad "stderr lacks a clear socket-in-use error: $(cat "$TMP/s2.err" 2>/dev/null)"
fi
if [ ! -s "$TMP/s2.out" ]; then
    ok "second serve printed no ready line"
else
    bad "second serve printed to stdout: $(cat "$TMP/s2.out")"
fi
if kill -0 "$S1" 2>/dev/null; then
    ok "first server still alive after the refused attempt"
else
    bad "first server died when the second was started"
fi

# ── 3. SIGINT: exit 0, socket gone, brain flushed ────────────────────────
say "3. SIGINT: exit 0, socket unlinked, brain flushed"
kill -INT "$S1" 2>/dev/null
if wait_exit "$S1"; then
    wait "$S1" 2>/dev/null
    s1_rc=$?
    if [ "$s1_rc" -eq 0 ]; then
        ok "server exited 0 after SIGINT"
    else
        bad "server exited $s1_rc after SIGINT (want 0)"
    fi
else
    bad "server still running 10s after SIGINT"
    kill -9 "$S1" 2>/dev/null
    wait "$S1" 2>/dev/null
fi
if [ ! -e "$SOCK" ]; then
    ok "socket file unlinked on shutdown"
else
    bad "socket file still present after shutdown ($SOCK)"
fi

# Reopen as a plain writer: the shutdown flush must have persisted the writes.
reopen="$("$WORKER" count "$BRAIN" 64 2>&1)"
reopen_n="$(printf '%s' "$reopen" | sed -n 's/.*"count":\([0-9][0-9]*\).*/\1/p')"
if [ -n "$reopen_n" ] && [ "$reopen_n" -ge $((BASE + 3)) ]; then
    ok "reopened brain has $reopen_n records (>= $((BASE + 3))): shutdown flush persisted them"
else
    bad "reopened brain count ${reopen_n:-?} < $((BASE + 3)): $reopen"
fi

# ── 4. --socket and --dim on a brand-new brain ───────────────────────────
say "4. --socket <path> --dim <n> creates and serves a fresh brain"
BRAIN2="$TMP/fresh-brain"
SOCK2="$TMP/fresh.sock"
start_server "$TMP/s3.out" "$TMP/s3.err" "$BRAIN2" --socket "$SOCK2" --dim 64
S3=$SERVER_PID
if wait_ready "$S3" "$TMP/s3.out"; then
    if [ "$(json_str "$TMP/s3.out" serving)" = "$SOCK2" ]; then
        ok "--socket honoured: $(head -1 "$TMP/s3.out")"
    else
        bad "serving=\"$(json_str "$TMP/s3.out" serving)\", want \"$SOCK2\""
    fi
else
    bad "fresh brain never became ready: $(head -2 "$TMP/s3.err" 2>/dev/null)"
fi
"$CLIENT" count "$SOCK2" >"$TMP/fresh0.out" 2>&1
if [ "$(json_num "$TMP/fresh0.out" count)" = "0" ]; then
    ok "fresh brain starts empty (count 0)"
else
    bad "fresh brain count: $(cat "$TMP/fresh0.out")"
fi
"$CLIENT" put "$SOCK2" 64 3 >"$TMP/fresh_put.out" 2>&1
"$CLIENT" count "$SOCK2" >"$TMP/fresh1.out" 2>&1
if [ "$(json_num "$TMP/fresh1.out" count)" = "3" ]; then
    ok "3 records written to the fresh brain over --socket"
else
    bad "fresh brain count after put: $(cat "$TMP/fresh1.out")"
fi
kill -INT "$S3" 2>/dev/null
if wait_exit "$S3"; then
    wait "$S3" 2>/dev/null
    s3_rc=$?
    [ "$s3_rc" -eq 0 ] && ok "fresh-brain server exited 0" \
                       || bad "fresh-brain server exited $s3_rc"
else
    bad "fresh-brain server did not stop"
    kill -9 "$S3" 2>/dev/null
    wait "$S3" 2>/dev/null
fi
if [ ! -e "$SOCK2" ]; then
    ok "custom socket unlinked on shutdown"
else
    bad "custom socket still present ($SOCK2)"
fi
reopen2="$("$WORKER" count "$BRAIN2" 64 2>&1)"
reopen2_n="$(printf '%s' "$reopen2" | sed -n 's/.*"count":\([0-9][0-9]*\).*/\1/p')"
if [ "$reopen2_n" = "3" ]; then
    ok "fresh brain persisted 3 records"
else
    bad "fresh brain reopen count ${reopen2_n:-?}: $reopen2"
fi

# ── 5. a stale socket (no listener) is reclaimed ─────────────────────────
say "5. stale socket file (bound once, no listener) is unlinked and reused"
BRAIN3="$TMP/stale-brain"
SOCK3="$TMP/stale.sock"
if "$CLIENT" stale "$SOCK3" >"$TMP/stale.out" 2>&1 && [ -S "$SOCK3" ]; then
    ok "planted a stale socket at $SOCK3"
else
    skip "could not plant a stale socket: $(cat "$TMP/stale.out" 2>/dev/null)"
fi
if [ -S "$SOCK3" ]; then
    start_server "$TMP/s4.out" "$TMP/s4.err" "$BRAIN3" --socket "$SOCK3" --dim 64
    S4=$SERVER_PID
    if wait_ready "$S4" "$TMP/s4.out"; then
        ok "serve reclaimed the stale socket: $(head -1 "$TMP/s4.out")"
    else
        bad "serve refused the stale socket: $(head -2 "$TMP/s4.err" 2>/dev/null)"
    fi
    kill -INT "$S4" 2>/dev/null
    if wait_exit "$S4"; then
        wait "$S4" 2>/dev/null
        s4_rc=$?
        if [ "$s4_rc" -eq 0 ]; then
            ok "stale-socket server exited 0"
        else
            bad "stale-socket server exited $s4_rc"
        fi
    else
        bad "stale-socket server did not stop"
        kill -9 "$S4" 2>/dev/null
        wait "$S4" 2>/dev/null
    fi
fi

printf '\n=============================================\n'
printf 'serve: %d passed, %d failed, %d skipped\n' "$PASS" "$FAIL" "$SKIP"
[ "$FAIL" -eq 0 ]
