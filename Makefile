# Construction des RPM de QRProtec (qrprotec, qrprotec-server, qrprotec-kiosk, qrprotec-front,
# qrprotec-common).
#
#   make rpm                     RPM + SRPM dans dist/, construits dans un conteneur Fedora
#   make rpm FEDORA_VERSION=42   pour une autre version de Fedora
#   make rpm-local               sur une machine Fedora, sans conteneur
#   make sources                 archives des sources seulement (dist/sources)
#   make lint                    shellcheck + rpmlint sur le spec
#   make icon ICON=image.png     icone du site web + logo des etiquettes (ImageMagick)
#   make doc                     documentation en PDF A4 a imprimer dans dist/doc (pandoc + weasyprint)
#   make clean

FEDORA_VERSION ?= 43
CONTAINER_ENGINE ?= $(shell command -v podman 2>/dev/null || command -v docker 2>/dev/null)
IMAGE ?= qrprotec-rpmbuild:f$(FEDORA_VERSION)
RPMBUILD_ARGS ?=
SHELL_SCRIPTS := bootstrap.sh packaging/*.sh packaging/files/qrprotec-manage \
                 packaging/files/qrprotec-setup packaging/files/qrprotec-backup \
                 packaging/files/qrprotec-kiosk-session packaging/files/qrprotec-front

.PHONY: rpm rpm-local image sources lint clean version icon doc

rpm: image
	$(CONTAINER_ENGINE) run --rm \
		-v "$(CURDIR):/src:Z" \
		-e RPMBUILD_ARGS="$(RPMBUILD_ARGS)" \
		-e QRPROTEC_VERSION="$(QRPROTEC_VERSION)" \
		$(IMAGE) \
		bash -c 'packaging/build-rpm.sh; status=$$?; chown -R $(shell id -u):$(shell id -g) dist; exit $$status'

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

# Icone du site (onglet, ecran d'accueil, notifications) et image « protec.png » des etiquettes,
# a partir d'une seule image (PNG ou JPEG, carree de preference).
WEB_DIR := database/inventory/web
LABEL_IMAGES_DIR := app/templates/images
MAGICK ?= $(shell command -v magick 2>/dev/null || command -v convert 2>/dev/null)

icon:
	@test -n "$(ICON)" || { echo "usage : make icon ICON=chemin/vers/image.png"; exit 1; }
	@test -n "$(MAGICK)" || { echo "ImageMagick (magick ou convert) requis"; exit 1; }
	$(MAGICK) "$(ICON)" -background white -alpha remove -alpha off -resize 512x512 -gravity center -extent 512x512 \
		-depth 8 -strip $(LABEL_IMAGES_DIR)/protec.png
	$(MAGICK) $(LABEL_IMAGES_DIR)/protec.png -resize 512x512 -depth 8 -strip $(WEB_DIR)/icon-512.png
	$(MAGICK) $(LABEL_IMAGES_DIR)/protec.png -resize 192x192 -depth 8 -strip $(WEB_DIR)/icon-192.png
	$(MAGICK) $(LABEL_IMAGES_DIR)/protec.png -resize 32x32 -depth 8 -strip $(WEB_DIR)/favicon.png

# Documentation imprimable (A4) : un PDF complet et un PDF par partie, dans dist/doc.
# Dependances : sudo dnf install pandoc weasyprint
# L'ordre des pages suit les sommaires de docs/*/README.md.
PANDOC ?= pandoc
DOC_DIR := dist/doc
DOC_DEPS := $(wildcard docs/*.md docs/*/*.md) docs/pdf/assemble.lua docs/pdf/template.html docs/pdf/print.css
DOC_UTILISATEUR := $(addprefix docs/utilisateur/,README.md notions.md mise-en-place.md verifier-un-lot.md \
                   gerer-le-stock.md administration.md depannage.md)
DOC_INSTALLATION := $(addprefix docs/installation/,README.md borne-complete.md back-seul.md front-seul.md \
                    exploitation.md)
DOC_TECHNIQUE := $(addprefix docs/technique/,README.md architecture.md donnees.md regles-de-gestion.md back.md \
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
	@mkdir -p $(DOC_DIR)
	$(PANDOC) -f markdown -t html5 /dev/null -o $@ \
		--lua-filter docs/pdf/assemble.lua \
		$(foreach page,$(DOC_PAGES),-M pages=$(page)) \
		-M title=QRProtec -M "subtitle=$(DOC_SUBTITLE)" -M lang=fr \
		-M "version=$$(packaging/version.sh)" -M "date=$$(date +%d/%m/%Y)" \
		--template docs/pdf/template.html --css "$(CURDIR)/docs/pdf/print.css" \
		--toc --toc-depth=2 --pdf-engine=weasyprint

clean:
	rm -rf dist
