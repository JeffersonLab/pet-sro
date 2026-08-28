#!/usr/bin/env bash
#
# docker_smoke_test.sh -- validate a built evio_ejfat_replay image.
#
# Everything here runs with `--network none`, so the container has no route to
# anything: the test cannot emit production traffic even by accident. The only
# executions are the programs' own --help and a --dry-run over a two-frame
# synthetic capture this script generates, which reads and synchronizes but
# sends nothing.
#
# Usage:
#   cpp/tests/docker_smoke_test.sh [IMAGE]
#
# IMAGE defaults to $PETSRO_IMAGE, then to pet-sro/evio-ejfat-replay:latest.
#
# Exit status is 0 only if every check passes.

set -euo pipefail

IMAGE="${1:-${PETSRO_IMAGE:-pet-sro/evio-ejfat-replay:latest}}"

# The image is amd64; on an arm64 host docker needs telling, and emulation
# makes each run take seconds rather than milliseconds.
PLATFORM_ARG=()
if [ -n "${PETSRO_PLATFORM:-}" ]; then
    PLATFORM_ARG=(--platform "${PETSRO_PLATFORM}")
elif [ "$(uname -m)" != "x86_64" ]; then
    PLATFORM_ARG=(--platform linux/amd64)
fi

WORKDIR="$(mktemp -d)"
trap 'rm -rf "$WORKDIR"' EXIT

passes=0
failures=0

pass() { printf '  ok    %s\n' "$1"; passes=$((passes + 1)); }
fail() { printf '  FAIL  %s\n' "$1"; failures=$((failures + 1)); }

# run_ro <args...> -- run the image with the fixture directory mounted
# read-only, no network, no extra privileges.
run_ro() {
    docker run --rm "${PLATFORM_ARG[@]}" \
        --network none \
        --read-only \
        --security-opt no-new-privileges \
        --mount "type=bind,source=$WORKDIR,target=/data,readonly" \
        "$@"
}

# ---------------------------------------------------------------------------
# Fixture: a two-frame WIRE_DUMP capture, built from the layout documented in
# cpp/include/SroWireFormat.hpp -- 18 big-endian words per block, block length
# in word 0, EVIO magic in word 7, timestamp in words 14/15.
# ---------------------------------------------------------------------------
emit_be32() {
    local v=$1 esc
    printf -v esc '\\x%02x\\x%02x\\x%02x\\x%02x' \
        $(( (v >> 24) & 0xFF )) $(( (v >> 16) & 0xFF )) \
        $(( (v >> 8) & 0xFF ))  $(( v & 0xFF ))
    printf '%b' "$esc"
}

# write_capture <path> <rocid>
write_capture() {
    local path=$1 rocid=$2 frame ts w i
    : > "$path"
    for frame in 0 1; do
        ts=$(( frame * 1000000 ))          # 1 ms frame period, in nanoseconds
        for i in $(seq 0 17); do
            case $i in
                0)  w=18 ;;                            # block length, in words
                7)  w=$(( 0xC0DA0100 )) ;;             # EVIO magic
                9)  w=$(( (rocid << 16) | 0xFF10 )) ;; # bank tag, rocid on top
                11) w=$(( 0xFF302011 )) ;;
                12) w=$(( 0x31010003 )) ;;
                13) w=$frame ;;                        # frame counter
                14) w=$(( ts & 0xFFFFFFFF )) ;;        # timestamp, low word
                15) w=$(( (ts >> 32) & 0xFFFFFFFF )) ;;# timestamp, high word
                16) w=$(( 0x41850001 )) ;;
                17) w=$(( 0x00000081 )) ;;
                *)  w=0 ;;
            esac
            emit_be32 "$w" >> "$path"
        done
    done
}

echo "==> smoke testing image: $IMAGE"
docker image inspect "$IMAGE" >/dev/null 2>&1 || {
    echo "  image not found; build it first (see DOCKER.md)" >&2
    exit 1
}

write_capture "$WORKDIR/stream0.bin" 2
write_capture "$WORKDIR/stream1.bin" 3
[ "$(wc -c < "$WORKDIR/stream0.bin")" -eq 144 ] \
    && pass "fixture capture is 2 x 72 bytes" \
    || fail "fixture capture has the wrong size"

# --- 1. The default entrypoint answers --help, exec form, exit 0 ----------
if out=$(run_ro "$IMAGE" --help 2>&1) \
        && grep -q -- '--file-count' <<<"$out" \
        && grep -q -- '--dry-run' <<<"$out"; then
    pass "evio_ejfat_replay --help"
