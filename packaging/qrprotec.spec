# QRProtec : back Django derriere Caddy + front ImGui (bureau ou plein ecran dans Cage).
#
# Paquets produits :
#   qrprotec          borne complete comme avant : back + kiosk sur la meme machine (paquet meta)
#   qrprotec-server   back seul (API Django, Caddy, notifications), sans ecran
#   qrprotec-kiosk    front seul en kiosk (Cage sur tty1), back local ou distant
#   qrprotec-front    front seul en application de bureau (pas de kiosk), back local ou distant
#   qrprotec-common   outils partages : assistant qrprotec-setup, sauvegardes
#
# Sources preparees par packaging/make-sources.sh (appele par `make rpm`) :
#   Source0 : code du depot + sous-modules imgui, implot et scanner_lib
#   Source1 : dependances Python figees par poetry.lock (Django, DRF...)
# La version vient du tag git (packaging/version.sh) :
#   rpmbuild -ba --define "qrprotec_version 1.2.0" packaging/qrprotec.spec

# --without kiosk : back seulement (qrprotec-server, qrprotec-common), aucun front compile
%bcond kiosk 1

%{!?qrprotec_version:%global qrprotec_version 0.0.0}
%if %{without kiosk}
# Sans le front, aucun binaire : pas de paquet debuginfo
%global debug_package %{nil}
%endif

# Documentation et licences de tous les paquets dans /usr/share/doc/qrprotec (lien des unites systemd)
%global _docdir_fmt %{name}
# Convention Fedora pour une application privee independante de l'architecture : /usr/share/<nom>
%global appdir %{_datadir}/%{name}
# Projet CMake du front
%global _vpath_srcdir app
# Le ^ des versions d'instantane (1.2.0^3.gabc1234) est remplace par _ dans le nom des archives
# (%%global est evalue tout de suite : partir de qrprotec_version, deja defini, pas de %%{version})
%global tar_version %{lua: print((rpm.expand("%{qrprotec_version}"):gsub("%^", "_")))}
# Marqueurs passes d'un scriptlet a l'autre (premiere installation...)
%global rpmstate %{_localstatedir}/lib/rpm-state/%{name}

# Bibliotheque privee du SDK Inateck (douchette Bluetooth) : elle ne doit rien fournir au systeme
%global __provides_exclude_from ^%{_libdir}/%{name}/.*$
%global __requires_exclude ^libinateck_scanner_ble\\.so.*$

# Dependances Python embarquees : versions de poetry.lock (verifiees dans %%prep)
%global django_version 6.1.1
%global drf_version 3.18.1
%global asgiref_version 3.12.1
%global sqlparse_version 0.6.0

Name:           qrprotec
Version:        %{qrprotec_version}
Release:        1%{?dist}
Summary:        QR-code inventory kiosk: backend and full-screen front on one machine

# QRProtec : pas de licence publiee. Embarques : Django, DRF, asgiref, sqlparse (BSD-3-Clause),
# Dear ImGui, qrcodegen (MIT), stb_image (MIT ou domaine public), jsQR (Apache-2.0),
# SDK Inateck (binaire proprietaire).
License:        LicenseRef-Proprietary AND BSD-3-Clause AND MIT AND Apache-2.0
URL:            https://github.com/etyloppihacilem/qrprotec
Source0:        %{name}-%{tar_version}.tar.gz
Source1:        %{name}-vendor-%{tar_version}.tar.gz
# Copies de packaging/files/*.sysusers, pour %%pre
Source2:        %{name}.sysusers
Source3:        %{name}-kiosk.sysusers

BuildRequires:  python3-devel
# notifications web (chiffrement Web Push, VAPID) : tests du %check
BuildRequires:  python3-cryptography
BuildRequires:  systemd-rpm-macros
%if %{with kiosk}
BuildRequires:  cmake
BuildRequires:  gcc-c++
BuildRequires:  pkgconfig(glfw3)
BuildRequires:  pkgconfig(libpng)
BuildRequires:  pkgconfig(freetype2)
BuildRequires:  pkgconfig(openssl)
BuildRequires:  mesa-libGL-devel
BuildRequires:  desktop-file-utils
%ifarch x86_64
BuildRequires:  libxdo-devel
%endif
%endif

