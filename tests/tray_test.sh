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
start menu     --menu --icon-name network-wireless-signal-good --tip-title Network --tip-text "Signal: <b>good</b><br>SSID: a&amp;b"
sleep 2

field() { $C get tray | python3 -c "import json,sys; d=[i for i in json.load(sys.stdin) if i['app_id']=='$1']; print(d[0]['$2'] if d else 'MISSING')"; }
count() { $C get tray | python3 -c "import json,sys; print(len(json.load(sys.stdin)))"; }

echo "== registration and icon source"
check "7 items registered"                      "$(count)" 7
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
check "item removed when its app exits" "$(count)" 6

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

echo "== hover tooltips"
tq() { $C popup tooltip | python3 -c "import json,sys; d=json.load(sys.stdin); print($1)"; }
rect() { $C get tray | python3 -c "import json,sys; r=[i for i in json.load(sys.stdin) if i['app_id']=='$1'][0]['rect']; print(r['x']+r['w']//2, r['y']+r['h']//2)"; }
check "ToolTip title/text read"      "$($C get tray | python3 -c "import json,sys; i=[i for i in json.load(sys.stdin) if i['app_id']=='menu'][0]; print(i['tooltip_title'], '|', repr(i['tooltip_text']))")" "Network | 'Signal: good\nSSID: a&b'"
check "no ToolTip: falls back to Title" "$($C get tray | python3 -c "import json,sys; print([i for i in json.load(sys.stdin) if i['app_id']=='themed'][0]['tooltip_title'])")" themed
$C popup hover $(rect menu); sleep 0.2
check "not shown straight away"      "$(tq "d['visible']")" False
sleep 0.7
check "shown after the delay"        "$(tq "d['visible']")" True
check "title and description lines"  "$(tq "d['lines']")" "['Network', 'Signal: good', 'SSID: a&b']"
$C popup dump "$OUT/5_tooltip.png"
$C popup hover 5 400; sleep 0.2
check "hidden when the pointer leaves" "$(tq "d['visible']")" False
$C popup hover $(rect menu); $C popup hover 5 400; sleep 0.9
check "moving away before the delay cancels it" "$(tq "d['visible']")" False
$C popup hover $(rect themed); sleep 0.9
check "another icon: its own tooltip" "$(tq "d['lines']")" "['themed']"
$C popup hover 5 400

echo "== native popup menu"
pq() { $C popup state | python3 -c "import json,sys; d=json.load(sys.stdin); print($1)"; }
IM=$(vis menu)
: > "$LOG"
$C traymenu $IM; sleep 1
check "right click opens the menu"          "$(pq "d['open']")" True
check "root rows (hidden ones dropped)"     "$(pq "[r['label'] for r in d['panels'][0]['rows']]")" "['Open', '', 'Check me', 'Radio', 'Disabled', 'More', 'Quit']"
check "separator and disabled flagged"      "$(pq "[(r['separator'], r['enabled']) for r in d['panels'][0]['rows']][1:5]")" "[(True, True), (False, True), (False, True), (False, False)]"
check "toggle states read"                  "$(pq "[r['toggle'] for r in d['panels'][0]['rows']][2:4]")" "[1, 1]"
check "icon-name resolved"                  "$(pq "d['panels'][0]['rows'][0]['icon']")" True
$C popup dump "$OUT/3_menu.png"
$C popup key Down;  check "Down selects the first row"         "$(pq "d['panels'][0]['hover']")" 0
$C popup key Down;  check "Down skips the separator"           "$(pq "d['panels'][0]['hover']")" 2
$C popup key Up;    check "Up goes back"                       "$(pq "d['panels'][0]['hover']")" 0
$C popup key Up;    check "Up wraps to the last row"           "$(pq "d['panels'][0]['hover']")" 6
$C popup key Up
check "Up passes over nothing selectable"   "$(pq "d['panels'][0]['hover']")" 5
$C popup key Up
check "Up skips the disabled row"           "$(pq "d['panels'][0]['hover']")" 3
$C popup key Down
check "Down skips the disabled row"         "$(pq "d['panels'][0]['hover']")" 5
$C popup key Right; check "Right opens the submenu"            "$(pq "len(d['panels'])")" 2
check "submenu rows (hidden dropped)"       "$(pq "[r['label'] for r in d['panels'][1]['rows']]")" "['Sub one', 'Sub two']"
check "submenu starts with a selection"     "$(pq "d['panels'][1]['hover']")" 0
$C popup dump "$OUT/4_submenu.png"
$C popup key Left;  check "Left closes the submenu"            "$(pq "len(d['panels'])")" 1
$C popup key Escape; check "Escape closes the menu"            "$(pq "d['open']")" False
check "closing without a choice sends no click" "$(grep -c 'Event' "$LOG")" 0

$C traymenu $IM; sleep 1
HOVERMORE=$(pq "d['panels'][0]['x'] + d['panels'][0]['w']//2, d['panels'][0]['rows'][5]['y'] + d['panels'][0]['rows'][5]['h']//2")
$C popup motion ${HOVERMORE//[(,)]/}
check "hovering a submenu row opens it"     "$(pq "len(d['panels'])")" 2
QUIT=$(pq "d['panels'][0]['x'] + d['panels'][0]['w']//2, d['panels'][0]['rows'][6]['y'] + d['panels'][0]['rows'][6]['h']//2")
$C popup click ${QUIT//[(,)]/}
sleep 0.5
check "clicking a row closes the menu"      "$(pq "d['open']")" False
check "clicking a row sends Event clicked"  "$(grep -c '^menu: Event 10 clicked' "$LOG")" 1
check "AboutToShow sent each time the submenu opens" "$(grep -c '^menu: AboutToShow 6' "$LOG")" 2

$C traymenu $IM; sleep 1
$C popup click 1 1
check "a click outside closes it"           "$(pq "d['open']")" False
check "...without choosing anything"        "$(grep -c '^menu: Event' "$LOG")" 1

kill $NW 2>/dev/null; wait $NW 2>/dev/null
echo "== $pass passed, $fail failed"
[ $fail -eq 0 ]
INNER
