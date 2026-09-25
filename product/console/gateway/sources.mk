# Single source of truth for the gateway's own translation units: the
# public gateway.h implementation, split into real modules along
# responsibility boundaries. gateway_internal.h is the shared internal
# interface between them; it is not itself a translation unit. Included by
# ../Makefile and read by build-gateway-arm.sh and the performance harness
# via `make -s -C product/console print-gateway-sources`. The FastCGI
# adapter (fastcgi_main.c, fastcgi_request.c, fastcgi_preview.c) and the
# preview sources (gateway/preview/sources.mk) are separate lists: they use
# only the public gateway.h/fastcgi_*.h interfaces.
GATEWAY_SOURCES := \
	gateway/gateway_core.c \
	gateway/gateway_account.c \
	gateway/gateway_account_store.c \
	gateway/gateway_store.c \
	gateway/gateway_session.c \
	gateway/gateway_schema.c \
	gateway/gateway_idempotency.c \
	gateway/gateway_preview_metadata.c \
	gateway/gateway_settings.c \
	gateway/gateway_aec.c \
	gateway/gateway_status.c \
	gateway/gateway_metrics.c \
	gateway/gateway_route_schema.c \
	gateway/gateway_route_backend.c \
	gateway/gateway_route_events.c \
	gateway/gateway_route_directory.c \
	gateway/gateway_route_users.c \
	gateway/gateway_auth_routes.c \
	gateway/gateway.c \
	gateway/gateway_route_response.c \
	gateway/gateway_route_request.c \
	gateway/gateway_device.c \
	gateway/gateway_transaction.c \
	gateway/gateway_control.c

# The unprivileged gateway links only the device protocol's client half:
# transport, wire framing, the client exchange, the recorder projection, and
# credential *schema* validation (never the credential store). Read by
# build-gateway-arm.sh and the performance harness via
# `make -s -C product/console print-device-client-sources`, so device source
# names live in exactly one place.
DEVICE_CLIENT_SOURCES := \
	device/transport.c \
	device/wire.c \
	device/client.c \
	device/projection.c \
	device/credentials.c
