SHELL := /bin/sh

WORK := $(CURDIR)/.work
SIPD_BUILD := $(WORK)/build/sipd
SIPD_HOST_CONFIG := $(WORK)/config/aula-sipd.conf
SIPD_HOST_RUN := $(WORK)/run/aula-sipd
PAYLOAD := $(WORK)/dist/aula-ti8168-sip-endpoint/runtime
QEMU_WORK := $(WORK)/build/qemu
QEMU_BINARY := $(QEMU_WORK)/build-aula-v11.0.3/qemu-system-arm
export PYTHONPYCACHEPREFIX := $(WORK)/cache/python

# The console sub-makes (device, gateway, test) compile the same object files
# under .work/build/console, so targets must not run concurrently under -j.
.NOTPARALLEL:

.PHONY: build verify test sipd-build sipd-test sipd-host-config sipd-pjsip emulator-status emulator-test \
	emulator-ui emulator-ui-status emulator-ui-stop emulator-source-check \
	console-device console-gateway console-test console-fastcgi-host \
	console-fastcgi ui-install ui-build ui-test package-ti8168 package-qemu \
	qemu-build qemu-verify qemu-run qemu-acceptance qemu-input-test arm-userspace-test live-build \
	live-package live-preflight live-install live-gates live-smoke \
	live-soak-15 live-soak-30 live-soak-60 live-reboot live-remove

build: sipd-build console-device console-gateway ui-build

verify: build test sipd-host-config emulator-status
	@echo "Maintained host build, unit tests, and synthetic emulator entry point verified."

test: sipd-test console-test emulator-test ui-test arm-userspace-test qemu-input-test emulator-source-check

arm-userspace-test:
	python3 -B -m unittest discover -s lab/arm-userspace/tests

qemu-input-test:
	python3 -B -m unittest discover -s lab/qemu/firmware/tests

emulator-source-check:
	python3 -B -m unittest discover -s lab/qemu/tests
	python3 -B lab/qemu/scripts/check_public_sources.py

sipd-build:
	@mkdir -p $(SIPD_BUILD)
	cmake -S product/sipd -B $(SIPD_BUILD) -DCMAKE_BUILD_TYPE=Release
	cmake --build $(SIPD_BUILD)

# Opt-in: downloads and builds the pinned pjproject (digest-checked) and
# links a daemon that can run --foreground. Never part of build or verify.
sipd-pjsip:
	sh dependencies/scripts/build-pjproject-host.sh
	cmake -C $(WORK)/build/pjproject/aula-sipd-pjsip.cmake -S product/sipd \
		-B $(WORK)/build/sipd-pjsip -DCMAKE_BUILD_TYPE=Release
	cmake --build $(WORK)/build/sipd-pjsip
	@echo "PJSIP-linked daemon: $(WORK)/build/sipd-pjsip/aula-sipd"

sipd-test: sipd-build
	cd $(SIPD_BUILD) && ctest --output-on-failure

# The device example names /run and /var/lib/aula-sipd, which a developer
# host lacks; the committed host example moves those two paths to
# /tmp/aula-sipd. Check that the host example differs from the device example
# only there (and in its "# host:" comments), then validate a copy whose
# private directory lives below .work.
SIPD_EXAMPLE := product/sipd/config/aula-sipd.example.conf
SIPD_HOST_EXAMPLE := product/sipd/config/aula-sipd.host.example.conf

# The run directory reaches the rewrite through the environment so any path
# character survives (a sed replacement would need escaping).
sipd-host-config: export SIPD_HOST_RUN := $(SIPD_HOST_RUN)
sipd-host-config: sipd-build
	@mkdir -p $(dir $(SIPD_HOST_CONFIG)) $(SIPD_HOST_RUN)
	@chmod 700 $(dir $(SIPD_HOST_CONFIG)) $(SIPD_HOST_RUN)
	@sed -e 's|^pid_file = .*|pid_file = /tmp/aula-sipd/aula-sipd.pid|' \
		-e 's|^settings_file = .*|settings_file = /tmp/aula-sipd/settings.json|' \
		$(SIPD_EXAMPLE) > $(SIPD_HOST_CONFIG).expected
	@grep -v '^# host:' $(SIPD_HOST_EXAMPLE) | cmp -s - $(SIPD_HOST_CONFIG).expected || \
		{ echo "$(SIPD_HOST_EXAMPLE) has drifted from $(SIPD_EXAMPLE)" >&2; exit 1; }
	python3 -c 'import os, sys; run = os.environ["SIPD_HOST_RUN"].encode() + b"/"; sys.stdout.buffer.write(b"".join(line.replace(b"/tmp/aula-sipd/", run, 1) for line in open(sys.argv[1], "rb")))' \
		$(SIPD_HOST_EXAMPLE) > $(SIPD_HOST_CONFIG).tmp
	@chmod 600 $(SIPD_HOST_CONFIG).tmp
	@mv -f $(SIPD_HOST_CONFIG).tmp $(SIPD_HOST_CONFIG)
	$(SIPD_BUILD)/aula-sipd --check-config $(SIPD_HOST_CONFIG)

emulator-status:
	PYTHONPATH=lab/emulator/src python3 lab/emulator/tests/smoke_cli.py

emulator-test:
	PYTHONPATH=lab/emulator/src python3 -m unittest discover \
		-s lab/emulator/tests -t lab/emulator

# Opt-in local HTTPS console: pinned native dependencies, real gateway and
# synthetic providers. Generated configuration and credentials stay in .work.
emulator-ui:
	python3 -B lab/emulator/examples/console.py start $(EMULATOR_UI_ARGS)

emulator-ui-status:
	python3 -B lab/emulator/examples/console.py status

emulator-ui-stop:
	python3 -B lab/emulator/examples/console.py stop

console-device:
	$(MAKE) -C product/console device-control

console-gateway:
	$(MAKE) -C product/console gateway-check

console-test:
	$(MAKE) -C product/console test

# Opt-in: downloads and builds the pinned, digest-checked FastCGI 2.4.7
# library below .work, then links the real FastCGI gateway against it.
console-fastcgi-host:
	sh dependencies/scripts/build-fastcgi-host.sh
	$(MAKE) -C product/console fastcgi FCGI_PREFIX="$(WORK)/build/fastcgi/prefix"
	@echo "FastCGI gateway: $(WORK)/build/console/aula-gateway-fcgi"

console-fastcgi:
	@test -n "$(FCGI_PREFIX)" || { echo "set FCGI_PREFIX to a vetted FastCGI 2.4.7 installation" >&2; exit 2; }
	$(MAKE) -C product/console fastcgi FCGI_PREFIX="$(FCGI_PREFIX)"

ui-install:
	$(MAKE) -C product/console ui-install

ui-build:
	$(MAKE) -C product/console ui-build

ui-test:
	$(MAKE) -C product/console ui-test

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
	QEMU_BINARY=$(QEMU_BINARY) sh lab/qemu/scripts/verify.sh --require-runtime

qemu-run: qemu-build
	python3 -B lab/qemu/scripts/probe_guest.py $(QEMU_BINARY) \
		--assembler "$${QEMU_ARM_AS:-arm-none-eabi-as}" \
		--linker "$${QEMU_ARM_LD:-arm-none-eabi-ld}"

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
