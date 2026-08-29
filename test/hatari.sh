# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Neil Rackett
#
# Booting an ST program under Hatari, for the end-to-end runners.
# Sourced, not run, the same way test/tos.sh is.
#
# This exists because the four runners were ~85% the same script, and
# the two load-bearing Hatari findings below were maintained in four
# separate command lines. A shallow copy does not propagate a fix: the
# gamepad runner grew per-check reporting while the other three still
# said only "FAILED", which is the tell.
#
# Provides: hatari_require, hatari_workdir, hatari_boot, hatari_wait,
# hatari_expect and hatari_verdict. Callers must already be at the repo
# root and have sourced tos.sh.

HATARI=${HATARI:-/Applications/Hatari.app/Contents/MacOS/Hatari}
MACHINE=${MACHINE:-st}
BUILD=target/atarist/build

# Every binary the caller needs, plus Hatari itself.
hatari_require()
{
    local f
    for f in "$@"; do
        if [ ! -f "$BUILD/$f" ]; then
            echo "build it first: STCMD_NO_TTY=1 stcmd make st"
            return 1
        fi
    done
    if [ ! -x "$HATARI" ]; then
        echo "Hatari not found; set \$HATARI"
        return 1
    fi
    return 0
}

# A drive C with an AUTO folder and a FIFO for the serial link. Sets
# WORK, FIFO and LOG, and arranges cleanup on exit.
# Pass --no-serial for a program that has no sender: opening a FIFO for
# reading blocks until a writer appears, so handing Hatari one that
# nobody will ever write to wedges it before the ST boots.
hatari_workdir()
{
    WORK=$(mktemp -d) || return 1
    LOG=$WORK/console.log
    mkdir "$WORK/AUTO" || return 1
    HATARI_PIDS=""

    if [ "${1:-}" = "--no-serial" ]; then
        FIFO=
    else
        FIFO=$WORK/serial.in
        mkfifo "$FIFO" || return 1
    fi

    trap hatari_cleanup EXIT
    return 0
}

# Kill everything we started and take the work directory with it. No
# grace period: these are throwaway emulator and harness processes with
# nothing to flush, and the verdict is already in the log by now.
hatari_cleanup()
{
    [ -n "${HATARI_PIDS:-}" ] && kill -9 $HATARI_PIDS 2>/dev/null
    [ -n "${WORK:-}" ] && rm -rf "$WORK"
    return 0
}

# Remember a process so cleanup takes it down.
hatari_own()
{
    HATARI_PIDS="${HATARI_PIDS:-} $1"
    disown "$1" 2>/dev/null
}

# Boot $WORK as drive C and auto-run one program from it. Start any
# sender first: it polls the FIFO until a reader appears, so having it
# already up means data flows the moment the ST opens the port, rather
# than the ST's own timeout racing the harness's startup.
#
# Two findings are baked in here rather than repeated per runner:
# --rs232-out is mandatory even when only receiving, because Hatari
# defaults it to /dev/modem and one failed open disables RS-232
# silently; and Hatari's Timer-D patch must stay on, because with it
# off reception under EmuTOS stalls after a single byte.
#
# Deliberately never --fast-forward. Every sender in this repo writes
# on a real-time clock, so an emulation running as fast as the host
# allows reaches the program's verdict before the data does. Making
# that an invariant rather than a per-script flag means nobody has to
# remember it.
hatari_boot()
{
    local prog=$1
    local serial=""

    [ -n "${FIFO:-}" ] && \
        serial="--rs232-in $FIFO --rs232-out $WORK/serial.out"

    SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
    "$HATARI" --tos "$TOS" --machine "$MACHINE" --memsize 4 \
        --fast-boot on --sound off --statusbar off \
        --timer-d true $serial \
        --conout 2 "$WORK/$prog" > "$LOG" 2>/dev/null &
    hatari_own $!
}

# Wait for a program to print its verdict token, or give up.
hatari_wait()
{
    local marker=$1 timeout=${2:-90} i
    echo "waiting for the verdict (up to ${timeout}s)..."
    for i in $(seq 1 $((timeout * 10))); do
        grep -q "$marker" "$LOG" 2>/dev/null && return 0
        sleep 0.1
    done
    return 1
}

# Per-check reporting, so a failure names the assertion that failed
# rather than just the run. Sets HATARI_FAILED.
HATARI_FAILED=0
hatari_expect()
{
    if grep -q "$1" "$LOG"; then
        echo "   ok: $2"
    else
        echo "   MISSING: $2   (wanted /$1/)"
        HATARI_FAILED=1
    fi
}

# Print the ST's output, then the verdict.
hatari_verdict()
{
    local pass=$1 fail=$2
    echo "--- ST console ---"
    # Hatari's own INFO/WARN chatter goes to the same log; everything
    # else on it came from the ST.
    grep -avE '^(INFO|WARN|ERROR) *:' "$LOG" | grep -av '^$' | tail -14

    if [ "$HATARI_FAILED" -eq 0 ]; then
        echo "--- $pass ---"
        return 0
    fi
    echo "--- $fail ---"
    return 1
}
