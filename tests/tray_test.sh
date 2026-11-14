#!/bin/bash
# End-to-end tray test: a nested nullwl on a private D-Bus session, fake tray items,
# and checks through IPC. Needs python3-gi, foot-less (no apps). Usage: tests/tray_test.sh [outdir]
set -u
OUT=${1:-/tmp/tray_test}; mkdir -p "$OUT"; rm -f "$OUT"/*
ROOT=$(cd "$(dirname "$0")/.." && pwd)
exec dbus-run-session -- bash -s "$ROOT" "$OUT" <<'INNER'
ROOT=$1; OUT=$2; cd "$ROOT"
pass=0; fail=0
check() { if [ "$2" = "$3" ]; then echo "  ok   $1"; pass=$((pass+1)); else echo "  FAIL $1: expected '$3', got '$2'"; fail=$((fail+1)); fi; }

./nullwl -c /dev/null > "$OUT/nullwl.log" 2>&1 &
NW=$!
sleep 2
export NULLWL_SOCKET=$(ls -t "$XDG_RUNTIME_DIR"/nullwl.*.sock | head -1)
C=./nullwlctl; LOG="$OUT/clicks.log"; : > "$LOG"

declare -A PID
start() { # name args...
  local n=$1; shift; mkfifo "$OUT/$n.fifo"
  python3 tests/sni_item.py --name "$n" --log "$LOG" "$@" < "$OUT/$n.fifo" > "$OUT/$n.out" 2>&1 &
  PID[$n]=$!
  exec {fd}> "$OUT/$n.fifo"; eval "FD_$n=$fd"
}
say() { local v="FD_$1"; echo "${*:2}" >&"${!v}"; }

start themed   --wellknown --icon-name blueman-tray
start pixmap   --pixmap
start fallback --icon-name no-such-icon-xyz --pixmap
start passive  --icon-name network-wireless --status Passive
start letter   
start attn     --icon-name network-wireless --attention-name network-error
sleep 2

field() { $C get tray | python3 -c "import json,sys; d=[i for i in json.load(sys.stdin) if i['app_id']=='$1']; print(d[0]['$2'] if d else 'MISSING')"; }
count() { $C get tray | python3 -c "import json,sys; print(len(json.load(sys.stdin)))"; }

echo "== registration and icon source"
check "6 items registered"                      "$(count)" 6
check "themed: icon from the theme"             "$(field themed icon_source)" theme
check "themed: well-known bus name kept"        "$($C get tray | python3 -c "import json,sys; print([i['bus_name'] for i in json.load(sys.stdin) if i['app_id']=='themed'][0].startswith('org.kde.StatusNotifierItem'))")" True
check "pixmap: icon from the pixmap"            "$(field pixmap icon_source)" pixmap
check "fallback: bad name falls back to pixmap" "$(field fallback icon_source)" pixmap
check "passive: reported as passive"            "$(field passive status)" passive
check "letter: nothing to draw"                 "$(field letter icon_source)" none
$C dump-tray "$OUT/1_initial.png" && echo "  (rendered $OUT/1_initial.png)"

echo "== live updates through signals"
say themed icon network-wireless-signal-good
say pixmap status Passive
say passive status Active
say attn status NeedsAttention
say letter title renamed
sleep 1.5
check "themed: NewIcon from a well-known-name item is honoured" "$(field themed icon_name)" network-wireless-signal-good
check "pixmap: now passive"        "$(field pixmap status)" passive
check "passive: now active"        "$(field passive status)" active
check "attn: needs attention"      "$(field attn status)" attention
check "letter: title updated"      "$(field letter title)" renamed
$C dump-tray "$OUT/2_after_signals.png" && echo "  (rendered $OUT/2_after_signals.png)"

echo "== unregistering"
kill "${PID[letter]}" 2>/dev/null; sleep 1.5
check "item removed when its app exits" "$(count)" 5

echo "== clicks and scrolls reach the item"
: > "$LOG"
# visible position of an item = its index among the non-passive ones, in tray order
vis() { $C get tray | python3 -c "import json,sys; v=[i['app_id'] for i in json.load(sys.stdin) if i['status']!='passive']; print(v.index('$1'))"; }
IA=$(vis attn); IT=$(vis themed)
$C trayactivate $IA;  sleep 0.3
$C traysecondary $IA; sleep 0.3
$C traymenu $IA;      sleep 0.3   # no dbusmenu, so this becomes ContextMenu
$C trayactivate $IT;  sleep 0.3
check "left click = Activate"                  "$(grep -c '^attn: Activate' "$LOG")" 1
check "middle click = SecondaryActivate"       "$(grep -c '^attn: SecondaryActivate' "$LOG")" 1
check "right click without dbusmenu = ContextMenu" "$(grep -c '^attn: ContextMenu' "$LOG")" 1
check "the click lands on the right item"      "$(grep -c '^themed: Activate' "$LOG")" 1
check "no click leaked to the others"          "$(grep -vc '^attn:\|^themed:' "$LOG")" 0

kill $NW 2>/dev/null; wait $NW 2>/dev/null
echo "== $pass passed, $fail failed"
[ $fail -eq 0 ]
INNER
