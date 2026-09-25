/* The public gateway remains one translation unit so existing Makefile callers
 * keep working; implementation is organized by security responsibility. */
#include "gateway_core.c"
#include "gateway_account.c"
#include "gateway_store.c"
#include "gateway_session.c"
#include "gateway_preview_metadata.c"
#include "gateway_settings.c"
#include "gateway_aec.c"
#include "gateway_status.c"
#include "gateway_routes.c"
#include "gateway_transaction.c"
#include "gateway_control.c"
