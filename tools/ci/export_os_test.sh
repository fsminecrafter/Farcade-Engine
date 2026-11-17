#!/usr/bin/env bash
# QEMU test of the export: the editor on Minimal-OS exports the demo project
# (tests/data/export_demo, staged flat into 0:/programs next to the editor
# and the prebuilt player) with its command line form, then runs the exported
# player on the package, as a separate process.
#
#   MINIMAL_OS_DIR=/path/to/minimal-os tools/ci/export_os_test.sh
#
# Needs `make os` first (build/os/bin/farcade-editor.run and feplayer.run).
# Passes when the player prints "FE PLAYER DONE frames=20 ... scripts=1" and the
# serial log has the lines the demo script printed.
set -euo pipefail
cd "$(dirname "$0")/../.."

BIN=build/os/bin
DEMO=tests/data/export_demo
[ -f $BIN/farcade-editor.run ] && [ -f $BIN/feplayer.run ] || { echo "run make os first"; exit 2; }

ARGS="--export 0:/programs 0:/games/ExportDemo --entry-only --run 20" \
    tools/ci/run_os.sh $BIN/farcade-editor.run \
    'FE PLAYER DONE frames=20 .*scripts=1' \
    'FE EXPORT FAIL|FE PLAYER FAIL|FE EXPORT game exited [1-9]|^error:' \
    $BIN/feplayer.run $DEMO/main.fscene $DEMO/spin.lua $DEMO/cube.mof $DEMO/project.fe

LOG=${LOG_DIR:-build/os/run_os}/farcade-editor.serial.log
for want in 'FE EXPORT OK 0:/games/ExportDemo/Export_Demo.fepak files=4' 'FE GAME start Spinner' 'FE GAME spin 15' 'FE PLAYER READY Export Demo'; do
    grep -aqF -- "$want" "$LOG" || { echo "export_os_test: missing in the serial log: $want"; exit 1; }
done
echo "export_os_test: PASS"