Requires:       %{name}-server = %{version}-%{release}
%if %{with kiosk}
Requires:       %{name}-kiosk = %{version}-%{release}
%endif

%description
QRProtec turns a minimal Fedora Server into a dedicated inventory kiosk for
first-aid equipment labeled with QR codes. This package installs everything
on one machine, as before the split:
- qrprotec-server: the Django backend behind Caddy (HTTPS);
- qrprotec-kiosk: the ImGui front full screen in Cage on tty1, talking to
  the local API (127.0.0.1, never exposed).
Other fronts (qrprotec-kiosk or qrprotec-front on other machines) can still
connect to this server with their own API key.

%package common
Summary:        QRProtec shared tools: setup assistant and backups
# qrprotec-backup : archives .tar.xz, instantane SQLite
Requires:       tar
Requires:       xz
Requires:       python3
Requires:       util-linux
Requires(post): coreutils
Requires(post): systemd
Requires(preun): systemd
Requires(postun): systemd

%description common
Tools shared by the QRProtec server and kiosk packages: the qrprotec-setup
assistant and qrprotec-backup (scheduled archives of the database and/or the
front settings, depending on what is installed).

%package server
Summary:        QRProtec backend: Django API behind Caddy (HTTPS)
License:        LicenseRef-Proprietary AND BSD-3-Clause AND Apache-2.0
Requires:       %{name}-common = %{version}-%{release}
Requires:       caddy
Requires:       python(abi) = %{python3_version}
# notifications web (Web Push) ; non embarque : module compile, fourni par Fedora
Requires:       python3-cryptography
Requires:       util-linux
Requires(post): coreutils
Requires(post): systemd
Requires(preun): systemd
Requires(postun): systemd

Provides:       bundled(python3dist(django)) = %{django_version}
Provides:       bundled(python3dist(djangorestframework)) = %{drf_version}
Provides:       bundled(python3dist(asgiref)) = %{asgiref_version}
Provides:       bundled(python3dist(sqlparse)) = %{sqlparse_version}
Provides:       bundled(js-jsqr) = 1.4.0

%description server
The QRProtec backend alone, for a server without screen:
- the Django backend runs as a hardened systemd service listening on
  127.0.0.1 only: public API, local API (front of the same machine, no key)
  and remote API (fronts of other machines, one API key per front);
- Caddy exposes the public API and the remote API over HTTPS (ports 80 and
  443).
Create a key for a remote front with: qrprotec-manage frontkey add NAME

%if %{with kiosk}
%package front
Summary:        QRProtec front: ImGui window for a desktop session
License:        LicenseRef-Proprietary AND MIT
Requires:       dejavu-sans-fonts
Requires:       hicolor-icon-theme
Recommends:     alsa-utils
Recommends:     bluez
Provides:       bundled(imgui) = 1.93.0
Provides:       bundled(implot) = 1.1
Provides:       bundled(qrcodegen)
Provides:       bundled(stb_image) = 2.30

%description front
The QRProtec front (scan stack, checks, label printing on a Niimbot printer)
as a regular desktop application, for a machine that is not dedicated to
QRProtec. It talks to the local API of the same machine, or to a remote
QRProtec server over HTTPS with an API key (Settings > Server, or the
QRPROTEC_API_URL and QRPROTEC_API_KEY environment variables).
Printing needs the user to be in the dialout group.

%package kiosk
Summary:        QRProtec kiosk: front full screen in Cage on tty1
License:        LicenseRef-Proprietary
Requires:       %{name}-front%{?_isa} = %{version}-%{release}
Requires:       %{name}-common = %{version}-%{release}
Requires:       cage
# Charges par GLFW avec dlopen (pas de dependance automatique) : sans eux, GLFW ne peut pas
# ouvrir de fenetre Wayland et Cage reste sur un ecran noir
Requires:       libwayland-client
Requires:       libwayland-cursor
Requires:       libwayland-egl
Requires:       libxkbcommon
Requires:       mesa-libEGL
Requires:       mesa-dri-drivers
Requires(post): systemd
Requires(preun): systemd
Requires(postun): systemd
# Extinction de l'ecran, bip de mauvais scan, douchette Bluetooth
Recommends:     swayidle
Recommends:     wlopm
Recommends:     wlr-randr

