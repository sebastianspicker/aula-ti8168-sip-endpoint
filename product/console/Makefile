CC ?= cc
CFLAGS ?= -std=c17 -Wall -Wextra -Werror -pedantic -O2
CPPFLAGS += -Igateway -I../sipd/include $(shell pkg-config --cflags jansson openssl)
LDLIBS += $(shell pkg-config --libs jansson openssl)
BUILD ?= ../../.work/build/console
DEPENDENCY_RUNNER := ../../tooling/console/with-dependencies.sh
# Single source of truth for the preview build's source list; see
# gateway/preview/INTEGRATION.md. Cross-build tooling reads it through
# `make -s -C product/console print-preview-sources`.
include gateway/preview/sources.mk
# Single source of truth for the gateway's own translation units and the
# device-client sources it links; see gateway/sources.mk, which
# build-gateway-arm.sh reads via
# `make -s -C product/console print-gateway-sources` and
# `make -s -C product/console print-device-client-sources`.
include gateway/sources.mk
PREVIEW_CPPFLAGS := -Igateway/preview -I../sipd/include \
	-I../sipd/src/backends

.PHONY: all clean fastcgi ui-install ui-build ui-test device-control test gateway-check \
	print-preview-sources print-gateway-sources print-device-client-sources

all: device-control

print-preview-sources:
	@for source in $(PREVIEW_SOURCES); do echo $$source; done

print-gateway-sources:
	@for source in $(GATEWAY_SOURCES); do echo $$source; done

print-device-client-sources:
	@for source in $(DEVICE_CLIENT_SOURCES); do echo $$source; done

# Included private C fragments are tracked transitively by the compiler.
# DEVICE_CLIENT_SOURCES (from gateway/sources.mk) is the only half of the
# device protocol the unprivileged gateway links: transport, wire framing,
# the client exchange, the recorder projection, and credential *schema*
# validation (never the credential store). DEVICE_SERVER (serving loop and
# the persistent credential store) links only into aula-device-control and
# the privileged companion.
DEVICE_SERVER := device/server.c device/credentials_store.c
DEVICE_CLIENT_OBJECTS := $(patsubst %.c,$(BUILD)/obj/%.o,$(DEVICE_CLIENT_SOURCES))
DEVICE_SERVER_OBJECTS := $(patsubst %.c,$(BUILD)/obj/%.o,$(DEVICE_SERVER))
DEVICE_COMPANION_OBJECTS := $(DEVICE_CLIENT_OBJECTS) $(DEVICE_SERVER_OBJECTS)
FCGI_SOURCES := $(DEVICE_CLIENT_SOURCES) $(GATEWAY_SOURCES) gateway/fastcgi_main.c gateway/fastcgi_request.c gateway/fastcgi_preview.c $(PREVIEW_SOURCES)
FCGI_OBJECTS := $(patsubst %.c,$(BUILD)/fcgi/%.o,$(subst ../sipd/,sipd/,$(FCGI_SOURCES)))

$(BUILD)/obj/%.o: %.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -pthread -MMD -MP -c $< -o $@

# Deliberately opt-in; consumes a locally provisioned FastCGI installation.
fastcgi: check-fcgi-prefix $(BUILD)/aula-gateway-fcgi

.PHONY: check-fcgi-prefix
check-fcgi-prefix:
	@test -n "$(FCGI_PREFIX)" || (echo "Set FCGI_PREFIX to a vetted FastCGI 2.4.7 installation"; exit 2)

$(BUILD)/aula-gateway-fcgi: $(FCGI_OBJECTS) $(wildcard $(FCGI_PREFIX)/lib/libfcgi.*)
	$(CC) $(CFLAGS) -pthread $(FCGI_OBJECTS) $(LDLIBS) -L$(FCGI_PREFIX)/lib -lfcgi -o $@

$(BUILD)/fcgi/%.o: %.c | check-fcgi-prefix
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(PREVIEW_CPPFLAGS) $(CFLAGS) -pthread -MMD -MP \
		-DAULA_GATEWAY_WITH_FCGI -I$(FCGI_PREFIX)/include -c $< -o $@

$(BUILD)/fcgi/sipd/%.o: ../sipd/%.c | check-fcgi-prefix
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(PREVIEW_CPPFLAGS) $(CFLAGS) -pthread -MMD -MP \
		-DAULA_GATEWAY_WITH_FCGI -I$(FCGI_PREFIX)/include -c $< -o $@

-include $(FCGI_OBJECTS:.o=.d)

ui-install:
	sh $(DEPENDENCY_RUNNER) --install

