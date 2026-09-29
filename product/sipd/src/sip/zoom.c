#include "aula_sipd/zoom.h"

#include "aula_sipd/sip.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

static int zoom_profile_is_valid(aula_zoom_profile profile) {
  return profile >= AULA_ZOOM_PROFILE_DIRECT_CRC &&
         profile <= AULA_ZOOM_PROFILE_PRIVATE_LAB;
}

static int zoom_digits(const char *value, size_t maximum, int optional) {
  size_t index;
  size_t length;
  if (value == NULL) return optional;
  length = strlen(value);
  if (length == 0U) return optional || 0;
  if (length >= maximum) return 0;
  for (index = 0U; index < length; ++index)
    if (!isdigit((unsigned char)value[index])) return 0;
  return 1;
}

static int zoom_dial_code_is_valid(const char *value) {
  size_t index;
  size_t length;
  if (value == NULL || value[0] == '\0') return 1;
  length = strlen(value);
  if (length >= AULA_ZOOM_MAX_DIAL_CODE_BYTES) return 0;
  for (index = 0U; index < length; ++index) {
    if (!isalnum((unsigned char)value[index])) return 0;
  }
  return 1;
}

static int zoom_domain_is_valid(const char *value) {
  size_t index;
  size_t length;
  size_t label_length = 0U;
  if (value == NULL || value[0] == '\0') return 0;
  length = strlen(value);
  if (length >= 254U || value[0] == '.' || value[length - 1U] == '.') return 0;
  for (index = 0U; index < length; ++index) {
    unsigned char character = (unsigned char)value[index];
    if (character == '.') {
      if (label_length == 0U || value[index - 1U] == '-') return 0;
      label_length = 0U;
      continue;
    }
    if ((!isalnum(character) && character != '-') ||
        (label_length == 0U && character == '-') || label_length == 63U)
      return 0;
    ++label_length;
  }
  return value[length - 1U] != '-';
}

static int zoom_uri_has_crc_domain(const char *domain) {
  size_t length = strcspn(domain, ":;");
  return length == strlen(AULA_ZOOM_CRC_DOMAIN) &&
         strncmp(domain, AULA_ZOOM_CRC_DOMAIN, length) == 0;
}

static const char *zoom_layout_command(aula_zoom_layout layout) {
  switch (layout) {
    case AULA_ZOOM_LAYOUT_GALLERY: return "";
    case AULA_ZOOM_LAYOUT_FULL_SCREEN: return "11";
    case AULA_ZOOM_LAYOUT_DUAL_VIDEO: return "308";
    default: return NULL;
  }
}

static aula_status zoom_output_error(aula_mutable_bytes *output,
                                      aula_status status) {
  if (output != NULL && output->data != NULL) {
    (void)memset(output->data, 0, output->capacity);
    output->length = 0U;
  }
  return status;
}

static int zoom_request_fields_are_valid(const aula_zoom_dial_request *request) {
  return zoom_digits(request->meeting_id, AULA_ZOOM_MAX_MEETING_ID_BYTES, 0) &&
         zoom_digits(request->passcode, AULA_ZOOM_MAX_PASSCODE_BYTES, 1) &&
         zoom_digits(request->host_key, AULA_ZOOM_MAX_HOST_KEY_BYTES, 1) &&
         zoom_dial_code_is_valid(request->dial_code) &&
         zoom_layout_command(request->layout) != NULL;
}

static int zoom_request_port_is_valid(const aula_zoom_dial_request *request) {
  return (request->has_target_port != 0 && request->target_port != 0U) ||
         (request->has_target_port == 0 && request->target_port == 0U);
}

static int zoom_request_domain_is_valid(const aula_zoom_dial_request *request,
                                        const char **out_domain) {
  const char *domain = request->target_domain;
  if (request->profile == AULA_ZOOM_PROFILE_DIRECT_CRC) {
    if (domain != NULL && strcmp(domain, AULA_ZOOM_CRC_DOMAIN) != 0) return 0;
    domain = AULA_ZOOM_CRC_DOMAIN;
  }
  if (!zoom_domain_is_valid(domain)) return 0;
  *out_domain = domain;
  return 1;
}

static int zoom_request_is_valid(const aula_zoom_dial_request *request,
                                 const char **out_domain) {
  return request != NULL && out_domain != NULL &&
         zoom_profile_is_valid(request->profile) &&
         zoom_request_fields_are_valid(request) && zoom_request_port_is_valid(request) &&
         zoom_request_domain_is_valid(request, out_domain);
}

static size_t zoom_last_dial_field(const char *const fields[6]) {
  if (fields[5][0] != '\0') return 5U;
  if (fields[3][0] != '\0') return 3U;
  if (fields[2][0] != '\0') return 2U;
  return fields[1][0] != '\0' ? 1U : 0U;
}

static void zoom_dial_fields(const aula_zoom_dial_request *request,
                             const char *fields[6], size_t *last_field) {
  fields[0] = request->meeting_id;
  fields[1] = request->passcode == NULL ? "" : request->passcode;
  fields[2] = zoom_layout_command(request->layout);
  fields[3] = request->host_key == NULL ? "" : request->host_key;
  fields[4] = "";
  fields[5] = request->dial_code == NULL ? "" : request->dial_code;
  *last_field = zoom_last_dial_field(fields);
}

