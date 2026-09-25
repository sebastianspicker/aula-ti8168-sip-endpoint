CC ?= cc
CFLAGS ?= -std=c17 -Wall -Wextra -Werror -pedantic -O2
CPPFLAGS += -Igateway $(shell pkg-config --cflags jansson openssl)
LDLIBS += $(shell pkg-config --libs jansson openssl)
BUILD ?= ../../.work/build/console
DEPENDENCY_RUNNER := ../../tooling/console/with-dependencies.sh
PREVIEW_SOURCES := \
	gateway/preview/preview_reader.c \
	gateway/preview/preview_reader_protocol.c \
	gateway/preview/preview_reader_rtp.c \
	gateway/preview/preview_flv.c \
	../sipd/src/backends/rtsp_parser.c \
	../sipd/src/media/h264.c \
	../sipd/src/media/h264_depacketizer.c
PREVIEW_CPPFLAGS := -Igateway/preview -I../sipd/include \
	-I../sipd/src/backends

.PHONY: test clean fastcgi test-fastcgi ui-install ui-test ui-build

test: $(BUILD)/test_device $(BUILD)/test_jobs $(BUILD)/test_queries $(BUILD)/test_credentials $(BUILD)/test_gateway $(BUILD)/test_fastcgi_request
	$(BUILD)/test_device
	$(BUILD)/test_jobs $(BUILD)
	$(BUILD)/test_queries
	$(BUILD)/test_credentials $(BUILD)
	$(BUILD)/test_gateway
	$(BUILD)/test_fastcgi_request

# Included private C fragments are tracked transitively by the compiler.
DEVICE_SHARED := device/transport.c device/protocol.c device/projection.c device/credentials.c
DEVICE_OBJECTS := $(patsubst %.c,$(BUILD)/obj/%.o,$(DEVICE_SHARED))
GATEWAY_OBJECTS := $(BUILD)/obj/gateway/gateway.o $(DEVICE_OBJECTS)
TEST_GATEWAY_OBJECTS := $(GATEWAY_OBJECTS) $(BUILD)/obj/tests/gateway/test_gateway.o
REQUEST_TEST_OBJECTS := $(BUILD)/obj/gateway/fastcgi_request.o $(BUILD)/obj/tests/gateway/test_fastcgi_request.o
FCGI_SOURCES := $(DEVICE_SHARED) gateway/gateway.c gateway/fastcgi_main.c gateway/fastcgi_request.c gateway/fastcgi_preview.c $(PREVIEW_SOURCES)
FCGI_OBJECTS := $(patsubst %.c,$(BUILD)/fcgi/%.o,$(subst ../sipd/,sipd/,$(FCGI_SOURCES)))

$(BUILD)/test_gateway: $(TEST_GATEWAY_OBJECTS)
	$(CC) $(CFLAGS) -pthread $^ $(LDLIBS) -o $@

$(BUILD)/test_fastcgi_request: $(REQUEST_TEST_OBJECTS)
	$(CC) $(CFLAGS) -pthread $^ -o $@

$(BUILD)/obj/%.o: %.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -pthread -MMD -MP -c $< -o $@

# Deliberately opt-in; consumes a locally provisioned FastCGI installation.
fastcgi: check-fcgi-prefix $(BUILD)/ls200-gateway-fcgi

test-fastcgi: fastcgi
	python3 -B tests/runtime/test_fastcgi_runtime.py --binary $(BUILD)/ls200-gateway-fcgi

.PHONY: check-fcgi-prefix
check-fcgi-prefix:
	@test -n "$(FCGI_PREFIX)" || (echo "Set FCGI_PREFIX to a vetted FastCGI 2.4.7 installation"; exit 2)

$(BUILD)/ls200-gateway-fcgi: $(FCGI_OBJECTS)
	$(CC) $(CFLAGS) -pthread $^ $(LDLIBS) -L$(FCGI_PREFIX)/lib -lfcgi -o $@

$(BUILD)/fcgi/%.o: %.c | check-fcgi-prefix
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(PREVIEW_CPPFLAGS) $(CFLAGS) -pthread -MMD -MP \
		-DLS200_GATEWAY_WITH_FCGI -I$(FCGI_PREFIX)/include -c $< -o $@

$(BUILD)/fcgi/sipd/%.o: ../sipd/%.c | check-fcgi-prefix
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(PREVIEW_CPPFLAGS) $(CFLAGS) -pthread -MMD -MP \
		-DLS200_GATEWAY_WITH_FCGI -I$(FCGI_PREFIX)/include -c $< -o $@

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
device-control: $(BUILD)/ls200-device-control

$(BUILD)/ls200-device-control: $(DEVICE_OBJECTS) $(BUILD)/obj/device/oem.o $(BUILD)/obj/device/oem_http.o $(BUILD)/obj/device/main.o $(BUILD)/obj/device/jobs.o $(BUILD)/obj/device/launcher.o $(BUILD)/obj/device/commands.o
	$(CC) $(CFLAGS) $^ $(LDLIBS) -o $@

$(BUILD)/test_device: $(DEVICE_OBJECTS) $(BUILD)/obj/tests/device/test_device.o
	$(CC) $(CFLAGS) -pthread $^ $(LDLIBS) -o $@

$(BUILD)/test_jobs: $(BUILD)/obj/device/jobs.o $(BUILD)/obj/tests/device/test_jobs.o
	$(CC) $(CFLAGS) $^ $(LDLIBS) -o $@

-include $(BUILD)/obj/device/jobs.d $(BUILD)/obj/tests/device/test_jobs.d

$(BUILD)/test_queries: $(DEVICE_OBJECTS) $(BUILD)/obj/tests/device/test_queries.o
	$(CC) $(CFLAGS) $^ $(LDLIBS) -o $@

-include $(BUILD)/obj/tests/device/test_queries.d
-include $(BUILD)/obj/device/oem_http.d
-include $(BUILD)/obj/device/launcher.d

$(BUILD)/test_credentials: $(DEVICE_OBJECTS) $(BUILD)/obj/device/oem_http.o $(BUILD)/obj/device/commands.o $(BUILD)/obj/tests/device/test_credentials.o
	$(CC) $(CFLAGS) $^ $(LDLIBS) -o $@

-include $(BUILD)/obj/tests/device/test_credentials.d

-include $(DEVICE_OBJECTS:.o=.d) $(BUILD)/obj/device/oem.d $(BUILD)/obj/device/main.d $(BUILD)/obj/tests/device/test_device.d

-include $(BUILD)/obj/device/commands.d
