#!/usr/bin/env bash
# Boot the game on the current save with mock VR, close a stray menu, click through a starting
# scene, then run the named validation scripts (tools/validate/<name>.py), printing their results.
# Usage: bash tools/validate/run_suite.sh leash_toggle leash_shake ...
cd "$(dirname "$0")/../.."
L="/d/Games/Black & White/bwvr/logs/bwvr.log"
# Never on a game that is already running: it may be the player's own session (2026-10-25: a test
# attached to the player's game, drove it and then closed it).
if tasklist 2>/dev/null | grep -qi runblack.exe; then
  echo "FAIL  the game is already running (the player's session?): close it first; nothing was tested"
  exit 1
fi
timeout 150 python tools/harness/bwctl.py boot >/dev/null 2>&1
python tools/harness/bwctl.py stereo "enabled 1;vr mock;base level;freeze 0;hands near 1;hands always 1;hands laser 0;pose 0 0 0 0 0 0;ctrl r pose 0 0 0 0 -0.3 0.5" >/dev/null
sleep 6
for i in 1 2; do python tools/harness/bwctl.py stereo "ctrl r trigger 1" >/dev/null; sleep 0.4; python tools/harness/bwctl.py stereo "ctrl r trigger 0" >/dev/null; sleep 2.5; done
# A menu left open (the boot's Esc can open one when the save loads straight into play): close it.
for i in 1 2; do
  python tools/harness/bwctl.py stereo "ui screeninfo" >/dev/null; sleep 0.8
  if grep "ui: screen 1920" "$L" | tail -1 | grep -q "box (1000000000"; then break; fi
  echo "(a menu is open: Esc)"; python tools/harness/bwctl.py input "key esc" >/dev/null; sleep 2
done
cd tools/validate
for t in "$@"; do
  echo "=== $t"
  timeout 900 python $t.py 2>&1 | grep -E "PASS|FAIL|SKIP|Error|error"
done