static aula_status zoom_append_field(aula_mutable_bytes *output, size_t *used,
                                      const char *field, int add_separator) {
  size_t field_length = strlen(field);
  if (add_separator != 0) {
    if (*used + 1U >= output->capacity) return AULA_STATUS_LIMIT_EXCEEDED;
    output->data[(*used)++] = (uint8_t)'.';
  }
  if (field_length >= output->capacity - *used) return AULA_STATUS_LIMIT_EXCEEDED;
  (void)memcpy(output->data + *used, field, field_length);
  *used += field_length;
  return AULA_STATUS_OK;
}

static aula_status zoom_append_fields(aula_mutable_bytes *output, size_t *used,
                                       const char *const fields[6], size_t last_field) {
  size_t index;
  for (index = 0U; index <= last_field; ++index) {
    aula_status status = zoom_append_field(output, used, fields[index], index != 0U);
    if (status != AULA_STATUS_OK) return status;
  }
  return AULA_STATUS_OK;
}

static aula_status zoom_append_domain(aula_mutable_bytes *output, size_t *used,
                                       const aula_zoom_dial_request *request,
                                       const char *domain) {
  int written = request->has_target_port != 0
      ? snprintf((char *)output->data + *used, output->capacity - *used,
                 "@%s:%u", domain, (unsigned int)request->target_port)
      : snprintf((char *)output->data + *used, output->capacity - *used,
                 "@%s", domain);
  if (written < 0 || (size_t)written >= output->capacity - *used)
    return AULA_STATUS_LIMIT_EXCEEDED;
  *used += (size_t)written;
  return AULA_STATUS_OK;
}

aula_status aula_zoom_build_dial_target(
    const aula_zoom_dial_request *request, aula_mutable_bytes *output) {
  const char *fields[6];
  const char *domain;
  size_t last_field;
  size_t used = 0U;
  int written;
  aula_status status;
  if (output == NULL || output->data == NULL || output->capacity == 0U) {
    return AULA_STATUS_INVALID_ARGUMENT;
  }
  (void)memset(output->data, 0, output->capacity);
  output->length = 0U;
  if (!zoom_request_is_valid(request, &domain))
    return zoom_output_error(output, AULA_STATUS_INVALID_DATA);
  zoom_dial_fields(request, fields, &last_field);
  written = snprintf((char *)output->data, output->capacity, "sip:");
  if (written < 0 || (size_t)written >= output->capacity)
    return zoom_output_error(output, AULA_STATUS_LIMIT_EXCEEDED);
  used = (size_t)written;
  status = zoom_append_fields(output, &used, fields, last_field);
  if (status != AULA_STATUS_OK) return zoom_output_error(output, status);
  status = zoom_append_domain(output, &used, request, domain);
  if (status != AULA_STATUS_OK) return zoom_output_error(output, status);
  output->length = used;
  return aula_sip_parse_dial_target((const char *)output->data, &(aula_sip_dial_target){0}) ==
                 AULA_STATUS_OK
             ? AULA_STATUS_OK
             : zoom_output_error(output, AULA_STATUS_INTERNAL_ERROR);
}

aula_status aula_zoom_classify_dial_target(
    const char *target_uri, aula_zoom_dial_target *out_target) {
  aula_sip_dial_target target;
  const char *domain;
  if (target_uri == NULL || out_target == NULL) return AULA_STATUS_INVALID_ARGUMENT;
  if (strstr(target_uri, "http:") != NULL || strstr(target_uri, "https:") != NULL ||
      aula_sip_parse_dial_target(target_uri, &target) != AULA_STATUS_OK)
    return AULA_STATUS_INVALID_DATA;
  domain = strchr(target_uri + 4U, '@');
  if (domain == NULL || domain[1] == '\0') return AULA_STATUS_INVALID_DATA;
  ++domain;
  (void)memset(out_target, 0, sizeof(*out_target));
  out_target->profile = zoom_uri_has_crc_domain(domain)
                            ? AULA_ZOOM_PROFILE_DIRECT_CRC
                            : AULA_ZOOM_PROFILE_PRIVATE_LAB;
  out_target->transport = target.transport;
  out_target->port = target.port;
  out_target->has_explicit_port = target.has_explicit_port;
  return AULA_STATUS_OK;
}

aula_status aula_zoom_format_redacted_profile(
    aula_zoom_profile profile, aula_mutable_bytes *output) {
  const char *name;
  int written;
  if (output == NULL || output->data == NULL || output->capacity == 0U ||
      !zoom_profile_is_valid(profile)) return AULA_STATUS_INVALID_ARGUMENT;
  name = profile == AULA_ZOOM_PROFILE_DIRECT_CRC ? "direct_crc" :
         profile == AULA_ZOOM_PROFILE_PROXY_REGISTRATION ? "proxy_registration" :
         "private_lab";
  written = snprintf((char *)output->data, output->capacity,
                     "zoom_profile=%s target=redacted", name);
  if (written < 0 || (size_t)written >= output->capacity) {
    output->length = 0U;
    return AULA_STATUS_LIMIT_EXCEEDED;
  }
  output->length = (size_t)written;
  return AULA_STATUS_OK;
}
