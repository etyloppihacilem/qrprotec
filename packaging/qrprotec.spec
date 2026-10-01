# Borne QRProtec : back Django derriere Caddy + front ImGui en plein ecran dans Cage.
#
# Sources preparees par packaging/make-sources.sh (appele par `make rpm`) :
#   Source0 : code du depot + sous-modules imgui et scanner_lib
#   Source1 : dependances Python figees par poetry.lock (Django, DRF...)
# La version vient du tag git (packaging/version.sh) :
#   rpmbuild -ba --define "qrprotec_version 1.2.0" packaging/qrprotec.spec

# --without kiosk : back + Caddy seulement (serveur sans ecran)
%bcond kiosk 1

%{!?qrprotec_version:%global qrprotec_version 0.0.0}
%if %{without kiosk}
# Sans le front, aucun binaire : pas de paquet debuginfo
%global debug_package %{nil}
%endif

# Convention Fedora pour une application privee independante de l'architecture : /usr/share/<nom>
%global appdir %{_datadir}/%{name}
# Projet CMake du front
%global _vpath_srcdir app
# Le ^ des versions d'instantane (1.2.0^3.gabc1234) est remplace par _ dans le nom des archives
# (%%global est evalue tout de suite : partir de qrprotec_version, deja defini, pas de %%{version})
%global tar_version %{lua: print((rpm.expand("%{qrprotec_version}"):gsub("%^", "_")))}

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
Summary:        QR-code inventory kiosk: Django backend, Caddy proxy, Cage front

# QRProtec : pas de licence publiee. Embarques : Django, DRF, asgiref, sqlparse (BSD-3-Clause),
# Dear ImGui, qrcodegen (MIT), stb_image (MIT ou domaine public), jsQR (Apache-2.0),
# SDK Inateck (binaire proprietaire).
License:        LicenseRef-Proprietary AND BSD-3-Clause AND MIT AND Apache-2.0
URL:            https://github.com/etyloppihacilem/qrprotec
Source0:        %{name}-%{tar_version}.tar.gz
Source1:        %{name}-vendor-%{tar_version}.tar.gz
# Copie de packaging/files/qrprotec.sysusers, pour %%pre
Source2:        %{name}.sysusers

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
BuildRequires:  mesa-libGL-devel
%ifarch x86_64
BuildRequires:  libxdo-devel
%endif
%endif

Requires:       caddy
Requires:       python(abi) = %{python3_version}
# notifications web (Web Push) ; non embarque : module compile, fourni par Fedora
Requires:       python3-cryptography
Requires:       util-linux
# qrprotec-backup : archives .tar.xz
Requires:       tar
Requires:       xz
Requires(post): coreutils
Requires(post): systemd
Requires(preun): systemd
Requires(postun): systemd

Provides:       bundled(python3dist(django)) = %{django_version}
Provides:       bundled(python3dist(djangorestframework)) = %{drf_version}
Provides:       bundled(python3dist(asgiref)) = %{asgiref_version}
Provides:       bundled(python3dist(sqlparse)) = %{sqlparse_version}
Provides:       bundled(js-jsqr) = 1.4.0

%if %{with kiosk}
Requires:       cage
Requires:       dejavu-sans-fonts
# Charges par GLFW avec dlopen (pas de dependance automatique) : sans eux, GLFW ne peut pas
# ouvrir de fenetre Wayland et Cage reste sur un ecran noir
Requires:       libwayland-client
Requires:       libwayland-cursor
Requires:       libwayland-egl
Requires:       libxkbcommon
Requires:       mesa-libEGL
Requires:       mesa-dri-drivers
# Extinction de l'ecran, bip de mauvais scan, douchette Bluetooth
Recommends:     swayidle
Recommends:     wlopm
Recommends:     wlr-randr
Recommends:     alsa-utils
Recommends:     bluez
Provides:       bundled(imgui) = 1.93.0
Provides:       bundled(qrcodegen)
Provides:       bundled(stb_image) = 2.30
%endif

