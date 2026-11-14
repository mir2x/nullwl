# nullwl

> ## Fork of dwl: credit where it is due
>
> **nullwl is a hard fork of [dwl](https://codeberg.org/dwl/dwl)** by the dwl
> contributors. dwl is itself derived from [dwm](https://dwm.suckless.org),
> [sway](https://github.com/swaywm/sway) and
> [tinywl](https://gitlab.freedesktop.org/wlroots/wlroots). Almost everything
> in this repository is their work, and all of it is under their licenses.
>
> - **Fork point:** dwl commit
>   [`49ca434`](https://codeberg.org/dwl/dwl/commit/49ca43451486c23776ada284023a2e8b7501913f)
>   (2026-01-15, `v0.8-12`, "Bumped to 0.9 for wlroots-0.20.0").
> - **This repo's first commit is that snapshot, unmodified.** Everything after
>   it is a change made in nullwl: `git diff a0221a5 HEAD` shows all of it.
> - **nullwl does not track dwl.** There is no upstream remote and no merging.
>   Fixes from dwl are only brought over by hand.

nullwl is a personal Wayland compositor based on [wlroots](https://gitlab.freedesktop.org/wlroots/wlroots).
Like dwl it is small, configured at compile time through `config.h`, and meant
to be read and changed.

## What was added on top of dwl

Community patches from the dwl patch collection
(<https://codeberg.org/dwl/dwl-patches>), each written by its own author, listed
there with their names. The patches applied here are:

bar, awesomebar, autostart, movestack, setrule, pertag, swallow,
simple_scratchpad, vanitygaps, hide_vacant_tags, bar-systray, alwayscenter.

Also from upstream dwl's contributor Guido Cella: IME support (input-method
and text-input protocols) and a use-after-free fix.

nullwl's own changes: coloured status text in the bar, ordered shutdown with
child cleanup, a StatusNotifierItem system tray (themed icons, native popup menus with submenus, scroll), XWayland
enabled, the Tokyo Night theme, and keybinding changes.

## Configuration

nullwl reads `~/.config/nullwl/nullwl.conf` (or `-c file`). `config.def.h`
holds the built-in defaults, so nullwl boots even with no file at all. Start
from [`nullwl.conf.example`](nullwl.conf.example), which documents every
option: appearance, input, key and mouse bindings, window rules, monitors,
autostart.

Reload without restarting: `Super+Shift+C`, or `kill -HUP $(pidof nullwl)`.
The file is reset to the defaults and re-read, so deleting a line really
removes it. A bad line is reported as `file:line: reason` and skipped.
Options that are only read at startup (font, bar and tray switches) say so
when they change.

## IPC

`nullwlctl` talks to a running nullwl over a Unix socket (newline-delimited
JSON): query monitors, windows and the tray, run any key-binding action, change
options live, and subscribe to events (focus, tags, windows, layout, ...).
See [IPC.md](IPC.md).

```
nullwlctl get clients | jq
nullwlctl view 3
nullwlctl subscribe focus tags
```

## Building

Dependencies: wlroots 0.20 (with libinput, and X11 for XWayland), wayland,
wayland-protocols, libinput, xkbcommon, pixman, fcft, libdbus, cairo,
librsvg, xcb and xcb-icccm, and pkg-config. `Xwayland` is needed at runtime.

```
cp config.def.h config.h   # first time only, then edit config.h
make
sudo make install
```

This installs the `nullwl` binary, its man page and the `nullwl.desktop`
session entry.

## License

GPLv3 or later, the same as dwl. See `LICENSE` for nullwl. The notices of dwl
(`LICENSE.dwl`) and of the code it derives from (`LICENSE.dwm`, `LICENSE.sway`,
`LICENSE.tinywl`) are preserved unchanged.
