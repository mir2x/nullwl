#!/usr/bin/env python3
"""A fake StatusNotifierItem for testing the tray.

  sni_item.py [--wellknown] [--name Id] [--icon-name N] [--theme-path DIR]
              [--pixmap] [--status Active|Passive|NeedsAttention] [--log FILE]

--pixmap serves a 32x32 image whose left half is red and right half is blue,
so a wrong byte order or a flipped image is obvious in a screenshot.
Commands on stdin change it live and emit the matching signal:
  icon NAME | pixmap | nopixmap | status S | title T | attention NAME
Clicks and scrolls it receives are appended to --log.
"""
import argparse, sys
import gi
gi.require_version("Gio", "2.0")
from gi.repository import Gio, GLib

XML = """<node><interface name="org.kde.StatusNotifierItem">
<property name="Category" type="s" access="read"/><property name="Id" type="s" access="read"/>
<property name="Title" type="s" access="read"/><property name="Status" type="s" access="read"/>
<property name="WindowId" type="u" access="read"/><property name="IconName" type="s" access="read"/>
<property name="IconPixmap" type="a(iiay)" access="read"/><property name="IconThemePath" type="s" access="read"/>
<property name="AttentionIconName" type="s" access="read"/><property name="AttentionIconPixmap" type="a(iiay)" access="read"/>
<property name="ToolTip" type="(sa(iiay)ss)" access="read"/><property name="ItemIsMenu" type="b" access="read"/><property name="Menu" type="o" access="read"/>
<method name="Activate"><arg type="i" name="x" direction="in"/><arg type="i" name="y" direction="in"/></method>
<method name="SecondaryActivate"><arg type="i" name="x" direction="in"/><arg type="i" name="y" direction="in"/></method>
<method name="ContextMenu"><arg type="i" name="x" direction="in"/><arg type="i" name="y" direction="in"/></method>
<method name="Scroll"><arg type="i" name="delta" direction="in"/><arg type="s" name="orientation" direction="in"/></method>
<signal name="NewIcon"/><signal name="NewAttentionIcon"/><signal name="NewStatus"><arg type="s"/></signal>
<signal name="NewTitle"/></interface></node>"""

ap = argparse.ArgumentParser()
ap.add_argument("--wellknown", action="store_true")
ap.add_argument("--name", default="test")
ap.add_argument("--icon-name", default="")
ap.add_argument("--theme-path", default="")
ap.add_argument("--attention-name", default="")
ap.add_argument("--pixmap", action="store_true")
ap.add_argument("--status", default="Active")
ap.add_argument("--menu-only", action="store_true")
ap.add_argument("--tip-title", default="")
ap.add_argument("--tip-text", default="")
ap.add_argument("--menu", action="store_true", help="serve a dbusmenu")
ap.add_argument("--log", default="/dev/null")
args = ap.parse_args()

state = dict(icon=args.icon_name, pixmap=args.pixmap, status=args.status, title=args.name, attention=args.attention_name)

def pixmap_variant():
    if not state["pixmap"]:
        return GLib.Variant("a(iiay)", [])
    w = h = 32
    px = bytearray()
    for y in range(h):
        for x in range(w):
            px += bytes((255, 255, 0, 0)) if x < w // 2 else bytes((255, 0, 0, 255))  # A R G B
    return GLib.Variant("a(iiay)", [(w, h, bytes(px))])

def get_property(conn, sender, path, iface, prop):
    return {
        "Category": GLib.Variant("s", "ApplicationStatus"), "Id": GLib.Variant("s", args.name),
        "Title": GLib.Variant("s", state["title"]), "Status": GLib.Variant("s", state["status"]),
        "WindowId": GLib.Variant("u", 0), "IconName": GLib.Variant("s", state["icon"]),
        "IconPixmap": pixmap_variant(), "IconThemePath": GLib.Variant("s", args.theme_path),
        "AttentionIconName": GLib.Variant("s", state["attention"]),
        "AttentionIconPixmap": GLib.Variant("a(iiay)", []),
        "ToolTip": GLib.Variant("(sa(iiay)ss)", ("", [], args.tip_title, args.tip_text)),
        "ItemIsMenu": GLib.Variant("b", args.menu_only), "Menu": GLib.Variant("o", "/MenuBar" if args.menu else "/NO_DBUSMENU"),
    }[prop]

def log(line):
    with open(args.log, "a") as f:
        f.write(f"{args.name}: {line}\n")