%description
QRProtec turns a minimal Fedora Server into a dedicated inventory kiosk for
first-aid equipment labeled with QR codes:
- the Django backend (public API and local API) runs as a hardened systemd
  service listening on 127.0.0.1 only;
- Caddy exposes the public API over HTTPS (ports 80 and 443);
- the ImGui front runs full screen in the Cage Wayland kiosk compositor
  on tty1.

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
install -Dpm 0755 packaging/files/qrprotec-setup %{buildroot}%{_sbindir}/qrprotec-setup
install -Dpm 0755 packaging/files/qrprotec-backup %{buildroot}%{_sbindir}/qrprotec-backup

# --- Configuration ---------------------------------------------------------------------------
install -Dpm 0640 packaging/files/qrprotec.conf %{buildroot}%{_sysconfdir}/%{name}/qrprotec.conf
install -Dpm 0644 packaging/files/backup.conf %{buildroot}%{_sysconfdir}/%{name}/backup.conf
install -Dpm 0644 packaging/files/qrprotec.caddyfile \
    %{buildroot}%{_sysconfdir}/caddy/Caddyfile.d/%{name}.caddyfile

# --- systemd ---------------------------------------------------------------------------------
install -Dpm 0644 packaging/files/qrprotec.service %{buildroot}%{_unitdir}/qrprotec.service
install -Dpm 0644 packaging/files/qrprotec-alerts.service %{buildroot}%{_unitdir}/qrprotec-alerts.service
install -Dpm 0644 packaging/files/qrprotec-alerts.timer %{buildroot}%{_unitdir}/qrprotec-alerts.timer
install -Dpm 0644 packaging/files/qrprotec-backup.service %{buildroot}%{_unitdir}/qrprotec-backup.service
sed -i 's|@SBINDIR@|%{_sbindir}|g' %{buildroot}%{_unitdir}/qrprotec-backup.service
install -Dpm 0644 packaging/files/qrprotec-backup.timer %{buildroot}%{_unitdir}/qrprotec-backup.timer
install -Dpm 0644 packaging/files/caddy-qrprotec.conf %{buildroot}%{_unitdir}/caddy.service.d/%{name}.conf
install -Dpm 0644 packaging/files/80-qrprotec.preset %{buildroot}%{_presetdir}/80-%{name}.preset
install -Dpm 0644 packaging/files/qrprotec.sysusers %{buildroot}%{_sysusersdir}/%{name}.conf
install -Dpm 0644 packaging/files/qrprotec.tmpfiles %{buildroot}%{_tmpfilesdir}/%{name}.conf
install -d -m 0750 %{buildroot}%{_sharedstatedir}/%{name}
install -d -m 0700 %{buildroot}%{_localstatedir}/backups/%{name}

%if %{with kiosk}
# --- Front (kiosk) ---------------------------------------------------------------------------
install -Dpm 0755 %{__cmake_builddir}/QRProtecApp %{buildroot}%{_libexecdir}/%{name}/qrprotec-front
install -Dpm 0755 packaging/files/qrprotec-kiosk-session \
    %{buildroot}%{_libexecdir}/%{name}/qrprotec-kiosk-session
sed -i -e 's|@LIBEXECDIR@|%{_libexecdir}|g' -e 's|@LIBDIR@|%{_libdir}|g' -e 's|@DATADIR@|%{_datadir}|g' \
    %{buildroot}%{_libexecdir}/%{name}/qrprotec-kiosk-session
%ifarch x86_64
install -Dpm 0755 app/scanner_lib/ble/linux/x86_64-unknown-linux-gnu/libinateck_scanner_ble.so \
    %{buildroot}%{_libdir}/%{name}/libinateck_scanner_ble.so
