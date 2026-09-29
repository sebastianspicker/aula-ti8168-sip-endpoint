#include "commands.h"
#include "credentials_store.h"
#include <string.h>

json_t *aula_device_dispatch(const char *operation, json_t *arguments,
    const char *correlation, const char **outcome) {
  if (strcmp(operation, "credentials.replace") == 0) {
    *outcome = aula_device_credentials_update(arguments, correlation);
    if (strcmp(*outcome, "duplicate") == 0) {
      *outcome = "succeeded";
      return aula_device_credentials_receipt(correlation);
    }
    if (strcmp(*outcome, "succeeded") != 0) return NULL;
  } else if (strcmp(operation, "credentials.status") != 0) {
    *outcome = "invalid";
    return NULL;
  }
  return aula_device_credentials_status("unknown");
}
