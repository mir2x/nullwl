# Changelog

nullwl's own changes are listed here. For dwl's history before the fork point
(49ca434), see https://codeberg.org/dwl/dwl.

## Unreleased
- Tray: freedesktop icon theme lookup (SVG and PNG, `icon_theme` option), `IconName`,
  `Status`, attention icons, live updates; items using well-known bus names
  (Qt, KDE) can now register; exact click targets; middle click and scroll.
- Tray menus are drawn by nullwl (checkmarks, radios, icons, submenus, keyboard
  and wheel navigation) instead of piping to rofi.
- IPC: `popup` command, richer `get tray`, `dump-tray`.
- Forked from dwl 49ca434 (v0.8-12, wlroots 0.20) and renamed to nullwl.
