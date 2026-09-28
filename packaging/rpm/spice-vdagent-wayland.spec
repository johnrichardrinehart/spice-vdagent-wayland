Name:           spice-vdagent-wayland
Version:        0.23.2
Release:        1%{?dist}
Summary:        SPICE agent for Linux guests, with native Wayland support

License:        GPL-3.0-or-later
URL:            https://github.com/bjthompson805/spice-vdagent-wayland
Source0:        %{url}/archive/refs/tags/v%{version}/%{name}-%{version}.tar.gz

BuildRequires:  gcc, make, autoconf, automake, libtool
BuildRequires:  pkgconfig(gtk4)
BuildRequires:  pkgconfig(glib-2.0)
BuildRequires:  pkgconfig(gio-unix-2.0)
BuildRequires:  pkgconfig(wayland-client)
BuildRequires:  pkgconfig(xfixes)
BuildRequires:  pkgconfig(xrandr)
BuildRequires:  pkgconfig(xinerama)
BuildRequires:  pkgconfig(x11)
BuildRequires:  pkgconfig(alsa)
BuildRequires:  pkgconfig(dbus-1)
BuildRequires:  pkgconfig(libdrm)
BuildRequires:  pkgconfig(pciaccess)
BuildRequires:  pkgconfig(libsystemd)
BuildRequires:  pkgconfig(spice-protocol)
BuildRequires:  systemd-rpm-macros

# Same binaries, install paths, and systemd units as the official package --
# this replaces it outright and cannot be installed alongside it.
Provides:       spice-vdagent = %{version}-%{release}
Obsoletes:      spice-vdagent < %{version}-%{release}
Conflicts:      spice-vdagent

%description
Fork of the official spice-vdagent adding native Wayland support:

 * Clipboard sync (text and images, CLIPBOARD and PRIMARY selections)
   via wlr-data-control-unstable-v1, since the standard wl_data_device
   protocol only notifies a client with keyboard focus, which a headless
   agent daemon structurally never has.
 * Resolution-setting via wlr-output-management-unstable-v1 (wlroots
   compositors: Hyprland, Sway, ...) or org.gnome.Mutter.DisplayConfig
   (GNOME), neither of which the official package implements under
   Wayland at all.

%prep
%autosetup -n %{name}-%{version}

%build
# --sysconfdir=%{_sysconfdir}: autotools' own default under --prefix=%{_prefix}
# (${prefix}/etc) is wrong for a real install -- verified locally that the
# spice-vdagent.desktop XDG autostart entry silently never fires from there,
# only from /etc/xdg/autostart.
#
# --with-init-script=systemd: without it, configure's own systemd detection
# does not reliably resolve to "yes" even with libsystemd-devel present --
# verified locally on Arch, where the same gap left socket activation support
# out of the build entirely and the built vdagentd then fought systemd's own
# spice-vdagentd.socket unit for the same listening socket.
# --sbindir=%{_sbindir}: explicit, not left to autotools' own default
# (${exec_prefix}/sbin) -- on a usr-merged Fedora, %_sbindir itself expands
# to the same place as %_bindir, but that's just this spec's own macro
# table, not something the built binary's actual install path knows about
# on its own. Leaving them to disagree is exactly what broke the first CI
# run: %files' %{_sbindir}/spice-vdagentd looked in usr/bin (where the
# macro pointed), but the binary had actually landed in a real, separate
# usr/sbin the autotools default created inside the fresh buildroot.
./autogen.sh --prefix=%{_prefix} --sysconfdir=%{_sysconfdir} \
             --sbindir=%{_sbindir} \
             --with-gtk4=yes --with-init-script=systemd
%make_build

%install
%make_install

%post
%systemd_post spice-vdagentd.socket

%preun
%systemd_preun spice-vdagentd.socket

%postun
%systemd_postun_with_restart spice-vdagentd.service

%files
%license COPYING
%doc README.md CHANGELOG.md
%{_bindir}/spice-vdagent
%{_sbindir}/spice-vdagentd
%{_mandir}/man1/spice-vdagent.1*
%{_mandir}/man1/spice-vdagentd.1*
%{_prefix}/lib/systemd/system/spice-vdagentd.service
%{_prefix}/lib/systemd/system/spice-vdagentd.socket
%{_prefix}/lib/systemd/user/spice-vdagent.service
%{_prefix}/lib/systemd/user/graphical-session.target.wants/spice-vdagent.service
%{_prefix}/lib/tmpfiles.d/spice-vdagentd.conf
%{_prefix}/lib/udev/rules.d/70-spice-vdagentd.rules
%{_sysconfdir}/xdg/autostart/spice-vdagent.desktop

%changelog
* Mon Sep 21 2026 spice-vdagent-wayland <noreply@github.com> - 0.23.2-1
- Packaging-only release; no change to the agent itself. The Arch package
  now installs spice-vdagentd to /usr/bin (0.23.1 shipped a real
  /usr/sbin/ directory that pacman rejected as a conflict with the
  filesystem package's symlink), and CI now test-installs it.
* Sun Sep 06 2026 spice-vdagent-wayland <noreply@github.com> - 0.23.1-1
- Fix guest resolution reporting on a fractionally-scaled Wayland
  compositor: gdk_monitor_get_geometry() returns logical pixels, not the
  physical pixels the guest framebuffer and absolute pointer positions
  actually need, making the on-screen cursor track proportionally wrong
  at any non-integer monitor scale.
* Sat Sep 05 2026 spice-vdagent-wayland <noreply@github.com> - 0.23.0-1
- Initial release: native Wayland clipboard (text + images, CLIPBOARD/
  PRIMARY) and resolution-setting (wlr-output-management / Mutter
  ApplyMonitorsConfig).