ui-build: ui-install
	sh $(DEPENDENCY_RUNNER) tsc -b
	sh $(DEPENDENCY_RUNNER) vite build \
		--outDir ../../../.work/dist/console-web --emptyOutDir

ui-test: ui-install
	sh $(DEPENDENCY_RUNNER) vitest run

clean:
	sh $(DEPENDENCY_RUNNER) --clean

device-control: $(BUILD)/aula-device-control

$(BUILD)/aula-device-control: $(DEVICE_COMPANION_OBJECTS) $(BUILD)/obj/device/main.o $(BUILD)/obj/device/jobs.o $(BUILD)/obj/device/launcher.o $(BUILD)/obj/device/commands.o
	$(CC) $(CFLAGS) $^ $(LDLIBS) -o $@

# Compile-only check of every gateway translation unit that does not need
# fcgiapp.h, so the default build catches compile rot without FastCGI.
# fastcgi_main.c and fastcgi_preview.c compile only in the opt-in fastcgi
# target above.
GATEWAY_CHECK_SOURCES := gateway/fastcgi_request.c $(PREVIEW_SOURCES)
GATEWAY_CHECK_OBJECTS := $(patsubst %.c,$(BUILD)/check/%.o,$(subst ../sipd/,sipd/,$(GATEWAY_CHECK_SOURCES)))

gateway-check: $(patsubst %.c,$(BUILD)/obj/%.o,$(GATEWAY_SOURCES)) $(GATEWAY_CHECK_OBJECTS)

$(BUILD)/check/%.o: %.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(PREVIEW_CPPFLAGS) $(CFLAGS) -pthread -MMD -MP -c $< -o $@

$(BUILD)/check/sipd/%.o: ../sipd/%.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(PREVIEW_CPPFLAGS) $(CFLAGS) -pthread -MMD -MP -c $< -o $@

-include $(GATEWAY_CHECK_OBJECTS:.o=.d)

# Host unit tests: standalone C programs linked against the pure gateway and
# the device client half, compiled without FastCGI. A non-zero exit fails.
CONSOLE_TESTS := test_gateway_basics test_gateway_session_renewal test_gateway_auth \
	test_gateway_idempotency test_gateway_logging test_gateway_auth_limits test_fastcgi_request \
	test_gateway_device test_gateway_device_routing
GATEWAY_OBJECTS := $(patsubst %.c,$(BUILD)/obj/%.o,$(GATEWAY_SOURCES))
CONSOLE_TEST_BINARIES := $(addprefix $(BUILD)/tests/,$(CONSOLE_TESTS))
CONSOLE_TEST_OBJECTS := $(CONSOLE_TESTS:%=$(BUILD)/obj/tests/%.o)
# Keep objects built through the pattern chain for incremental rebuilds.
.SECONDARY: $(GATEWAY_OBJECTS) $(CONSOLE_TEST_OBJECTS)

test: $(CONSOLE_TEST_BINARIES)
	@for binary in $(CONSOLE_TEST_BINARIES); do $$binary || exit 1; done

$(BUILD)/tests/%: $(BUILD)/obj/tests/%.o $(GATEWAY_OBJECTS) $(DEVICE_CLIENT_OBJECTS)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -pthread $^ $(LDLIBS) -o $@

# The routing fixture substitutes only connect(), recording the configured path
# and identity. The framing/peer contract uses the normal transport elsewhere.
$(BUILD)/tests/test_gateway_device_routing: $(BUILD)/obj/tests/test_gateway_device_routing.o $(GATEWAY_OBJECTS) $(filter-out $(BUILD)/obj/device/transport.o,$(DEVICE_CLIENT_OBJECTS)) $(BUILD)/obj/tests/device_transport_real.o
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -pthread $^ $(LDLIBS) -o $@

$(BUILD)/obj/tests/device_transport_real.o: device/transport.c device/transport.h
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Daula_device_connect=aula_device_connect_real -c $< -o $@

# The request-mapping test also links the FastCGI adapter's pure mapping unit.
$(BUILD)/tests/test_fastcgi_request: $(BUILD)/obj/gateway/fastcgi_request.o

-include $(GATEWAY_OBJECTS:.o=.d) $(CONSOLE_TEST_OBJECTS:.o=.d)

-include $(DEVICE_COMPANION_OBJECTS:.o=.d) \
	$(BUILD)/obj/device/main.d \
	$(BUILD)/obj/device/jobs.d \
	$(BUILD)/obj/device/launcher.d \
	$(BUILD)/obj/device/commands.d
