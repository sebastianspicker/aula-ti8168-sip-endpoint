SHELL := /bin/sh

WORK := $(CURDIR)/.work
UV_CACHE := $(WORK)/cache/uv
EMULATOR_ENV := $(WORK)/cache/emulator-venv
QUALITY_ENV := $(WORK)/cache/quality-venv
PAYLOAD := $(WORK)/dist/aula-ti8168-sip-endpoint/runtime
QEMU_OVERLAY := $(WORK)/dist/qemu/aula-ti8168-sip-endpoint-overlay
QEMU_WORK := $(WORK)/build/qemu
QEMU_BINARY := $(QEMU_WORK)/build-aula-v11.0.3/qemu-system-arm
TRASH_BUNDLE ?= $(HOME)/.Trash/aula-ti8168-sip-endpoint-clean-$(shell date -u +%Y%m%dT%H%M%SZ)
export PYTHONPYCACHEPREFIX := $(WORK)/cache/python
export PYTEST_ADDOPTS := -p no:cacheprovider
export UV_NO_EDITABLE := 1
# Lab pytest suites reuse the emulator's synced dev environment (lab/emulator
# `setup`) without building the emulator package into its source tree.
LAB_PYTEST = UV_CACHE_DIR=$(UV_CACHE) UV_PROJECT_ENVIRONMENT=$(EMULATOR_ENV) uv run --project lab/emulator --no-sync pytest -q
export UV_OFFLINE := 1

.PHONY: verify check-layout quality test-product test-lab test-deployment \
	package-qemu package-ti8168 qemu-build qemu-verify qemu-run \
	qemu-acceptance live-build live-package live-preflight live-install \
	live-gates live-smoke live-soak-15 live-soak-30 live-soak-60 live-reboot live-remove clean

verify: check-layout check-source-boundary quality test-product-core test-lab-core test-deployment
	@echo "Host baseline passed; native features and rebuilt QEMU model require verify-native and verify-qemu-model."

.PHONY: verify-native quality-qemu-model verify-qemu-model verify-all verify-evidence
verify-native:
	@test -n "$(NATIVE_INPUTS)" || { echo "native lane unavailable: set NATIVE_INPUTS to reviewed native dependency-path JSON" >&2; exit 2; }
	python3 -B tooling/quality/native.py --inputs "$(NATIVE_INPUTS)"

quality-qemu-model:
	@test -n "$(QEMU_BASE_SOURCE)" || { echo "QEMU model lane unavailable: set QEMU_BASE_SOURCE to the reviewed local pinned checkout" >&2; exit 2; }
	UV_CACHE_DIR=$(UV_CACHE) UV_PROJECT_ENVIRONMENT=$(QUALITY_ENV) uv run --offline --project tooling/quality --locked python tooling/quality/qemu_model.py --source "$(QEMU_BASE_SOURCE)"

verify-qemu-model:
	@test -n "$(QEMU_BASE_SOURCE)" || { echo "QEMU model lane unavailable: set QEMU_BASE_SOURCE to the reviewed local pinned checkout" >&2; exit 2; }
	UV_CACHE_DIR=$(UV_CACHE) UV_PROJECT_ENVIRONMENT=$(QUALITY_ENV) uv run --offline --project tooling/quality --locked python tooling/quality/qemu_model.py --source "$(QEMU_BASE_SOURCE)" --verify $(if $(QEMU_NINJA),--ninja "$(QEMU_NINJA)") $(if $(QEMU_PYTHON),--python "$(QEMU_PYTHON)") $(if $(QEMU_SUBPROJECT_SOURCE),--subproject-source "$(QEMU_SUBPROJECT_SOURCE)")

verify-all: verify verify-native verify-qemu-model verify-evidence

verify-evidence:
	@test -d evidence/firmware-analysis/reverse_engineering/tools/tests || \
		{ echo "evidence lane unavailable: the evidence/ research corpus is not present" >&2; exit 2; }
	@mkdir -p $(WORK)/locks
	@mkdir -p $(UV_CACHE)
	UV_CACHE_DIR=$(UV_CACHE) UV_PROJECT_ENVIRONMENT=$(QUALITY_ENV) uv run --project tooling/quality --locked python tooling/quality/check.py --scope evidence
	python3 -B -m unittest discover -s evidence/firmware-analysis/reverse_engineering/tools/tests -p 'test_*.py'
	python3 -B evidence/firmware-analysis/reverse_engineering/deep/application/tools/test_generate_declarative_dataflow.py
	python3 -B evidence/firmware-analysis/reverse_engineering/deep/kernel_modules/test_recover_kernel_abi_bindings.py
	python3 -B evidence/firmware-analysis/reverse_engineering/deep/userland/test_rebuild_deep_userland.py
	AULA_REQUIRE_EVIDENCE=1 UV_CACHE_DIR=$(UV_CACHE) UV_PROJECT_ENVIRONMENT=$(QUALITY_ENV) \
		uv run --project tooling/quality --locked python -m unittest tooling.quality.tests.test_check
	$(MAKE) -C lab/emulator setup
	AULA_REQUIRE_EVIDENCE=1 $(LAB_PYTEST) -m corpus lab/qemu/tests

