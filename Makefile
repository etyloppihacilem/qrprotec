# Construction des RPM de QRProtec (qrprotec, qrprotec-server, qrprotec-kiosk, qrprotec-front,
# qrprotec-common).
#
#   make rpm                     RPM + SRPM dans dist/, construits dans un conteneur Fedora
#   make rpm FEDORA_VERSION=42   pour une autre version de Fedora
#   make rpm-local               sur une machine Fedora, sans conteneur
#   make sources                 archives des sources seulement (dist/sources)
#   make lint                    shellcheck + rpmlint sur le spec
#   make icon ICON=image.png     icone du site web et de l'application (ImageMagick)
#   make doc                     documentation en PDF A4 a imprimer dans dist/doc (pandoc + weasyprint)
#   make flatpak                 front seul (application de bureau) en Flatpak : dist/QRProtec-front.flatpak
#   make clean

FEDORA_VERSION ?= 43
CONTAINER_ENGINE ?= $(shell command -v podman 2>/dev/null || command -v docker 2>/dev/null)
IMAGE ?= qrprotec-rpmbuild:f$(FEDORA_VERSION)
RPMBUILD_ARGS ?=
SHELL_SCRIPTS := bootstrap.sh packaging/*.sh packaging/files/qrprotec-manage \
                 packaging/files/qrprotec-setup packaging/files/qrprotec-backup \
                 packaging/files/qrprotec-kiosk-session packaging/files/qrprotec-front

.PHONY: rpm rpm-local image sources lint clean version icon doc flatpak

# Proprietaire de dist/ a la fin de la construction, vu depuis le conteneur. En podman rootless
# (defaut sur Fedora), root du conteneur est deja l'utilisateur courant : chown vers son uid le
# donnerait a un uid subordonne (100999...), que seul root peut ensuite supprimer. Idem pour docker
# rootless. Sous sudo, on rend dist/ a l'utilisateur qui a lance sudo.
# (Evaluation paresseuse : `podman info` n'est lance que pour `make rpm`.)
ROOTLESS = $(shell { $(CONTAINER_ENGINE) info --format '{{.Host.Security.Rootless}}' 2>/dev/null; \
               $(CONTAINER_ENGINE) info --format '{{.SecurityOptions}}' 2>/dev/null | grep -q rootless \
               && echo true; } | grep -qx true && echo true)
DIST_OWNER ?= $(if $(ROOTLESS),0:0,$(or $(SUDO_UID),$(shell id -u)):$(or $(SUDO_GID),$(shell id -g)))

rpm: image
	$(CONTAINER_ENGINE) run --rm \
		-v "$(CURDIR):/src:Z" \
		-e RPMBUILD_ARGS="$(RPMBUILD_ARGS)" \
		-e QRPROTEC_VERSION="$(QRPROTEC_VERSION)" \
		$(IMAGE) \
		bash -c 'packaging/build-rpm.sh; status=$$?; chown -R $(DIST_OWNER) dist; exit $$status'

image:
	@test -n "$(CONTAINER_ENGINE)" || { echo "podman ou docker requis (ou : make rpm-local)"; exit 1; }
	$(CONTAINER_ENGINE) build --build-arg FEDORA_VERSION=$(FEDORA_VERSION) \
		-t $(IMAGE) -f packaging/Containerfile packaging

rpm-local:
	RPMBUILD_ARGS="$(RPMBUILD_ARGS)" packaging/build-rpm.sh

sources:
	packaging/make-sources.sh

version:
	@packaging/version.sh

lint:
	shellcheck $(SHELL_SCRIPTS)
	rpmlint -c packaging/rpmlint.toml packaging/qrprotec.spec

# Icone du site (onglet, ecran d'accueil, notifications) et de l'application, a partir d'une seule
# image (PNG ou JPEG, carree de preference). Le logo des etiquettes est a part :
# app/templates/images/protection-civile.png.
# Fond blanc autour du logo (coins arrondis) : transparent pour l'icone « any », remplace par ICON_BG pour les
# icones plein cadre (maskable d'Android, que le lanceur decoupe en rond, et apple-touch-icon, arrondie par iOS).
# Le maskable reduit le logo a ICON_SAFE pour qu'il tienne dans la zone sure (cercle de 80 %).
WEB_DIR := database/inventory/web
MAGICK ?= $(shell command -v magick 2>/dev/null || command -v convert 2>/dev/null)
ICON_BG ?= \#1f2a33
ICON_SAFE ?= 82%
ICON_CORNERS := -draw "color 0,0 floodfill" -draw "color 511,0 floodfill" -draw "color 0,511 floodfill" \
	-draw "color 511,511 floodfill"

icon:
	@test -n "$(ICON)" || { echo "usage : make icon ICON=chemin/vers/image.png"; exit 1; }
	@test -n "$(MAGICK)" || { echo "ImageMagick (magick ou convert) requis"; exit 1; }
	$(MAGICK) "$(ICON)" -background white -alpha remove -alpha off -resize 512x512 -gravity center -extent 512x512 \
		-depth 8 -strip $(WEB_DIR)/icon-source.png
	$(MAGICK) $(WEB_DIR)/icon-source.png -fuzz 45% -fill "$(ICON_BG)" $(ICON_CORNERS) $(WEB_DIR)/icon-full.png
	$(MAGICK) $(WEB_DIR)/icon-source.png -alpha set -fuzz 45% -fill none $(ICON_CORNERS) \
		-depth 8 -strip $(WEB_DIR)/icon-512.png
	$(MAGICK) $(WEB_DIR)/icon-512.png -resize 192x192 -depth 8 -strip $(WEB_DIR)/icon-192.png
	$(MAGICK) $(WEB_DIR)/icon-512.png -resize 32x32 -depth 8 -strip $(WEB_DIR)/favicon.png
	$(MAGICK) $(WEB_DIR)/icon-full.png -resize $(ICON_SAFE) -background "$(ICON_BG)" -gravity center -extent 512x512 \
		-alpha off -depth 8 -strip $(WEB_DIR)/icon-maskable-512.png
	$(MAGICK) $(WEB_DIR)/icon-maskable-512.png -resize 192x192 -depth 8 -strip $(WEB_DIR)/icon-maskable-192.png
	$(MAGICK) $(WEB_DIR)/icon-full.png -resize 180x180 -alpha off -depth 8 -strip $(WEB_DIR)/apple-touch-icon.png
	rm -f $(WEB_DIR)/icon-source.png $(WEB_DIR)/icon-full.png

# Documentation imprimable (A4) : un PDF complet et un PDF par partie, dans dist/doc.
# Dependances : sudo dnf install pandoc weasyprint google-noto-color-emoji-fonts
# L'ordre des pages suit les sommaires de docs/*/README.md.
PANDOC ?= pandoc
# make doc WEASYPRINT_FLAGS=--debug : detail d'une erreur de weasyprint
WEASYPRINT_FLAGS ?=
DOC_DIR := dist/doc
DOC_DEPS := $(wildcard docs/*.md docs/*/*.md) docs/pdf/assemble.lua docs/pdf/template.html docs/pdf/print.css
DOC_UTILISATEUR := $(addprefix docs/utilisateur/,README.md notions.md mise-en-place.md verifier-un-lot.md \
                   gerer-le-stock.md administration.md depannage.md)
