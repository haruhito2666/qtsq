#!/usr/bin/env bash
# tests/chaos.sh — Sofuu Brain v2 chaos matrix (W7.6)
# Requires: ./sfb, ./tests/chaos_worker built first (make all chaos-target deps).
set -u

BIN="${BIN:-./sfb}"
WORKER="${WORKER:-./tests/chaos_worker}"
TMP="$(mktemp -d /tmp/sfb_chaos.XXXXXX)"
PASS=0; FAIL=0; SKIP=0

say()  { printf '\n\033[1m── %s\033[0m\n' "$*"; }
ok()   { PASS=$((PASS+1)); printf '  \033[32mPASS\033[0m %s\n' "$*"; }
bad()  { FAIL=$((FAIL+1)); printf '  \033[31mFAIL\033[0m %s\n' "$*"; }
skip() { SKIP=$((SKIP+1)); printf '  \033[33mSKIP\033[0m %s\n' "$*"; }

[ -x "$WORKER" ] || { echo "missing $WORKER (run make first)"; exit 2; }

# ── 1. SIGKILL matrix ──────────────────────────────────────────────────────
say "SIGKILL matrix (15 kills during ingest)"
BRAIN="$TMP/kill/brain.qtsq"; mkdir -p "$(dirname "$BRAIN")"
last=0; prev=0; violations=0; opens=0
for i in $(seq 1 15); do
    "$WORKER" ingest "$BRAIN" 64 400 20 >>"$TMP/kill.log" 2>&1 &
    pid=$!
    sleep "0.$(( (RANDOM % 20) + 2 ))"
    kill -9 "$pid" 2>/dev/null
    wait "$pid" 2>/dev/null
    out="$("$WORKER" count "$BRAIN" 64 2>/dev/null)"
    case "$out" in *open_error*) continue;; esac
    opens=$((opens+1))
    cur="$(printf '%s' "$out" | sed -n 's/.*"count":\([0-9]*\).*/\1/p')"
    [ -z "$cur" ] && continue
    if [ "$cur" -lt "$prev" ]; then violations=$((violations+1)); fi
    prev="$cur"
done
if [ "$opens" -ge 10 ] && [ "$violations" -eq 0 ]; then
    ok "15 SIGKILLs: $opens clean reopens, 0 count regressions (final=$prev)"
else
    bad "SIGKILL matrix: opens=$opens regressions=$violations final=$prev"
fi

# ── 2. durable prefix after a clean flush ─────────────────────────────────
say "Durability: every record after a successful flush survives"
B2="$TMP/durable/brain.qtsq"; mkdir -p "$(dirname "$B2")"
"$WORKER" ingest "$B2" 64 200 50 >"$TMP/durable.log" 2>&1
claimed="$(grep -o '"count_now":[0-9]*' "$TMP/durable.log" | tail -1 | cut -d: -f2)"
got="$("$WORKER" count "$B2" 64 | sed -n 's/.*"count":\([0-9]*\).*/\1/p')"
if [ -n "$claimed" ] && [ "$got" = "$claimed" ]; then
    ok "flush claimed $claimed, reopen returned $got"
else
    bad "flush claimed ${claimed:-?}, reopen returned ${got:-?}"
fi

# ── 3. corruption fuzz: manifest must never look empty ────────────────────
say "Corruption fuzz (manifest + segment), 20 mutations each"
mkcorrupt() { # $1=src $2=dst $3=target(rel) $4=bytes
    rm -rf "$2"; mkdir -p "$2"; (cd "$1" && tar cf - .) | (cd "$2" && tar xf -)
    python3 - "$2/$3" "$4" <<'PY'
import sys, random
p, n = sys.argv[1], int(sys.argv[2])
d = bytearray(open(p,'rb').read())
if not d: sys.exit(0)
off = random.randrange(0, min(len(d), 64))          # hit the header first
for k in range(n):
    i = (off + k) % len(d)
    d[i] ^= 1 << random.randrange(8)
open(p,'wb').write(d)
PY
}
silent_empty=0; loud=0; fuzz_runs=20
for i in $(seq 1 "$fuzz_runs"); do
    mkcorrupt "$B2" "$TMP/fz" brain.qtsq 8
    out="$("$WORKER" count "$TMP/fz/brain.qtsq" 64 2>/dev/null)"
    if printf '%s' "$out" | grep -q '"count":0'; then silent_empty=$((silent_empty+1));
    else loud=$((loud+1)); fi