%description kiosk
Runs the QRProtec front full screen in the Cage Wayland kiosk compositor on
tty1. By default it uses the backend of the same machine (qrprotec-server);
to use a server on another machine:
  sudo qrprotec-setup --api-url https://server.example.org --api-key KEY
%endif

%prep
%autosetup -n %{name}-%{tar_version} -a 1
# Les Provides bundled() doivent suivre poetry.lock
for dist in django-%{django_version} djangorestframework-%{drf_version} \
            asgiref-%{asgiref_version} sqlparse-%{sqlparse_version}; do
    test -d "vendor/$dist.dist-info" || { echo "$dist absent de vendor/ : mettre a jour le spec"; exit 1; }
done

%build
%if %{with kiosk}
# CMAKE_SKIP_BUILD_RPATH : pas de RUNPATH vers l'arborescence de compilation dans l'executable
%cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_SKIP_BUILD_RPATH=ON
%cmake_build
%endif

%install
# --- Back : projet Django + dependances embarquees ---------------------------------------------
install -d %{buildroot}%{appdir}/backend
cp -a database/manage.py database/qrprotecDB database/inventory %{buildroot}%{appdir}/backend/
rm -f %{buildroot}%{appdir}/backend/inventory/tests.py
cp -a vendor %{buildroot}%{appdir}/vendor
find %{buildroot}%{appdir} -name __pycache__ -type d -prune -exec rm -rf {} +
# Rien n'est execute directement (manage.py est lance par qrprotec-manage via python3)
find %{buildroot}%{appdir} -type f -exec chmod 0644 {} +
find %{buildroot}%{appdir} -type f \( -name '*.py' -o -name '*.py-tpl' \) -exec sed -i '1{/^#!/d}' {} +
# Traductions des dependances : francais et anglais seulement
find %{buildroot}%{appdir}/vendor -type d -name locale -prune -print0 | while IFS= read -r -d '' dir; do
    find "$dir" -mindepth 1 -maxdepth 1 -type d ! -name fr ! -name en -exec rm -rf {} +
done
%py_byte_compile %{python3} %{buildroot}%{appdir}

install -Dpm 0755 packaging/files/qrprotec-manage %{buildroot}%{_bindir}/qrprotec-manage
sed -i 's|@APPDIR@|%{appdir}|g' %{buildroot}%{_bindir}/qrprotec-manage

install -Dpm 0640 packaging/files/qrprotec.conf %{buildroot}%{_sysconfdir}/%{name}/qrprotec.conf
install -Dpm 0644 packaging/files/qrprotec.caddyfile \
    %{buildroot}%{_sysconfdir}/caddy/Caddyfile.d/%{name}.caddyfile

install -Dpm 0644 packaging/files/qrprotec.service %{buildroot}%{_unitdir}/qrprotec.service
install -Dpm 0644 packaging/files/qrprotec-alerts.service %{buildroot}%{_unitdir}/qrprotec-alerts.service
install -Dpm 0644 packaging/files/qrprotec-alerts.timer %{buildroot}%{_unitdir}/qrprotec-alerts.timer
install -Dpm 0644 packaging/files/caddy-qrprotec.conf %{buildroot}%{_unitdir}/caddy.service.d/%{name}.conf
install -Dpm 0644 packaging/files/80-qrprotec.preset %{buildroot}%{_presetdir}/80-%{name}.preset
install -Dpm 0644 packaging/files/qrprotec.sysusers %{buildroot}%{_sysusersdir}/%{name}.conf
install -Dpm 0644 packaging/files/qrprotec.tmpfiles %{buildroot}%{_tmpfilesdir}/%{name}.conf
install -d -m 0750 %{buildroot}%{_sharedstatedir}/%{name}

