CC ?= cc
CFLAGS ?= -std=c17 -Wall -Wextra -Werror -pedantic -O2
CPPFLAGS += -Igateway -I../sipd/include $(shell pkg-config --cflags jansson openssl)
LDLIBS += $(shell pkg-config --libs jansson openssl)
BUILD ?= ../../.work/build/console
DEPENDENCY_RUNNER := ../../tooling/console/with-dependencies.sh
# Single source of truth for the preview build's source list; see
# gateway/preview/INTEGRATION.md and tests/preview/run.sh, which reads it via
# `make -s -C product/console print-preview-sources`.
include gateway/preview/sources.mk
# Single source of truth for the gateway's own translation units and the
# device-client sources it links; see gateway/sources.mk, which
# build-gateway-arm.sh and the performance harness read via
# `make -s -C product/console print-gateway-sources` and
# `make -s -C product/console print-device-client-sources`.
include gateway/sources.mk
PREVIEW_CPPFLAGS := -Igateway/preview -I../sipd/include \
	-I../sipd/src/backends

.PHONY: test clean fastcgi test-fastcgi ui-install ui-test ui-build \
	print-preview-sources print-gateway-sources print-device-client-sources

print-preview-sources:
	@for source in $(PREVIEW_SOURCES); do echo $$source; done

print-gateway-sources:
	@for source in $(GATEWAY_SOURCES); do echo $$source; done

print-device-client-sources:
	@for source in $(DEVICE_CLIENT_SOURCES); do echo $$source; done

test: $(BUILD)/test_device $(BUILD)/test_jobs $(BUILD)/test_queries $(BUILD)/test_credentials $(BUILD)/test_gateway $(BUILD)/test_fastcgi_request
	$(BUILD)/test_device
	$(BUILD)/test_jobs $(BUILD)
	$(BUILD)/test_queries
	$(BUILD)/test_credentials $(BUILD)
	$(BUILD)/test_gateway
	$(BUILD)/test_fastcgi_request
	python3 -B tests/gateway/test_route_parity.py
	sh tests/preview/run.sh

# Included private C fragments are tracked transitively by the compiler.
# DEVICE_CLIENT_SOURCES (from gateway/sources.mk) is the only half of the
# device protocol the unprivileged gateway links: transport, wire framing,
# the client exchange, the recorder projection, and credential *schema*
# validation (never the credential store). DEVICE_SERVER (serving loop and
# the persistent credential store) links only into aula-device-control and
# its tests.
DEVICE_SERVER := device/server.c device/credentials_store.c
DEVICE_CLIENT_OBJECTS := $(patsubst %.c,$(BUILD)/obj/%.o,$(DEVICE_CLIENT_SOURCES))
DEVICE_SERVER_OBJECTS := $(patsubst %.c,$(BUILD)/obj/%.o,$(DEVICE_SERVER))
DEVICE_COMPANION_OBJECTS := $(DEVICE_CLIENT_OBJECTS) $(DEVICE_SERVER_OBJECTS)
GATEWAY_OBJECTS := $(patsubst %.c,$(BUILD)/obj/%.o,$(GATEWAY_SOURCES)) $(DEVICE_CLIENT_OBJECTS)
TEST_GATEWAY_OBJECTS := $(GATEWAY_OBJECTS) $(BUILD)/obj/tests/gateway/test_gateway.o
REQUEST_TEST_OBJECTS := $(BUILD)/obj/gateway/fastcgi_request.o $(BUILD)/obj/tests/gateway/test_fastcgi_request.o
FCGI_SOURCES := $(DEVICE_CLIENT_SOURCES) $(GATEWAY_SOURCES) gateway/fastcgi_main.c gateway/fastcgi_request.c gateway/fastcgi_preview.c $(PREVIEW_SOURCES)
FCGI_OBJECTS := $(patsubst %.c,$(BUILD)/fcgi/%.o,$(subst ../sipd/,sipd/,$(FCGI_SOURCES)))

$(BUILD)/test_gateway: $(TEST_GATEWAY_OBJECTS)
	$(CC) $(CFLAGS) -pthread $^ $(LDLIBS) -o $@