.PHONY: test-fastcgi benchmark-latency
test-fastcgi:
	$(MAKE) -C product/console test-fastcgi FCGI_PREFIX="$(FCGI_PREFIX)"

benchmark-latency:
	@test -n "$(BENCHMARK_BASELINE)" || { echo "set BENCHMARK_BASELINE to a preserved source checkout" >&2; exit 2; }
	python3 -B tooling/performance/run_benchmarks.py --baseline-source "$(BENCHMARK_BASELINE)" --current-source . --output $(WORK)/reports/optimization/runtime-benchmarks.json

.PHONY: test-product-core test-lab-core
test-product: quality test-product-core
test-lab: quality test-lab-core test-deployment

check-layout:
	python3 -B tooling/quality/layout.py

.PHONY: check-source-boundary check-public-release release-source
check-source-boundary:
	python3 -B tooling/quality/public_release.py --check-boundary

check-public-release:
	python3 -B tooling/quality/public_release.py --check-release

release-source:
	python3 -B tooling/quality/public_release.py --export

quality:
	@mkdir -p $(WORK)/locks
	@mkdir -p $(UV_CACHE)
	UV_CACHE_DIR=$(UV_CACHE) UV_PROJECT_ENVIRONMENT=$(QUALITY_ENV) uv run --project tooling/quality --locked python tooling/quality/check.py --scope all
	UV_CACHE_DIR=$(UV_CACHE) UV_PROJECT_ENVIRONMENT=$(QUALITY_ENV) uv run --project tooling/quality --locked python -m unittest discover -s tooling/quality/tests
	python3 -B -m unittest discover -s tooling/console/tests
	python3 -B -m unittest discover -s tooling/workspace/tests

test-product-core:
	@mkdir -p $(WORK)/build $(WORK)/cache $(WORK)/dist $(WORK)/reports
	python3 -B -m unittest discover -s tooling/performance/tests
	UV_CACHE_DIR=$(UV_CACHE) AULA_SIPD_VERIFY_ENABLE=1 sh product/sipd/tools/verify-repository.sh --run
	$(MAKE) -C product/console test
	AULA_CONSOLE_REQUIRE_READY=1 $(MAKE) -C product/console ui-install ui-test ui-build

test-lab-core:
	@mkdir -p $(WORK)/build/qemu $(WORK)/cache $(WORK)/dist/qemu $(WORK)/reports/qemu
	$(MAKE) -C lab/emulator verify-core
	$(LAB_PYTEST) lab/qemu/tests
	$(LAB_PYTEST) lab/sip-peer/tests
	UV_CACHE_DIR=$(UV_CACHE) sh lab/qemu/scripts/verify.sh

test-deployment:
	python3 -B deployment/tests/test_console_assets.py
	python3 -B deployment/tests/test_shared_records.py
	python3 -B deployment/tests/test_name_migration.py
	python3 -B deployment/tests/test_deployment_contract.py
	python3 -B deployment/tests/test_live_deployment_contract.py
	python3 -B deployment/tests/test_payload_trust.py
	python3 -B -m unittest discover -s tooling/live/tests -p 'test_*.py'
	python3 -B -m unittest discover -s tooling/device-evidence/tests -p 'test_*.py'
	python3 -B -m unittest discover -s dependencies/scripts/tests -p 'test_*.py'

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
	@echo "QEMU payload overlay packaging is unavailable for the synthetic machine model" >&2; exit 2

qemu-build:
	sh lab/qemu/scripts/fetch-qemu.sh
	sh lab/qemu/scripts/prepare-qemu.sh
	sh lab/qemu/scripts/build.sh

qemu-verify: qemu-build
	QEMU_BINARY=$(QEMU_BINARY) sh lab/qemu/scripts/verify.sh --require-source

qemu-run:
	@echo "No maintained guest launcher; supply a reviewed ELF or raw image to the synthetic QEMU model explicitly" >&2; exit 2

qemu-acceptance:
	@echo "Two-slot product acceptance is unavailable in the synthetic QEMU lane; live gates remain closed" >&2; exit 2

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

clean:
	sh tooling/workspace/trash-work.sh "$(TRASH_BUNDLE)"