# --- Commun : assistant et sauvegardes ---------------------------------------------------------
install -Dpm 0755 packaging/files/qrprotec-setup %{buildroot}%{_sbindir}/qrprotec-setup
install -Dpm 0755 packaging/files/qrprotec-backup %{buildroot}%{_sbindir}/qrprotec-backup
install -Dpm 0644 packaging/files/backup.conf %{buildroot}%{_sysconfdir}/%{name}/backup.conf
install -Dpm 0644 packaging/files/qrprotec-backup.service %{buildroot}%{_unitdir}/qrprotec-backup.service
sed -i 's|@SBINDIR@|%{_sbindir}|g' %{buildroot}%{_unitdir}/qrprotec-backup.service
install -Dpm 0644 packaging/files/qrprotec-backup.timer %{buildroot}%{_unitdir}/qrprotec-backup.timer
install -Dpm 0644 packaging/files/80-qrprotec-common.preset %{buildroot}%{_presetdir}/80-%{name}-common.preset
install -Dpm 0644 packaging/files/qrprotec-common.tmpfiles %{buildroot}%{_tmpfilesdir}/%{name}-common.conf
install -d -m 0700 %{buildroot}%{_localstatedir}/backups/%{name}

%if %{with kiosk}
# --- Front (bureau) --------------------------------------------------------------------------
install -Dpm 0755 %{__cmake_builddir}/QRProtecApp %{buildroot}%{_libexecdir}/%{name}/qrprotec-front
install -Dpm 0755 packaging/files/qrprotec-front %{buildroot}%{_bindir}/qrprotec-front
sed -i -e 's|@LIBEXECDIR@|%{_libexecdir}|g' -e 's|@LIBDIR@|%{_libdir}|g' -e 's|@DATADIR@|%{_datadir}|g' \
    %{buildroot}%{_bindir}/qrprotec-front
%ifarch x86_64
install -Dpm 0755 app/scanner_lib/ble/linux/x86_64-unknown-linux-gnu/libinateck_scanner_ble.so \
    %{buildroot}%{_libdir}/%{name}/libinateck_scanner_ble.so