%endif
install -d %{buildroot}%{appdir}/templates
install -pm 0644 app/templates/*.qr %{buildroot}%{appdir}/templates/
install -d %{buildroot}%{appdir}/templates/images
install -pm 0644 app/templates/images/* %{buildroot}%{appdir}/templates/images/
install -Dpm 0644 packaging/files/kiosk.conf %{buildroot}%{_sysconfdir}/%{name}/kiosk.conf
install -Dpm 0644 packaging/files/qrprotec-kiosk.service %{buildroot}%{_unitdir}/qrprotec-kiosk.service
install -Dpm 0644 packaging/files/logind-qrprotec.conf \
    %{buildroot}%{_prefix}/lib/systemd/logind.conf.d/50-%{name}.conf
install -Dpm 0644 packaging/files/sleep-qrprotec.conf \
    %{buildroot}%{_prefix}/lib/systemd/sleep.conf.d/50-%{name}.conf
install -d -m 0750 %{buildroot}%{_sharedstatedir}/%{name}-kiosk
%endif

%check
%if %{with kiosk}
%ctest
%endif
PYTHONPATH=database:vendor QRPROTEC_SECRET_KEY=rpm-check QRPROTEC_DEBUG=0 \
    QRPROTEC_DB_PATH="$PWD/check.sqlite3" \
    %{python3} -s database/manage.py test inventory --noinput -v 1
rm -f check.sqlite3

%pre
# Utilisateurs crees avant la pose des fichiers, si rpm ne le fait pas lui-meme (sysusers natif)
%{?sysusers_create_compat:%sysusers_create_compat %{SOURCE2}}

%post
%systemd_post qrprotec.service qrprotec-alerts.timer qrprotec-backup.timer %{?with_kiosk:qrprotec-kiosk.service}
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
# SELinux : Caddy (domaine httpd_t) doit pouvoir joindre le back sur 127.0.0.1:8000
if command -v selinuxenabled >/dev/null 2>&1 && selinuxenabled; then
    if getsebool httpd_can_network_connect 2>/dev/null | grep -q -- '--> off'; then
        setsebool -P httpd_can_network_connect 1 || :
    fi
fi
if [ $1 -eq 1 ]; then
    # Premiere installation : Caddy active au demarrage, cible graphique (kiosk) par defaut
    systemctl --no-reload preset caddy.service >/dev/null 2>&1 || :
%if %{with kiosk}
    systemctl set-default graphical.target >/dev/null 2>&1 || :
%endif
    mkdir -p %{_localstatedir}/lib/rpm-state/%{name}
    touch %{_localstatedir}/lib/rpm-state/%{name}/first-install
elif [ ! -e %{_sysconfdir}/systemd/system/timers.target.wants/qrprotec-backup.timer ] && \
     ! grep -q '^QRPROTEC_BACKUP_SCHEDULE="\?off"\?$' %{_sysconfdir}/%{name}/backup.conf 2>/dev/null; then
    # Mise a jour depuis une version sans sauvegarde : %%systemd_post n'active les nouvelles
    # unites qu'a la premiere installation
    systemctl --no-reload preset qrprotec-backup.timer >/dev/null 2>&1 || :
    mkdir -p %{_localstatedir}/lib/rpm-state/%{name}
    touch %{_localstatedir}/lib/rpm-state/%{name}/start-backup-timer
fi

%preun
%systemd_preun qrprotec.service qrprotec-alerts.timer qrprotec-backup.timer %{?with_kiosk:qrprotec-kiosk.service}

%postun
%systemd_postun_with_restart qrprotec.service qrprotec-alerts.timer qrprotec-backup.timer %{?with_kiosk:qrprotec-kiosk.service}
if [ $1 -eq 0 ] && [ -d /run/systemd/system ]; then
    # Desinstallation : Caddy oublie le site QRProtec ; donnees, sauvegardes et configuration
    # modifiee restent
    rm -rf %{_sysconfdir}/systemd/system/qrprotec-backup.timer.d
    systemctl daemon-reload >/dev/null 2>&1 || :
    systemctl try-reload-or-restart caddy.service >/dev/null 2>&1 || :
%if %{with kiosk}
    if [ "$(systemctl get-default 2>/dev/null)" = graphical.target ] && \
       ! systemctl is-enabled --quiet display-manager.service 2>/dev/null; then
        systemctl set-default multi-user.target >/dev/null 2>&1 || :
    fi
    systemctl --no-block start getty@tty1.service >/dev/null 2>&1 || :
%endif
fi

%posttrans
if [ -d /run/systemd/system ]; then
    systemctl daemon-reload >/dev/null 2>&1 || :
    if [ -e %{_localstatedir}/lib/rpm-state/%{name}/first-install ]; then
        rm -f %{_localstatedir}/lib/rpm-state/%{name}/first-install
        systemctl start qrprotec.service qrprotec-alerts.timer qrprotec-backup.timer >/dev/null 2>&1 || :
        systemctl start caddy.service >/dev/null 2>&1 || :
%if %{with kiosk}
        systemctl --no-block start qrprotec-kiosk.service >/dev/null 2>&1 || :
%endif
    fi
    if [ -e %{_localstatedir}/lib/rpm-state/%{name}/start-backup-timer ]; then
        rm -f %{_localstatedir}/lib/rpm-state/%{name}/start-backup-timer
        systemctl start qrprotec-backup.timer >/dev/null 2>&1 || :
    fi
    # Installation ou mise a jour : Caddy relit le fragment et l'environnement
    systemctl try-reload-or-restart caddy.service >/dev/null 2>&1 || :
fi

%files
%license app/third_party/qrcodegen/LICENSE database/inventory/web/vendor/jsQR-LICENSE
%doc README.md
%{_bindir}/qrprotec-manage
%{_sbindir}/qrprotec-setup
%{_sbindir}/qrprotec-backup
%{appdir}/
%dir %{_sysconfdir}/%{name}
%config(noreplace) %attr(0640,root,qrprotec) %{_sysconfdir}/%{name}/qrprotec.conf
%config(noreplace) %{_sysconfdir}/%{name}/backup.conf
%dir %{_sysconfdir}/caddy/Caddyfile.d
%config(noreplace) %{_sysconfdir}/caddy/Caddyfile.d/%{name}.caddyfile
%{_unitdir}/qrprotec.service
%{_unitdir}/qrprotec-alerts.service
%{_unitdir}/qrprotec-alerts.timer
%{_unitdir}/qrprotec-backup.service
%{_unitdir}/qrprotec-backup.timer
%dir %{_unitdir}/caddy.service.d
%{_unitdir}/caddy.service.d/%{name}.conf
%{_presetdir}/80-%{name}.preset
%{_sysusersdir}/%{name}.conf
%{_tmpfilesdir}/%{name}.conf
%dir %attr(0750,qrprotec,qrprotec) %{_sharedstatedir}/%{name}
%dir %attr(0700,root,root) %{_localstatedir}/backups/%{name}
%if %{with kiosk}
%license app/imgui/LICENSE.txt
%dir %{_libexecdir}/%{name}
%{_libexecdir}/%{name}/qrprotec-front
%{_libexecdir}/%{name}/qrprotec-kiosk-session
%ifarch x86_64
%{_libdir}/%{name}/
%endif
%config(noreplace) %{_sysconfdir}/%{name}/kiosk.conf
%{_unitdir}/qrprotec-kiosk.service
%dir %{_prefix}/lib/systemd/logind.conf.d
%{_prefix}/lib/systemd/logind.conf.d/50-%{name}.conf
%dir %{_prefix}/lib/systemd/sleep.conf.d
%{_prefix}/lib/systemd/sleep.conf.d/50-%{name}.conf
%dir %attr(0750,qrprotec-kiosk,qrprotec-kiosk) %{_sharedstatedir}/%{name}-kiosk
%endif

%changelog
* %{lua: print(os.date("!%a %b %d %Y"))} Hippolyte Melica <hippolytemelica@gmail.com> - %{version}-%{release}
- Build from git, see the repository history for details
