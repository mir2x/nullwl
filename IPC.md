# nullwl IPC

nullwl listens on a Unix socket and speaks newline-delimited JSON: one request
per line, one reply per request, and (after `subscribe`) one event per line.

- Socket: `$XDG_RUNTIME_DIR/nullwl.$WAYLAND_DISPLAY.sock`, mode 0600, same-user
  only. nullwl exports the path as `NULLWL_SOCKET` to everything it starts.
- `nullwlctl` is a small client for it; anything that can write a line to a
  socket works too: `echo '{"cmd":"version"}' | socat - UNIX-CONNECT:$NULLWL_SOCKET`.

## Requests and replies

```
{"cmd":"dispatch","action":"view","arg":"3"}
{"ok":true,"data":null}
{"ok":false,"error":"view: expected a tag 1..9 or 'all', got '99'"}
```

Requests are flat objects: values are strings, numbers, booleans, null, or
arrays of those. A request line may be up to 64 KiB.

| cmd | fields | reply data |
|---|---|---|
| `version` | | `{name, version, protocol}` |
| `get` | `what`: `monitors` `clients` `focused` `tray` `errors` `options` `actions` | see below |
| `dispatch` | `action`, optional `arg` | `null`. Runs any key-binding action (`get actions`) |
| `set` | `option`, `value` | `null`. Changes an option live, until the next reload |
| `focus` / `close` | `id` of a window | `null` |
| `reload` | | `{errors, messages}` |
| `subscribe` | optional `events`: list of event names | `null`, then events follow |

`nullwlctl` maps these directly: `nullwlctl get clients`,
`nullwlctl view 3` (any action name works as a command), `nullwlctl set borderpx 2`.

### `get`

- `monitors`: `name focused x y width height scale tagset occupied urgent tags[] layout symbol nmaster mfact gaps{}`.
  `tagset`, `occupied` and `urgent` are bit masks, bit 0 = tag 1.
- `clients` / `focused`: `id app_id title pid monitor tags floating fullscreen hidden urgent focused xwayland x y width height`.
  `id` is stable for the life of the window.
- `tray`: `[{bus_name, object, app_id, ready}]`.
- `errors`: the file and the messages from the last (re)load, as `file:line: reason`.
- `options`: every option with its current `value` and whether it is `restart`-only.

## Events

After `subscribe`, the server sends `{"event":NAME,"data":...}` lines. Events are
derived by comparing snapshots, so a burst of changes is reported once, as the
net difference.

| event | data |
|---|---|
| `window-open` | client object |
| `window-close` | `{id}` |
| `window-state` | client object, when tags, floating, fullscreen, hidden, urgent or monitor changed |
| `title` | `{id, title}` |
| `focus` | client object, or `null` when nothing is focused |
| `tags` | `{monitor, tagset, occupied, urgent}` |
| `layout` | `{monitor, symbol, nmaster, mfact}` |
| `monitor-add` / `monitor-remove` | `{name}` |
| `tray` | `{items}` |
| `config-reload` | `{errors}` |

A subscriber that stops reading is disconnected once about 1 MiB is queued, so
it can never stall the compositor.

## Example

```sh
# print the focused window's title whenever it changes
nullwlctl subscribe focus title | while read -r line; do
    echo "$line" | jq -r '.data.title // empty'
done
```