DOC_INSTALLATION := $(addprefix docs/installation/,README.md borne-complete.md back-seul.md front-seul.md \
                    exploitation.md)
DOC_TECHNIQUE := $(addprefix docs/technique/,README.md architecture.md donnees.md regles-de-gestion.md back.md websockets.md \
                 front-imgui.md front-web.md packaging.md decisions.md maintenance.md)
DOC_PDFS := $(addprefix $(DOC_DIR)/,qrprotec-documentation.pdf qrprotec-utilisateur.pdf \
            qrprotec-installation.pdf qrprotec-technique.pdf)

doc: $(DOC_PDFS)

$(DOC_DIR)/qrprotec-documentation.pdf: DOC_PAGES = docs/README.md $(DOC_UTILISATEUR) $(DOC_INSTALLATION) $(DOC_TECHNIQUE)
$(DOC_DIR)/qrprotec-documentation.pdf: DOC_SUBTITLE = Documentation
$(DOC_DIR)/qrprotec-utilisateur.pdf: DOC_PAGES = $(DOC_UTILISATEUR)
$(DOC_DIR)/qrprotec-utilisateur.pdf: DOC_SUBTITLE = Guide de l'utilisateur
$(DOC_DIR)/qrprotec-installation.pdf: DOC_PAGES = $(DOC_INSTALLATION)
$(DOC_DIR)/qrprotec-installation.pdf: DOC_SUBTITLE = Guide d'installation
$(DOC_DIR)/qrprotec-technique.pdf: DOC_PAGES = $(DOC_TECHNIQUE)
$(DOC_DIR)/qrprotec-technique.pdf: DOC_SUBTITLE = Documentation technique

