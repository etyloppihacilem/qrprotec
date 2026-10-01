# Construction des RPM de QRProtec (qrprotec, qrprotec-server, qrprotec-kiosk, qrprotec-front,
# qrprotec-common).
#
#   make rpm                     RPM + SRPM dans dist/, construits dans un conteneur Fedora
#   make rpm FEDORA_VERSION=42   pour une autre version de Fedora
#   make rpm-local               sur une machine Fedora, sans conteneur
#   make sources                 archives des sources seulement (dist/sources)
#   make lint                    shellcheck + rpmlint sur le spec
#   make icon ICON=image.png     icone du site web + logo des etiquettes (ImageMagick)
#   make clean

FEDORA_VERSION ?= 43
CONTAINER_ENGINE ?= $(shell command -v podman 2>/dev/null || command -v docker 2>/dev/null)
IMAGE ?= qrprotec-rpmbuild:f$(FEDORA_VERSION)
RPMBUILD_ARGS ?=
SHELL_SCRIPTS := bootstrap.sh packaging/*.sh packaging/files/qrprotec-manage \
                 packaging/files/qrprotec-setup packaging/files/qrprotec-backup \
                 packaging/files/qrprotec-kiosk-session packaging/files/qrprotec-front

.PHONY: rpm rpm-local image sources lint clean version icon

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

clean:
	rm -rf dist