%endif
install -d %{buildroot}%{appdir}/templates
install -pm 0644 app/templates/*.qr %{buildroot}%{appdir}/templates/
install -d %{buildroot}%{appdir}/templates/images
install -pm 0644 app/templates/images/* %{buildroot}%{appdir}/templates/images/
desktop-file-install --dir=%{buildroot}%{_datadir}/applications packaging/files/qrprotec.desktop
# Icone de la fenetre et du menu : celle du site web (`make icon`)
install -Dpm 0644 database/inventory/web/icon-512.png %{buildroot}%{_datadir}/icons/hicolor/512x512/apps/%{name}.png
install -Dpm 0644 database/inventory/web/icon-192.png %{buildroot}%{_datadir}/icons/hicolor/192x192/apps/%{name}.png
install -Dpm 0644 database/inventory/web/favicon.png %{buildroot}%{_datadir}/icons/hicolor/32x32/apps/%{name}.png

# --- Kiosk -----------------------------------------------------------------------------------
install -Dpm 0755 packaging/files/qrprotec-kiosk-session \
    %{buildroot}%{_libexecdir}/%{name}/qrprotec-kiosk-session
sed -i 's|@BINDIR@|%{_bindir}|g' %{buildroot}%{_libexecdir}/%{name}/qrprotec-kiosk-session
install -Dpm 0644 packaging/files/kiosk.conf %{buildroot}%{_sysconfdir}/%{name}/kiosk.conf
install -Dpm 0600 packaging/files/front-api.conf %{buildroot}%{_sysconfdir}/%{name}/front-api.conf
install -Dpm 0644 packaging/files/qrprotec-kiosk.service %{buildroot}%{_unitdir}/qrprotec-kiosk.service
install -Dpm 0644 packaging/files/80-qrprotec-kiosk.preset %{buildroot}%{_presetdir}/80-%{name}-kiosk.preset
install -Dpm 0644 packaging/files/qrprotec-kiosk.sysusers %{buildroot}%{_sysusersdir}/%{name}-kiosk.conf
install -Dpm 0644 packaging/files/qrprotec-kiosk.tmpfiles %{buildroot}%{_tmpfilesdir}/%{name}-kiosk.conf
install -Dpm 0644 packaging/files/logind-qrprotec.conf \
    %{buildroot}%{_prefix}/lib/systemd/logind.conf.d/50-%{name}.conf
install -Dpm 0644 packaging/files/sleep-qrprotec.conf \
    %{buildroot}%{_prefix}/lib/systemd/sleep.conf.d/50-%{name}.conf
install -d -m 0750 %{buildroot}%{_sharedstatedir}/%{name}-kiosk
%endif

%check
%if %{with kiosk}
%ctest
desktop-file-validate %{buildroot}%{_datadir}/applications/qrprotec.desktop
%endif
PYTHONPATH=database:vendor QRPROTEC_SECRET_KEY=rpm-check QRPROTEC_DEBUG=0 \
    QRPROTEC_DB_PATH="$PWD/check.sqlite3" \
    %{python3} -s database/manage.py test inventory --noinput -v 1
rm -f check.sqlite3

# --- Scriptlets : commun ---------------------------------------------------------------------
%post common
%tmpfiles_create %{_tmpfilesdir}/%{name}-common.conf
# Premiere installation (ou passage depuis l'ancien paquet unique) : sauvegardes automatiques, sauf si
# elles ont ete desactivees (QRPROTEC_BACKUP_SCHEDULE=off)
if [ $1 -eq 1 ] && \
   ! grep -q '^QRPROTEC_BACKUP_SCHEDULE="\?off"\?$' %{_sysconfdir}/%{name}/backup.conf 2>/dev/null; then
    systemctl --no-reload preset qrprotec-backup.timer >/dev/null 2>&1 || :
    mkdir -p %{rpmstate}
    touch %{rpmstate}/start-backup-timer
fi

%preun common
%systemd_preun qrprotec-backup.timer

%postun common
%systemd_postun qrprotec-backup.timer
if [ $1 -eq 0 ] && [ -d /run/systemd/system ]; then
    # Desinstallation : drop-in de frequence genere par qrprotec-backup ; sauvegardes conservees
    rm -rf %{_sysconfdir}/systemd/system/qrprotec-backup.timer.d
    systemctl daemon-reload >/dev/null 2>&1 || :
fi

%posttrans common
if [ -d /run/systemd/system ] && [ -e %{rpmstate}/start-backup-timer ]; then
    rm -f %{rpmstate}/start-backup-timer
    systemctl daemon-reload >/dev/null 2>&1 || :
    systemctl start qrprotec-backup.timer >/dev/null 2>&1 || :
fi

# --- Scriptlets : back -----------------------------------------------------------------------
%pre server
# Utilisateurs crees avant la pose des fichiers, si rpm ne le fait pas lui-meme (sysusers natif)
%{?sysusers_create_compat:%sysusers_create_compat %{SOURCE2}}
# Passage depuis l'ancien paquet unique qrprotec : le back est deja installe et active, ne pas
# reactiver ce que l'administrateur a coupe (une reinstallation apres desinstallation, elle, l'active)
if [ $1 -eq 1 ] && [ -e %{_sysconfdir}/systemd/system/multi-user.target.wants/qrprotec.service ]; then
    mkdir -p %{rpmstate}
    touch %{rpmstate}/server-existing
fi

%post server
if [ ! -e %{rpmstate}/server-existing ]; then
%systemd_post qrprotec.service qrprotec-alerts.timer
fi
# rpm cree les utilisateurs (lignes u) mais ignore les appartenances aux groupes (lignes m)
systemd-sysusers %{_sysusersdir}/%{name}.conf >/dev/null 2>&1 || :
%tmpfiles_create %{_tmpfilesdir}/%{name}.conf
# Cle secrete de Django, generee une seule fois et jamais remplacee
if [ ! -s %{_sysconfdir}/%{name}/secret_key ]; then
    (umask 077 && head -c 48 /dev/urandom | base64 -w 0 | tr '+/' '-_' \
        > %{_sysconfdir}/%{name}/secret_key) || :
    chgrp qrprotec %{_sysconfdir}/%{name}/secret_key 2>/dev/null || :
    chmod 0640 %{_sysconfdir}/%{name}/secret_key 2>/dev/null || :
fi
chgrp qrprotec %{_sysconfdir}/%{name}/qrprotec.conf 2>/dev/null || :
# SELinux : Caddy (domaine httpd_t) doit pouvoir joindre le back sur 127.0.0.1:8000 et 8002
if command -v selinuxenabled >/dev/null 2>&1 && selinuxenabled; then
    if getsebool httpd_can_network_connect 2>/dev/null | grep -q -- '--> off'; then
        setsebool -P httpd_can_network_connect 1 || :
    fi
fi
if [ $1 -eq 1 ] && [ ! -e %{rpmstate}/server-existing ]; then
    # Premiere installation : Caddy active au demarrage, services demarres en fin de transaction
    systemctl --no-reload preset caddy.service >/dev/null 2>&1 || :
    mkdir -p %{rpmstate}
    touch %{rpmstate}/server-first-install
fi
rm -f %{rpmstate}/server-existing

%preun server
%systemd_preun qrprotec.service qrprotec-alerts.timer

%postun server
%systemd_postun_with_restart qrprotec.service qrprotec-alerts.timer
if [ $1 -eq 0 ] && [ -d /run/systemd/system ]; then
    # Desinstallation : Caddy oublie le site QRProtec ; donnees et configuration modifiee restent
    systemctl daemon-reload >/dev/null 2>&1 || :
    systemctl try-reload-or-restart caddy.service >/dev/null 2>&1 || :
fi

%posttrans server
if [ -d /run/systemd/system ]; then
    systemctl daemon-reload >/dev/null 2>&1 || :
    if [ -e %{rpmstate}/server-first-install ]; then
        rm -f %{rpmstate}/server-first-install
        systemctl start qrprotec.service qrprotec-alerts.timer >/dev/null 2>&1 || :
        systemctl start caddy.service >/dev/null 2>&1 || :
    fi
    # Installation ou mise a jour : Caddy relit le fragment et l'environnement
    systemctl try-reload-or-restart caddy.service >/dev/null 2>&1 || :
fi

%if %{with kiosk}
# --- Scriptlets : kiosk ----------------------------------------------------------------------
%pre kiosk
%{?sysusers_create_compat:%sysusers_create_compat %{SOURCE3}}
# Passage depuis l'ancien paquet unique qrprotec : le kiosk est deja installe et active
if [ $1 -eq 1 ] && [ -e %{_sysconfdir}/systemd/system/graphical.target.wants/qrprotec-kiosk.service ]; then
    mkdir -p %{rpmstate}
    touch %{rpmstate}/kiosk-existing
fi

%post kiosk
if [ ! -e %{rpmstate}/kiosk-existing ]; then
%systemd_post qrprotec-kiosk.service
fi
systemd-sysusers %{_sysusersdir}/%{name}-kiosk.conf >/dev/null 2>&1 || :
%tmpfiles_create %{_tmpfilesdir}/%{name}-kiosk.conf
if [ $1 -eq 1 ] && [ ! -e %{rpmstate}/kiosk-existing ]; then
    # Premiere installation : demarrage sur la cible graphique (kiosk)
    systemctl set-default graphical.target >/dev/null 2>&1 || :
    mkdir -p %{rpmstate}
    touch %{rpmstate}/kiosk-first-install
fi
rm -f %{rpmstate}/kiosk-existing

%preun kiosk
%systemd_preun qrprotec-kiosk.service

%postun kiosk
%systemd_postun_with_restart qrprotec-kiosk.service
if [ $1 -eq 0 ] && [ -d /run/systemd/system ]; then
    systemctl daemon-reload >/dev/null 2>&1 || :
    if [ "$(systemctl get-default 2>/dev/null)" = graphical.target ] && \
       ! systemctl is-enabled --quiet display-manager.service 2>/dev/null; then
        systemctl set-default multi-user.target >/dev/null 2>&1 || :
    fi
    systemctl --no-block start getty@tty1.service >/dev/null 2>&1 || :
fi

%posttrans kiosk
if [ -d /run/systemd/system ] && [ -e %{rpmstate}/kiosk-first-install ]; then
    rm -f %{rpmstate}/kiosk-first-install
    systemctl daemon-reload >/dev/null 2>&1 || :
    # apres le back s'il est installe dans la meme transaction (After=qrprotec.service)
    systemctl --no-block start qrprotec-kiosk.service >/dev/null 2>&1 || :
fi
%endif

%files
# Paquet meta : borne complete (back + kiosk)
%doc README.md

%files common
%doc README.md
%{_sbindir}/qrprotec-setup
%{_sbindir}/qrprotec-backup
%dir %{_sysconfdir}/%{name}
%config(noreplace) %{_sysconfdir}/%{name}/backup.conf
%{_unitdir}/qrprotec-backup.service
%{_unitdir}/qrprotec-backup.timer
%{_presetdir}/80-%{name}-common.preset
%{_tmpfilesdir}/%{name}-common.conf
%dir %attr(0700,root,root) %{_localstatedir}/backups/%{name}

%files server
%license database/inventory/web/vendor/jsQR-LICENSE
%{_bindir}/qrprotec-manage
%dir %{appdir}
%{appdir}/backend/
%{appdir}/vendor/
%config(noreplace) %attr(0640,root,qrprotec) %{_sysconfdir}/%{name}/qrprotec.conf
%dir %{_sysconfdir}/caddy/Caddyfile.d
%config(noreplace) %{_sysconfdir}/caddy/Caddyfile.d/%{name}.caddyfile
%{_unitdir}/qrprotec.service
%{_unitdir}/qrprotec-alerts.service
%{_unitdir}/qrprotec-alerts.timer
%dir %{_unitdir}/caddy.service.d
%{_unitdir}/caddy.service.d/%{name}.conf
%{_presetdir}/80-%{name}.preset
%{_sysusersdir}/%{name}.conf
%{_tmpfilesdir}/%{name}.conf
%dir %attr(0750,qrprotec,qrprotec) %{_sharedstatedir}/%{name}

%if %{with kiosk}
%files front
%doc README.md
%license app/third_party/qrcodegen/LICENSE app/imgui/LICENSE.txt
%license app/implot/LICENSE
%{_bindir}/qrprotec-front
%dir %{_libexecdir}/%{name}
%{_libexecdir}/%{name}/qrprotec-front
%ifarch x86_64
%{_libdir}/%{name}/
%endif
%dir %{appdir}
%{appdir}/templates/
%{_datadir}/applications/qrprotec.desktop
%{_datadir}/icons/hicolor/512x512/apps/%{name}.png
%{_datadir}/icons/hicolor/192x192/apps/%{name}.png
%{_datadir}/icons/hicolor/32x32/apps/%{name}.png

%files kiosk
%dir %{_libexecdir}/%{name}
%{_libexecdir}/%{name}/qrprotec-kiosk-session
%dir %{_sysconfdir}/%{name}
%config(noreplace) %{_sysconfdir}/%{name}/kiosk.conf
%config(noreplace) %attr(0600,root,root) %{_sysconfdir}/%{name}/front-api.conf
%{_unitdir}/qrprotec-kiosk.service
%{_presetdir}/80-%{name}-kiosk.preset
%{_sysusersdir}/%{name}-kiosk.conf
%{_tmpfilesdir}/%{name}-kiosk.conf
%dir %{_prefix}/lib/systemd/logind.conf.d
%{_prefix}/lib/systemd/logind.conf.d/50-%{name}.conf
%dir %{_prefix}/lib/systemd/sleep.conf.d
%{_prefix}/lib/systemd/sleep.conf.d/50-%{name}.conf
%dir %attr(0750,qrprotec-kiosk,qrprotec-kiosk) %{_sharedstatedir}/%{name}-kiosk
%endif

%changelog
* %{lua: print(os.date("!%a %b %d %Y"))} Hippolyte Melica <hippolytemelica@gmail.com> - %{version}-%{release}
- Build from git, see the repository history for details