$(DOC_PDFS): $(DOC_DEPS)
	@command -v $(PANDOC) >/dev/null || { echo "pandoc requis (sudo dnf install pandoc)"; exit 1; }
	@command -v weasyprint >/dev/null || { echo "weasyprint requis (sudo dnf install weasyprint)"; exit 1; }
	@fc-list 2>/dev/null | grep -q "Noto Color Emoji" || echo "attention : police Noto Color Emoji absente, emojis vides dans le PDF (sudo dnf install google-noto-color-emoji-fonts)"
	@mkdir -p $(DOC_DIR)
	$(PANDOC) -f markdown -t html5 -s /dev/null -o $(@:.pdf=.html) \
		--lua-filter docs/pdf/assemble.lua \
		$(foreach page,$(DOC_PAGES),-M pages=$(page)) \
		-M title=QRProtec -M "subtitle=$(DOC_SUBTITLE)" -M lang=fr \
		-M "version=$$(packaging/version.sh)" -M "date=$$(date +%d/%m/%Y)" \
		--template docs/pdf/template.html --css "$(CURDIR)/docs/pdf/print.css" \
		--toc --toc-depth=2
	@# weasyprint lance a part (pas --pdf-engine) : pandoc cache son erreur derriere « Error producing PDF. »
	weasyprint $(WEASYPRINT_FLAGS) $(@:.pdf=.html) $@

# Flatpak du front (pas de kiosk), avec le runtime Flathub installe pour l'utilisateur au besoin.
# Dependances : sudo dnf install flatpak flatpak-builder
# --disable-rofiles-fuse : fusermount3 refuse de monter dans dist/ sur certaines machines (Fedora Atomic,
# SELinux), « Failure spawning rofiles-fuse ». Ce montage ne sert qu a proteger le cache.
FLATPAK_ID := io.github.etyloppihacilem.QRProtec
FLATPAK_MANIFEST := packaging/flatpak/$(FLATPAK_ID).yml
FLATHUB := https://dl.flathub.org/repo/flathub.flatpakrepo

flatpak:
	@command -v flatpak-builder >/dev/null || { echo "flatpak-builder requis (sudo dnf install flatpak-builder)"; exit 1; }
	git submodule update --init app/imgui app/implot app/scanner_lib
	flatpak remote-add --user --if-not-exists flathub $(FLATHUB)
	flatpak-builder --user --install-deps-from=flathub --force-clean --ccache --disable-rofiles-fuse \
		--state-dir=dist/flatpak/state --repo=dist/flatpak/repo dist/flatpak/build $(FLATPAK_MANIFEST)
	flatpak build-bundle --runtime-repo=$(FLATHUB) dist/flatpak/repo dist/QRProtec-front.flatpak $(FLATPAK_ID)

clean:
	rm -rf dist