$(BUILD)/test_fastcgi_request: $(REQUEST_TEST_OBJECTS)
	$(CC) $(CFLAGS) -pthread $^ -o $@

$(BUILD)/obj/%.o: %.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -pthread -MMD -MP -c $< -o $@

# Deliberately opt-in; consumes a locally provisioned FastCGI installation.
fastcgi: check-fcgi-prefix $(BUILD)/aula-gateway-fcgi

test-fastcgi: fastcgi
	python3 -B tests/runtime/test_fastcgi_runtime.py --binary $(BUILD)/aula-gateway-fcgi

.PHONY: check-fcgi-prefix
check-fcgi-prefix:
	@test -n "$(FCGI_PREFIX)" || (echo "Set FCGI_PREFIX to a vetted FastCGI 2.4.7 installation"; exit 2)

$(BUILD)/aula-gateway-fcgi: $(FCGI_OBJECTS)
	$(CC) $(CFLAGS) -pthread $^ $(LDLIBS) -L$(FCGI_PREFIX)/lib -lfcgi -o $@

$(BUILD)/fcgi/%.o: %.c | check-fcgi-prefix
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(PREVIEW_CPPFLAGS) $(CFLAGS) -pthread -MMD -MP \
		-DAULA_GATEWAY_WITH_FCGI -I$(FCGI_PREFIX)/include -c $< -o $@

$(BUILD)/fcgi/sipd/%.o: ../sipd/%.c | check-fcgi-prefix
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(PREVIEW_CPPFLAGS) $(CFLAGS) -pthread -MMD -MP \
		-DAULA_GATEWAY_WITH_FCGI -I$(FCGI_PREFIX)/include -c $< -o $@

-include $(TEST_GATEWAY_OBJECTS:.o=.d) $(REQUEST_TEST_OBJECTS:.o=.d) $(FCGI_OBJECTS:.o=.d)

ui-install:
	sh $(DEPENDENCY_RUNNER) --install

ui-test: ui-install
	sh $(DEPENDENCY_RUNNER) vitest run

ui-build: ui-install
	sh $(DEPENDENCY_RUNNER) tsc -b
	sh $(DEPENDENCY_RUNNER) vite build \
		--outDir ../../../.work/dist/console-web --emptyOutDir

clean:
	sh $(DEPENDENCY_RUNNER) --clean

.PHONY: device-control
device-control: $(BUILD)/aula-device-control

$(BUILD)/aula-device-control: $(DEVICE_COMPANION_OBJECTS) $(BUILD)/obj/device/main.o $(BUILD)/obj/device/jobs.o $(BUILD)/obj/device/launcher.o $(BUILD)/obj/device/commands.o
	$(CC) $(CFLAGS) $^ $(LDLIBS) -o $@

$(BUILD)/test_device: $(DEVICE_COMPANION_OBJECTS) $(BUILD)/obj/tests/device/test_device.o
	$(CC) $(CFLAGS) -pthread $^ $(LDLIBS) -o $@

$(BUILD)/test_jobs: $(BUILD)/obj/device/jobs.o $(BUILD)/obj/tests/device/test_jobs.o
	$(CC) $(CFLAGS) $^ $(LDLIBS) -o $@

-include $(BUILD)/obj/device/jobs.d $(BUILD)/obj/tests/device/test_jobs.d

$(BUILD)/test_queries: $(DEVICE_COMPANION_OBJECTS) $(BUILD)/obj/tests/device/test_queries.o
	$(CC) $(CFLAGS) $^ $(LDLIBS) -o $@

-include $(BUILD)/obj/tests/device/test_queries.d
-include $(BUILD)/obj/device/launcher.d

$(BUILD)/test_credentials: $(DEVICE_COMPANION_OBJECTS) $(BUILD)/obj/device/commands.o $(BUILD)/obj/tests/device/test_credentials.o
	$(CC) $(CFLAGS) $^ $(LDLIBS) -o $@

-include $(BUILD)/obj/tests/device/test_credentials.d

-include $(DEVICE_COMPANION_OBJECTS:.o=.d) $(BUILD)/obj/device/oem.d $(BUILD)/obj/device/main.d $(BUILD)/obj/tests/device/test_device.d

-include $(BUILD)/obj/device/commands.d
