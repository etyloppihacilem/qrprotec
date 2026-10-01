# Construction du RPM de la borne QRProtec.
#
#   make rpm                     RPM + SRPM dans dist/, construits dans un conteneur Fedora
#   make rpm FEDORA_VERSION=42   pour une autre version de Fedora
#   make rpm-local               sur une machine Fedora, sans conteneur
#   make sources                 archives des sources seulement (dist/sources)
#   make lint                    shellcheck + rpmlint sur le spec
#   make clean

FEDORA_VERSION ?= 43
CONTAINER_ENGINE ?= $(shell command -v podman 2>/dev/null || command -v docker 2>/dev/null)
IMAGE ?= qrprotec-rpmbuild:f$(FEDORA_VERSION)
RPMBUILD_ARGS ?=
SHELL_SCRIPTS := bootstrap.sh packaging/*.sh packaging/files/qrprotec-manage \
                 packaging/files/qrprotec-setup packaging/files/qrprotec-kiosk-session

.PHONY: rpm rpm-local image sources lint clean version

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

clean:
	rm -rf dist
