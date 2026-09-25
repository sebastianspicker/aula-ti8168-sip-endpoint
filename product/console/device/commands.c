#include "commands.h"
#include "credentials_store.h"
#include "oem.h"
#include <string.h>

json_t *ls200_device_dispatch(const char *operation, json_t *arguments,
    const char *correlation, const char **outcome) {
  if (strcmp(operation, "credentials.replace") == 0) {
    *outcome = ls200_device_credentials_update(arguments, correlation);
    if (strcmp(*outcome, "duplicate") == 0) {
      *outcome = "succeeded";
      return ls200_device_credentials_receipt(correlation);
    }
    if (strcmp(*outcome, "succeeded") != 0) return NULL;
    ls200_device_oem_reset(NULL);
  } else if (strcmp(operation, "credentials.status") != 0) {
    *outcome = "invalid";
    return NULL;
  }
  return ls200_device_oem_connection();
}
