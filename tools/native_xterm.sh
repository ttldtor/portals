#!/bin/sh
# Copyright (c) 2026 ttldtor.
# SPDX-License-Identifier: BSL-1.0
# Usage: native_xterm.sh XTERM SUPERVISOR LOGDIR DEADLINE_MS EXPECTED_EXIT DRIVE QUIT_MS CHILD [ARGS...]
set -eu
if [ "$#" -lt 8 ]; then exit 2; fi
xterm=$1 supervisor=$2 logdir=$3 deadline=$4 expected=$5 drive=$6 quit=$7
shift 7
mkdir "$logdir"
logdir=$(realpath "$logdir")
title="portals-native-$(date +%s)-$$"
sha256sum "$xterm" "$supervisor" "$1" > "$logdir/hashes.txt"
printf 'title=%s\nDISPLAY=%s\nTERM=%s\nexpected_child_exit=%s\n' "$title" "${DISPLAY-}" "${TERM-}" "$expected" > "$logdir/launch.log"
"$xterm" -iconic -geometry 120x40 -title "$title" -xrm 'XTerm*allowSendEvents:true' \
  -e "$supervisor" --log "$logdir/supervisor.log" --title "$title" --deadline-ms "$deadline" \
  --quit-ms "$quit" --expected-exit "$expected" --drive "$drive" -- "$@" \
  > "$logdir/xterm.stdout" 2> "$logdir/xterm.stderr" &
terminalPid=$!
printf 'terminal_pid=%s\n' "$terminalPid" >> "$logdir/launch.log"
limit=$(( ($(date +%s) * 1000 + deadline + 12000) / 1000 ))
forced=0
while kill -0 "$terminalPid" 2>/dev/null; do
  if [ "$(date +%s)" -ge "$limit" ]; then
    forced=1
    kill -TERM "$terminalPid" 2>/dev/null || true
    sleep 1
    kill -KILL "$terminalPid" 2>/dev/null || true
    break
  fi
  sleep 0.1
done
wait "$terminalPid" || true
alive=0
if [ -f "$logdir/supervisor.log" ]; then
  for ownPid in $(sed -n 's/^\(harness\|child\)_pid=\([0-9]*\).*$/\2/p' "$logdir/supervisor.log"); do
    if kill -0 "$ownPid" 2>/dev/null; then alive=$((alive + 1)); fi
  done
fi
printf 'forced_cleanup=%s\nterminal_gone=1\nown_pid_alive=%s\n' "$forced" "$alive" >> "$logdir/launch.log"
cat "$logdir/supervisor.log"
if [ "$forced" -eq 0 ] && [ "$alive" -eq 0 ] && grep -q '^PASS: native application supervisor' "$logdir/supervisor.log"; then exit 0; fi
exit 1
