#!/usr/bin/env bash
# Runs a Minimal-OS program (.run) in QEMU and waits for its verdict.
#
#   tools/ci/run_os.sh <program.run> <pass-regex> [fail-regex] [extra files...]
#
# The program and the extra files are copied into the Minimal-OS tree's
# src/resources/install2/ (packed into the boot image and installed as
# 0:/programs/<name>), the ISO is built there (plain RamFS build), QEMU boots
# it headless (GRUB entry "Minimal-OS (RamFS)"), and
#     run 0:/programs/<name>.run <ARGS>
# is typed on the serial line, like Minimal-OS's tools/debug/ci_os0.sh does.
# The script then waits for a serial line matching pass-regex (exit 0) or
# fail-regex (exit 1). A CPU exception or kernel panic, QEMU dying, or the
# timeout end it with exit 2. The staged files are removed again afterwards.
#
# Environment:
#   MINIMAL_OS_DIR  Minimal-OS checkout to build in (default /home/user/wt-mos-rt)
#   ARGS            arguments for the program
#   QEMU_EXTRA      extra QEMU arguments, e.g. "-cpu max -smp 4" (default CPU: qemu64, 2 CPUs)
#   QEMU_SMP        number of CPUs (default 2; QEMU_EXTRA can override with -smp)
#   QEMU_MEM        memory (default 1024M)
#   TIMEOUT         seconds to wait for the verdict after starting the program (default 900)
#   SCREENDUMP      PNG file: take a screenshot when the verdict line appears
#                   (SCREENDUMP_DELAY seconds later, default 2) before QEMU is stopped
#   MONITOR_CMDS    QEMU monitor commands sent before the screenshot, separated by ';'
#                   (e.g. "mouse_move 40 20;mouse_button 1;mouse_button 0")
#   USB_MOUSE       1 (default): add a USB mouse next to the USB keyboard
#   GRUB_UP         'up' presses after selecting the last GRUB entry (see ci_os0.sh)
#   NO_BUILD        1: use the ISO that is there (the files must already be in it)
#   LOG_DIR         where the serial and QEMU monitor logs go (default ./build/os/run_os)
set -euo pipefail

