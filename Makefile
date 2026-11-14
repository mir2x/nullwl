.POSIX:
.SUFFIXES:

include config.mk

# flags for compiling
NWLCPPFLAGS = -I. -DWLR_USE_UNSTABLE -D_POSIX_C_SOURCE=200809L \
	-DVERSION=\"$(VERSION)\" $(XWAYLAND)
NWLDEVCFLAGS = -g -Wpedantic -Wall -Wextra -Wdeclaration-after-statement \
	-Wno-unused-parameter -Wshadow -Wunused-macros -Werror=strict-prototypes \
	-Werror=implicit -Werror=return-type -Werror=incompatible-pointer-types \
	-Wfloat-conversion

# CFLAGS / LDFLAGS
PKGS      = wayland-server xkbcommon libinput pixman-1 fcft dbus-1 cairo librsvg-2.0 $(XLIBS)
NWLCFLAGS = `$(PKG_CONFIG) --cflags $(PKGS)` $(WLR_INCS) $(NWLCPPFLAGS) $(NWLDEVCFLAGS) $(CFLAGS)
LDLIBS    = `$(PKG_CONFIG) --libs $(PKGS)` $(WLR_LIBS) -lm $(LIBS)

TRAYOBJS = systray/watcher.o systray/tray.o systray/item.o systray/icon.o systray/menu.o systray/helpers.o systray/themeicon.o
TRAYDEPS = systray/watcher.h systray/tray.h systray/item.h systray/icon.h systray/menu.h systray/helpers.h systray/themeicon.h

all: nullwl nullwlctl
nullwlctl: nullwlctl.c
	$(CC) $(NWLDEVCFLAGS) -D_POSIX_C_SOURCE=200809L $(CFLAGS) $(LDFLAGS) -o $@ nullwlctl.c
nullwl: nullwl.o util.o conf.o ipc.o dbus.o $(TRAYOBJS) $(TRAYDEPS)
	$(CC) nullwl.o util.o conf.o ipc.o dbus.o $(TRAYOBJS) $(NWLCFLAGS) $(LDFLAGS) $(LDLIBS) -o $@
nullwl.o: nullwl.c popup.h client.h ime.h dbus.h conf.h rtconf.h ipc.h ipcstate.h config.h config.mk cursor-shape-v1-protocol.h \
	pointer-constraints-unstable-v1-protocol.h wlr-layer-shell-unstable-v1-protocol.h \
	wlr-output-power-management-unstable-v1-protocol.h xdg-shell-protocol.h \
	simple_scratchpad.h $(TRAYDEPS)
util.o: util.c util.h
conf.o: conf.c conf.h
ipc.o: ipc.c ipc.h
dbus.o: dbus.c dbus.h
systray/watcher.o: systray/watcher.c $(TRAYDEPS)
systray/tray.o: systray/tray.c $(TRAYDEPS)
systray/item.o: systray/item.c $(TRAYDEPS)
systray/icon.o: systray/icon.c $(TRAYDEPS)
systray/menu.o: systray/menu.c $(TRAYDEPS)
systray/helpers.o: systray/helpers.c $(TRAYDEPS)

# wayland-scanner is a tool which generates C headers and rigging for Wayland
# protocols, which are specified in XML. wlroots requires you to rig these up
# to your build system yourself and provide them in the include path.
WAYLAND_SCANNER   = `$(PKG_CONFIG) --variable=wayland_scanner wayland-scanner`
WAYLAND_PROTOCOLS = `$(PKG_CONFIG) --variable=pkgdatadir wayland-protocols`

cursor-shape-v1-protocol.h:
	$(WAYLAND_SCANNER) enum-header \
		$(WAYLAND_PROTOCOLS)/staging/cursor-shape/cursor-shape-v1.xml $@
pointer-constraints-unstable-v1-protocol.h:
	$(WAYLAND_SCANNER) enum-header \
		$(WAYLAND_PROTOCOLS)/unstable/pointer-constraints/pointer-constraints-unstable-v1.xml $@
wlr-layer-shell-unstable-v1-protocol.h:
	$(WAYLAND_SCANNER) enum-header \
		protocols/wlr-layer-shell-unstable-v1.xml $@
wlr-output-power-management-unstable-v1-protocol.h:
	$(WAYLAND_SCANNER) server-header \
		protocols/wlr-output-power-management-unstable-v1.xml $@
xdg-shell-protocol.h:
	$(WAYLAND_SCANNER) server-header \
		$(WAYLAND_PROTOCOLS)/stable/xdg-shell/xdg-shell.xml $@

config.h:
	cp config.def.h $@
clean:
	rm -f nullwl nullwlctl *.o *-protocol.h systray/*.o

dist: clean
	mkdir -p nullwl-$(VERSION)
	cp -R LICENSE* Makefile CHANGELOG.md README.md client.h config.def.h \
		config.mk protocols nullwl.1 nullwl.c ime.h util.c util.h conf.c conf.h rtconf.h ipc.c ipc.h ipcstate.h nullwlctl.c nullwl.desktop nullwl.conf.example \
		nullwl-$(VERSION)
	tar -caf nullwl-$(VERSION).tar.gz nullwl-$(VERSION)
	rm -rf nullwl-$(VERSION)

install: nullwl nullwlctl
	mkdir -p $(DESTDIR)$(PREFIX)/bin
	rm -f $(DESTDIR)$(PREFIX)/bin/nullwl
	cp -f nullwl $(DESTDIR)$(PREFIX)/bin
	chmod 755 $(DESTDIR)$(PREFIX)/bin/nullwl
	cp -f nullwlctl $(DESTDIR)$(PREFIX)/bin
	chmod 755 $(DESTDIR)$(PREFIX)/bin/nullwlctl
	mkdir -p $(DESTDIR)$(MANDIR)/man1
	cp -f nullwl.1 $(DESTDIR)$(MANDIR)/man1
	chmod 644 $(DESTDIR)$(MANDIR)/man1/nullwl.1
	mkdir -p $(DESTDIR)$(DATADIR)/wayland-sessions
	cp -f nullwl.desktop $(DESTDIR)$(DATADIR)/wayland-sessions/nullwl.desktop
	chmod 644 $(DESTDIR)$(DATADIR)/wayland-sessions/nullwl.desktop
uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/nullwl $(DESTDIR)$(PREFIX)/bin/nullwlctl $(DESTDIR)$(MANDIR)/man1/nullwl.1 \
		$(DESTDIR)$(DATADIR)/wayland-sessions/nullwl.desktop

.SUFFIXES: .c .o
.c.o:
	$(CC) $(CPPFLAGS) $(NWLCFLAGS) -o $@ -c $<
