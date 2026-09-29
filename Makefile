SHELL := /bin/sh

WORK := $(CURDIR)/.work
SIPD_BUILD := $(WORK)/build/sipd
EMULATOR_STATE := $(WORK)/cache/emulator-run/synthetic-state.json
PAYLOAD := $(WORK)/dist/aula-ti8168-sip-endpoint/runtime
QEMU_WORK := $(WORK)/build/qemu
QEMU_BINARY := $(QEMU_WORK)/build-aula-v11.0.3/qemu-system-arm
export PYTHONPYCACHEPREFIX := $(WORK)/cache/python

.PHONY: build verify sipd-build emulator-status console-device \
	console-fastcgi ui-install ui-build package-ti8168 package-qemu \
	qemu-build qemu-verify qemu-run qemu-acceptance live-build \
	live-package live-preflight live-install live-gates live-smoke \
	live-soak-15 live-soak-30 live-soak-60 live-reboot live-remove

build: sipd-build console-device ui-build

verify: build emulator-status
	@echo "Maintained host build and synthetic emulator entry point verified."

sipd-build:
	@mkdir -p $(SIPD_BUILD)
	cmake -S product/sipd -B $(SIPD_BUILD) -DCMAKE_BUILD_TYPE=Release
	cmake --build $(SIPD_BUILD)

emulator-status:
	@mkdir -p $(dir $(EMULATOR_STATE))
	PYTHONPATH=lab/emulator/src python3 -m aula_emulator status \
		--state-file $(EMULATOR_STATE)

console-device:
	$(MAKE) -C product/console device-control

console-fastcgi:
	@test -n "$(FCGI_PREFIX)" || { echo "set FCGI_PREFIX to a vetted FastCGI 2.4.7 installation" >&2; exit 2; }
	$(MAKE) -C product/console fastcgi FCGI_PREFIX="$(FCGI_PREFIX)"

ui-install:
	$(MAKE) -C product/console ui-install

ui-build:
	$(MAKE) -C product/console ui-build

package-ti8168:
	@test -n "$(SIPD_BINARY)" -a -n "$(GATEWAY_BINARY)" -a -n "$(NGINX_BINARY)" \
		-a -n "$(DEVICE_BINARY)" -a -n "$(ATOMIC_REPLACE_BINARY)" -a -n "$(MIME_TYPES)" -a -n "$(FASTCGI_PARAMS)" || \
		{ echo "set SIPD_BINARY, GATEWAY_BINARY, DEVICE_BINARY, NGINX_BINARY, ATOMIC_REPLACE_BINARY, MIME_TYPES, and FASTCGI_PARAMS" >&2; exit 2; }
	@mkdir -p $(WORK)/dist/aula-ti8168-sip-endpoint
	python3 deployment/payload/build-payload.py \
		--sipd "$(SIPD_BINARY)" --gateway "$(GATEWAY_BINARY)" --device "$(DEVICE_BINARY)" \
		--nginx "$(NGINX_BINARY)" --atomic-replace "$(ATOMIC_REPLACE_BINARY)" \
		--ui-dist $(WORK)/dist/console-web \
		--nginx-config product/console/nginx/nginx.conf \
		--mime-types "$(MIME_TYPES)" --fastcgi-params "$(FASTCGI_PARAMS)" \
		--sip-config product/sipd/config/aula-sipd.example.conf \
		--gateway-config product/console/gateway/config/aula-console.example.conf \
		--output $(PAYLOAD)

package-qemu:
	@echo "QEMU payload packaging is unavailable for the synthetic machine model" >&2; exit 2

qemu-build:
	sh lab/qemu/scripts/fetch-qemu.sh
	sh lab/qemu/scripts/prepare-qemu.sh
	sh lab/qemu/scripts/build.sh

qemu-verify: qemu-build
	QEMU_BINARY=$(QEMU_BINARY) sh lab/qemu/scripts/verify.sh --require-source

qemu-run:
	@echo "Supply a reviewed ELF or raw image to the synthetic QEMU model explicitly" >&2; exit 2

qemu-acceptance:
	@echo "Product acceptance is unavailable in the synthetic QEMU lane" >&2; exit 2

live-build:
	@test -n "$${LIVE_BUILD_MANIFEST:-}" || { echo "set LIVE_BUILD_MANIFEST to an absolute hash-reviewed input manifest" >&2; exit 2; }
	python3 -B tooling/live/build.py --manifest "$${LIVE_BUILD_MANIFEST}"

live-package:
	python3 -B tooling/live/package.py

live-gates:
	@test -n "$${QEMU_ENTROPY_HELPER:-}" || { echo "set QEMU_ENTROPY_HELPER to an absolute reviewed ARM helper" >&2; exit 2; }
	python3 -B tooling/live/gates.py --run --entropy-helper "$${QEMU_ENTROPY_HELPER}"

live-preflight:
	python3 -B tooling/live/campaign.py preflight

live-install:
	python3 -B tooling/live/campaign.py install

live-smoke:
	python3 -B tooling/live/campaign.py smoke

live-soak-15:
	python3 -B tooling/live/campaign.py soak-15

live-soak-30:
	python3 -B tooling/live/campaign.py soak-30

live-soak-60:
	python3 -B tooling/live/campaign.py soak-60

live-reboot:
	python3 -B tooling/live/campaign.py reboot

live-remove:
	python3 -B tooling/live/campaign.py remove