else
    fail "evio_ejfat_replay --help"
    printf '%s\n' "$out" | sed 's/^/        /'
fi

# --- 2. The receiver is present and answers --help too --------------------
if out=$(run_ro --entrypoint /usr/local/bin/evio_ejfat_recv "$IMAGE" --help 2>&1) \
        && grep -q -- '--recv-port' <<<"$out"; then
    pass "evio_ejfat_recv --help"
else
    fail "evio_ejfat_recv --help"
    printf '%s\n' "$out" | sed 's/^/        /'
fi

# --- 3. Both executables exist where the docs say they do -----------------
if run_ro --entrypoint /usr/bin/test "$IMAGE" -x /usr/local/bin/evio_ejfat_replay \
   && run_ro --entrypoint /usr/bin/test "$IMAGE" -x /usr/local/bin/evio_ejfat_recv; then
    pass "executables present at /usr/local/bin"
else
    fail "executables missing from /usr/local/bin"
fi

# --- 4. No unresolved shared libraries ------------------------------------
# ldd exits 0 even with missing libraries, so the output is what is checked.
missing=$(run_ro --entrypoint /usr/bin/ldd "$IMAGE" \
              /usr/local/bin/evio_ejfat_replay 2>&1 | grep -c 'not found' || true)
missing2=$(run_ro --entrypoint /usr/bin/ldd "$IMAGE" \
               /usr/local/bin/evio_ejfat_recv 2>&1 | grep -c 'not found' || true)
if [ "$missing" -eq 0 ] && [ "$missing2" -eq 0 ]; then
    pass "no unresolved shared libraries"
else
    fail "unresolved shared libraries ($missing + $missing2)"
fi

# --- 5. The process runs as a non-root UID --------------------------------
uid=$(run_ro --entrypoint /usr/bin/id "$IMAGE" -u 2>&1 | tr -d '[:space:]' || true)
if [ "$uid" != "0" ] && [ -n "$uid" ]; then
    pass "runs as non-root (uid $uid)"
else
    fail "runs as uid '$uid'"
fi

# --- 6. No toolchain left in the runtime image ----------------------------
# /usr/local/include exists (empty) in the base image, so it is the *contents*
# that matter there; likewise /usr/local/lib, which legitimately holds the
# runtime shared objects but must hold no static libraries or .pc files.
leftovers=$(run_ro --entrypoint /bin/sh "$IMAGE" -c \
    'for f in /usr/bin/gcc /usr/bin/g++ /usr/bin/cc /usr/bin/cmake /usr/bin/make \
              /usr/bin/ld /usr/bin/as /usr/bin/ar /usr/bin/strip /usr/bin/pkg-config \
              /usr/bin/curl /usr/bin/git /src /out; do
        [ -e "$f" ] && echo "$f"
     done
     find /usr/local/include -mindepth 1 -maxdepth 1 2>/dev/null | head -5
     find /usr/local -name "*.a" -o -name "*.pc" -o -name "*.h" -o -name "*.hpp" \
        2>/dev/null | head -5
     exit 0' 2>&1 | tr -d '\r' || true)
if [ -z "$leftovers" ]; then
    pass "no compilers, headers or sources in the runtime image"
else
    fail "build-time leftovers in the runtime image:"
    printf '%s\n' "$leftovers" | sed 's/^/        /'
fi

# --- 7. A real run: read, synchronize and account for packets, send nothing
if out=$(run_ro "$IMAGE" \
            --file-count 2 --dry-run --loop-limit 1 --stats-interval 0 \
            stream0.bin stream1.bin 2>&1); then
    if grep -qE 'EVIO events sent[[:space:]]*:[[:space:]]*4' <<<"$out"; then
        pass "--dry-run replayed 2 synchronized groups (4 events), no traffic"
    else
        fail "--dry-run produced unexpected accounting"
        printf '%s\n' "$out" | sed 's/^/        /'
    fi
else
    fail "--dry-run exited non-zero"
    printf '%s\n' "$out" | sed 's/^/        /'
fi

# --- 8. Bad arguments are rejected, not ignored ---------------------------
set +e
run_ro "$IMAGE" --file-count 2 --dry-run stream0.bin >/dev/null 2>&1
rc=$?
set -e
if [ "$rc" -eq 2 ]; then
    pass "argument validation rejects a file-count mismatch (exit 2)"
else
    fail "expected exit 2 for a file-count mismatch, got $rc"
fi

echo
echo "==> $passes passed, $failures failed"
[ "$failures" -eq 0 ]