def on_call(conn, sender, path, iface, method, params, invocation):
    log(method + " " + " ".join(str(v) for v in params.unpack()))
    invocation.return_value(None)

conn = Gio.bus_get_sync(Gio.BusType.SESSION, None)
info = Gio.DBusNodeInfo.new_for_xml(XML)
conn.register_object("/StatusNotifierItem", info.interfaces[0], on_call, get_property, None)

MENU_XML = """<node><interface name="com.canonical.dbusmenu">
<method name="GetLayout"><arg type="i" direction="in"/><arg type="i" direction="in"/><arg type="as" direction="in"/>
<arg type="u" direction="out"/><arg type="(ia{sv}av)" direction="out"/></method>
<method name="Event"><arg type="i" direction="in"/><arg type="s" direction="in"/><arg type="v" direction="in"/><arg type="u" direction="in"/></method>
<method name="AboutToShow"><arg type="i" direction="in"/><arg type="b" direction="out"/></method>
</interface></node>"""

def V(sig, val):
    return GLib.Variant(sig, val)

def node(id, label=None, kids=(), **p):
    props = {}
    if label is not None: props["label"] = V("s", label)
    for k, v in p.items():
        k = k.replace("_", "-")
        props[k] = V("b", v) if isinstance(v, bool) else V("i", v) if isinstance(v, int) else V("s", v)
    return (id, props, [V("(ia{sv}av)", c) for c in kids])

LAYOUT = node(0, None, [
    node(1, "_Open", icon_name="document-open"),
    node(2, type="separator"),
    node(3, "Check me", toggle_type="checkmark", toggle_state=1),
    node(4, "Radio", toggle_type="radio", toggle_state=1),
    node(5, "Disabled", enabled=False),
    node(6, "More", [node(7, "Sub one"), node(8, "Sub two"), node(9, "Hidden", visible=False)], children_display="submenu"),
    node(10, "Quit"),
])

def on_menu_call(conn, sender, path, iface, method, params, invocation):
    if method == "GetLayout":
        invocation.return_value(V("(u(ia{sv}av))", (1, LAYOUT)))
    elif method == "AboutToShow":
        log(f"AboutToShow {params.unpack()[0]}")
        invocation.return_value(V("(b)", (False,)))
    else:
        u = params.unpack()
        log(f"Event {u[0]} {u[1]}")
        invocation.return_value(None)

if args.menu:
    conn.register_object("/MenuBar", Gio.DBusNodeInfo.new_for_xml(MENU_XML).interfaces[0], on_menu_call, None, None)

service = conn.get_unique_name()
if args.wellknown:
    service = f"org.kde.StatusNotifierItem-{GLib.random_int_range(1000, 99999)}-1"
    conn.call_sync("org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "RequestName",
                   GLib.Variant("(su)", (service, 0)), None, Gio.DBusCallFlags.NONE, -1, None)

def register(*_):
    try:
        conn.call_sync("org.kde.StatusNotifierWatcher", "/StatusNotifierWatcher", "org.kde.StatusNotifierWatcher",
                       "RegisterStatusNotifierItem", GLib.Variant("(s)", (service,)), None, Gio.DBusCallFlags.NONE, 2000, None)
        print(f"registered {args.name} as {service}", flush=True)
    except GLib.Error as e:
        print("register failed:", e.message, flush=True)
        return True  # retry: the watcher may not be up yet
    return False
GLib.timeout_add(300, register)

def emit(name, params=None):
    conn.emit_signal(None, "/StatusNotifierItem", "org.kde.StatusNotifierItem", name, params)

def on_stdin(fd, cond):
    line = sys.stdin.readline()
    if not line:
        loop.quit(); return False
    cmd, _, arg = line.strip().partition(" ")
    if cmd == "icon": state["icon"] = arg; emit("NewIcon")
    elif cmd == "pixmap": state["pixmap"] = True; emit("NewIcon")
    elif cmd == "nopixmap": state["pixmap"] = False; emit("NewIcon")
    elif cmd == "status": state["status"] = arg; emit("NewStatus", GLib.Variant("(s)", (arg,)))
    elif cmd == "title": state["title"] = arg; emit("NewTitle")
    elif cmd == "attention": state["attention"] = arg; emit("NewAttentionIcon")
    return True
GLib.io_add_watch(sys.stdin.fileno(), GLib.IO_IN, on_stdin)
loop = GLib.MainLoop()
loop.run()