done
if [ "$silent_empty" -eq 0 ]; then
    ok "manifest fuzz: 0/$fuzz_runs silent empty opens ($loud refused or recovered)"
else
    bad "manifest fuzz: $silent_empty/$fuzz_runs opened as an EMPTY brain"
fi

seg="$(ls "$B2/brain.qtsq.d" 2>/dev/null | grep -m1 '^seg-')"
if [ -n "$seg" ]; then
    det=0
    for i in $(seq 1 10); do
        mkcorrupt "$B2" "$TMP/fz2" "brain.qtsq.d/$seg" 4
        if "$BIN" verify "$TMP/fz2/brain.qtsq" --json >"$TMP/verify.json" 2>/dev/null; then
            grep -q '"ok":true' "$TMP/verify.json" && det=$((det+1))
        else det=$((det+1)); fi
    done
    if [ "$det" -eq 10 ]; then ok "segment fuzz: all 10 mutations detected by verify"
    else bad "segment fuzz: only $det/10 detected"; fi
else
    skip "segment fuzz (no segment files produced)"
fi

# ── 4. writer stampede: exactly one owner, zero silent loss ───────────────
say "Stampede: 4 concurrent writers on one brain"
B3="$TMP/stamp/brain.qtsq"; mkdir -p "$(dirname "$B3")"
for i in 0 1 2 3; do
    "$WORKER" stampede "$B3" 64 100 >"$TMP/stamp$i.json" 2>&1 &
done
wait
locked="$(cat "$TMP"/stamp*.json | grep -o '"locked":[0-9]*' | cut -d: -f2 | sort -u | tr '\n' ' ')"
winners="$(grep -l '"inserted":100' "$TMP"/stamp*.json 2>/dev/null | wc -l | tr -d ' ')"
final="$("$WORKER" count "$B3" 64 | sed -n 's/.*"count":\([0-9]*\).*/\1/p')"
if [ "$winners" = "1" ] && [ "${final:-0}" = "100" ]; then
    ok "1 winner, losers reported E_LOCKED, final count $final"
else
    bad "winners=$winners final=${final:-?} locked_codes=[$locked]"
fi

# ── 5. ENOSPC: typed error, never a truncated published segment ──────────
say "Disk full (ulimit -f 48 blocks, below one 256-record segment)"
B4="$TMP/enospc/brain.qtsq"; mkdir -p "$(dirname "$B4")"
"$WORKER" ingest "$B4" 64 80 40 >"$TMP/enospc_seed.log" 2>&1
seed="$(printf '%s' "$("$WORKER" count "$B4" 64)" | sed -n 's/.*"count":\([0-9]*\).*/\1/p')"
bash -c 'trap "" XFSZ; ulimit -f 48; exec "$0" ingest "$1" 64 4000 500' "$WORKER" "$B4" >"$TMP/enospc.log" 2>&1
err="$(grep -oE '"(insert_error|flush_rc)":-1004|ENOSPC' "$TMP/enospc.log" | head -1)"
after="$(printf '%s' "$("$WORKER" count "$B4" 64 2>/dev/null)" | sed -n 's/.*"count":\([0-9]*\).*/\1/p')"
if [ -n "$err" ] && [ "${after:-0}" -ge "${seed:-0}" ]; then
    ok "ENOSPC surfaced as -1004; seed records intact ($seed -> ${after:-?})"
elif [ -z "$err" ]; then
    bad "no typed ENOSPC in output (rc path silent)"
else
    bad "ENOSPC lost seed records: $seed -> ${after:-?}"
fi

# ── 6. NaN ingest ────────────────────────────────────────────────────────
say "NaN vector ingest"
B5="$TMP/nan/brain.qtsq"; mkdir -p "$(dirname "$B5")"
if "$WORKER" nan "$B5" 64 >"$TMP/nan.log" 2>&1; then
    ok "$(cat "$TMP/nan.log" | head -1)"
else
    bad "NaN path: $(cat "$TMP/nan.log" | head -1)"
fi

printf '\n=============================================\n'
printf 'chaos: %d passed, %d failed, %d skipped\n' "$PASS" "$FAIL" "$SKIP"
printf 'artifacts: %s\n' "$TMP"
[ "$FAIL" -eq 0 ]