usage() { sed -n '2,30p' "$0" | sed 's/^# \{0,1\}//'; exit 2; }
[ $# -ge 2 ] || usage

PROGRAM=$1; PASS_RE=$2; FAIL_RE=${3:-}
shift 2; [ $# -gt 0 ] && shift
EXTRA_FILES=("$@")

MINIMAL_OS_DIR=${MINIMAL_OS_DIR:-/home/user/wt-mos-rt}
QEMU_BIN=${QEMU_BIN:-qemu-system-x86_64}
QEMU_EXTRA=${QEMU_EXTRA:-}
QEMU_SMP=${QEMU_SMP:-2}
TIMEOUT=${TIMEOUT:-900}
BOOT_TIMEOUT=${BOOT_TIMEOUT:-240}
GRUB_UP=${GRUB_UP:-0}
USB_MOUSE=${USB_MOUSE:-1}
SCREENDUMP=${SCREENDUMP:-}
SCREENDUMP_DELAY=${SCREENDUMP_DELAY:-2}
LOG_DIR=${LOG_DIR:-$PWD/build/os/run_os}

[ -f "$PROGRAM" ] || { echo "[run_os] no such program: $PROGRAM"; exit 2; }
[ -d "$MINIMAL_OS_DIR/src" ] || { echo "[run_os] MINIMAL_OS_DIR=$MINIMAL_OS_DIR is not a Minimal-OS tree"; exit 2; }
for f in "${EXTRA_FILES[@]}"; do [ -f "$f" ] || { echo "[run_os] no such file: $f"; exit 2; }; done

NAME=$(basename "$PROGRAM")
STAGE_DIR="$MINIMAL_OS_DIR/src/resources/install2"
ISO="$MINIMAL_OS_DIR/dist/x86_64/kernel.iso"
mkdir -p "$LOG_DIR"
SERIAL="$LOG_DIR/${NAME%.run}.serial.log"
BUILD_LOG="$LOG_DIR/minimal-os-build.log"
TEST_CMD="run 0:/programs/$NAME${ARGS:+ $ARGS}"

# One run per Minimal-OS tree at a time (the lock lives outside the tree).
LOCK="${TMPDIR:-/tmp}/run_os.$(printf '%s' "$MINIMAL_OS_DIR" | cksum | cut -d' ' -f1).lock"
exec 9>"$LOCK"
if ! flock -w 3600 9; then echo "[run_os] could not lock $LOCK"; exit 2; fi

TMP=$(mktemp -d)
QEMU_PID=""
BG_PIDS=""
STAGED=()
cleanup() {
    if [ -n "$QEMU_PID" ]; then kill "$QEMU_PID" 2>/dev/null || true; wait "$QEMU_PID" 2>/dev/null || true; fi
    for p in $BG_PIDS; do kill "$p" 2>/dev/null || true; done
    for f in "${STAGED[@]}"; do rm -f "$f"; done
    rm -rf "$TMP"
    if command -v git >/dev/null && git -C "$MINIMAL_OS_DIR" rev-parse >/dev/null 2>&1; then
        local dirty
        dirty=$(git -C "$MINIMAL_OS_DIR" status --short --untracked-files=all -- src 2>/dev/null || true)
        [ -z "$dirty" ] || echo "[run_os] warning: $MINIMAL_OS_DIR/src is not clean:"$'\n'"$dirty"
    fi
}
trap cleanup EXIT
trap 'exit 2' INT TERM

# ---- stage and build
if [ -z "${NO_BUILD:-}" ]; then
    mkdir -p "$STAGE_DIR"
    for f in "$PROGRAM" "${EXTRA_FILES[@]}"; do
        dst="$STAGE_DIR/$(basename "$f")"
        if [ -e "$dst" ]; then
            echo "[run_os] $dst exists already (a file of the Minimal-OS build, or left over); not overwriting it"
            exit 2
        fi
        cp "$f" "$dst"
        STAGED+=("$dst")
    done
    echo "[run_os] staged ${#STAGED[@]} file(s) in $STAGE_DIR, building the ISO (log: $BUILD_LOG)"
    t0=$(date +%s)
    if ! make -C "$MINIMAL_OS_DIR" -j4 build-x86_64 >"$BUILD_LOG" 2>&1; then
        echo "[run_os] FAIL: the Minimal-OS build failed:"; tail -n 40 "$BUILD_LOG"; exit 2
    fi
    echo "[run_os] ISO built in $(( $(date +%s) - t0 )) s"
fi
[ -f "$ISO" ] || { echo "[run_os] no ISO at $ISO"; exit 2; }

# ---- boot
mkfifo "$TMP/ser.in" "$TMP/ser.out" "$TMP/mon.in" "$TMP/mon.out"
rm -f "$SERIAL"
cat "$TMP/ser.out" >/dev/null & BG_PIDS="$BG_PIDS $!"
MONITOR_LOG="$LOG_DIR/${NAME%.run}.monitor.log"
cat "$TMP/mon.out" > "$MONITOR_LOG" & BG_PIDS="$BG_PIDS $!"

ACCEL=()
if [ -w /dev/kvm ]; then ACCEL=(-accel kvm); echo "[run_os] KVM"; else echo "[run_os] TCG (no /dev/kvm)"; fi
MOUSE=()
# Explicit ports, as Minimal-OS's 'make run' does: without them QEMU puts the
# second device behind a USB hub, which the kernel does not drive.
[ "$USB_MOUSE" = 1 ] && MOUSE=(-device usb-mouse,bus=uhci.0,port=2)

echo "[run_os] QEMU: -smp $QEMU_SMP -m ${QEMU_MEM:-1024M} ${QEMU_EXTRA:-} ; serial log $SERIAL"
# shellcheck disable=SC2086
"$QEMU_BIN" "${ACCEL[@]}" \
    -smp "$QEMU_SMP" -m "${QEMU_MEM:-1024M}" \
    -cdrom "$ISO" -boot d -display none \
    -chardev "pipe,id=ser0,path=$TMP/ser,logfile=$SERIAL" -serial chardev:ser0 \
    -chardev "pipe,id=mon0,path=$TMP/mon" -mon chardev=mon0,mode=readline \
    -device piix3-usb-uhci,id=uhci -device usb-kbd,bus=uhci.0,port=1 "${MOUSE[@]}" \
    -no-reboot -no-shutdown $QEMU_EXTRA &
QEMU_PID=$!

qemu_alive() { kill -0 "$QEMU_PID" 2>/dev/null; }
monitor_cmd() { printf '%s\n' "$1" > "$TMP/mon.in"; }
serial_type() {
    local line="$1" i
    for ((i = 0; i < ${#line}; i++)); do printf '%s' "${line:$i:1}" > "$TMP/ser.in"; sleep 0.05; done
    printf '\r' > "$TMP/ser.in"
}
fail_tail() { echo "[run_os] last 60 lines of $SERIAL:"; tail -n 60 "$SERIAL" 2>/dev/null || true; }

sleep 2
for _ in 1 2 3 4 5 6 7 8; do monitor_cmd "sendkey down"; sleep 0.4; done
for ((i = 0; i < GRUB_UP; i++)); do monitor_cmd "sendkey up"; sleep 0.4; done
monitor_cmd "sendkey ret"

ready=""
for ((t = 0; t < BOOT_TIMEOUT; t++)); do
    if grep -aqF "=== TERMINAL READY ===" "$SERIAL" 2>/dev/null || grep -aqF "mosWM detected; skipping terminal UI" "$SERIAL" 2>/dev/null; then ready=1; break; fi
    qemu_alive || { echo "[run_os] FAIL: QEMU exited during boot"; fail_tail; exit 2; }
    sleep 1
done
[ -n "$ready" ] || { echo "[run_os] FAIL: terminal not ready within ${BOOT_TIMEOUT}s"; fail_tail; exit 2; }
grep -aqF "RamFS mode" "$SERIAL" || echo "[run_os] warning: no 'RamFS mode' in the boot log (GRUB selection missed?)"
sleep 3

echo "[run_os] typing: $TEST_CMD"
start_line=$(wc -l < "$SERIAL")
serial_type "$TEST_CMD"
seen=""
for ((t = 0; t < 20; t++)); do grep -aqF "Command ready: '$TEST_CMD'" "$SERIAL" && { seen=1; break; }; sleep 1; done
if [ -z "$seen" ]; then echo "[run_os] command echo not seen, typing it again"; serial_type "$TEST_CMD"; fi
T_START=$(date +%s)

# Output of the program: the serial log after the command, without the echo.
program_output() { tail -n +"$((start_line + 1))" "$SERIAL" 2>/dev/null | tr -d '\r' | grep -av -F "$TEST_CMD" || true; }

RESULT=""
for ((t = 0; t < TIMEOUT; t++)); do
    out=$(program_output)
    if printf '%s\n' "$out" | grep -aqE -- "$PASS_RE"; then RESULT=PASS; break; fi
    if [ -n "$FAIL_RE" ] && printf '%s\n' "$out" | grep -aqE -- "$FAIL_RE"; then RESULT=FAIL; break; fi
    if printf '%s\n' "$out" | grep -aqE "!!! PAGE FAULT !!!|UNHANDLED CPU EXCEPTION|KERNEL PANIC|PANIC:"; then RESULT=CRASH; break; fi
    if printf '%s\n' "$out" | grep -aqE "Assertion failed|abort\(\) called"; then RESULT=ABORT; break; fi
    # the program ended without a verdict (its threads end as "<path>#thread@...")
    if printf '%s\n' "$out" | grep -aqF "Exiting process: 0:/programs/$NAME@"; then
        sleep 1; out=$(program_output)
        if printf '%s\n' "$out" | grep -aqE -- "$PASS_RE"; then RESULT=PASS
        elif [ -n "$FAIL_RE" ] && printf '%s\n' "$out" | grep -aqE -- "$FAIL_RE"; then RESULT=FAIL
        else RESULT=EXITED; fi
        break
    fi
    qemu_alive || { RESULT=QEMU; break; }
    sleep 1
done
ELAPSED=$(( $(date +%s) - T_START ))

if [ -n "$SCREENDUMP" ] && qemu_alive; then
    sleep "$SCREENDUMP_DELAY"
    if [ -n "${MONITOR_CMDS:-}" ]; then
        IFS=';' read -r -a cmds <<< "$MONITOR_CMDS"
        for c in "${cmds[@]}"; do echo "[run_os] monitor: $c"; monitor_cmd "$c"; sleep 0.5; done
        sleep "$SCREENDUMP_DELAY"
    fi
    ppm="$TMP/screen.ppm"
    monitor_cmd "screendump $ppm"
    for ((t = 0; t < 30; t++)); do [ -s "$ppm" ] && break; sleep 1; done
    sleep 1
    if [ -s "$ppm" ]; then
        mkdir -p "$(dirname "$SCREENDUMP")"
        python3 -I - "$ppm" "$SCREENDUMP" <<'EOF'
import sys, zlib, struct
src, dst = sys.argv[1], sys.argv[2]
data = open(src, 'rb').read()
parts = data.split(None, 4)
assert parts[0] == b'P6', 'not a P6 PPM'
w, h = int(parts[1]), int(parts[2])
pix = data[len(data) - w * h * 3:]
raw = b''.join(b'\x00' + pix[y * w * 3:(y + 1) * w * 3] for y in range(h))
def chunk(tag, body):
    c = struct.pack('>I', len(body)) + tag + body
    return c + struct.pack('>I', zlib.crc32(tag + body) & 0xffffffff)
png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) \
      + chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b'')
open(dst, 'wb').write(png)
print(f"[run_os] screenshot {w}x{h}: {dst}")
EOF
    else
        echo "[run_os] warning: screendump produced nothing"
    fi
fi

echo "[run_os] ---- program output ($NAME)"
program_output | head -n 300
echo "[run_os] ----"
case "$RESULT" in
    PASS)  echo "[run_os] PASS after ${ELAPSED}s"; exit 0 ;;
    FAIL)  echo "[run_os] FAIL (the program reported failure) after ${ELAPSED}s"; exit 1 ;;
    CRASH) echo "[run_os] FAIL: CPU exception / panic after ${ELAPSED}s"; fail_tail; exit 2 ;;
    ABORT) echo "[run_os] FAIL: the program aborted after ${ELAPSED}s"; exit 2 ;;
    EXITED) echo "[run_os] FAIL: the program ended without a verdict after ${ELAPSED}s"; exit 2 ;;
    QEMU)  echo "[run_os] FAIL: QEMU exited"; fail_tail; exit 2 ;;
    *)     echo "[run_os] FAIL: no verdict within ${TIMEOUT}s"; fail_tail; exit 2 ;;
esac
