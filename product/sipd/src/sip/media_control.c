#include "media_control.h"

#include <string.h>

typedef struct media_control_reader {
  aula_bytes input;
  size_t offset;
} media_control_reader;

static int control_take(media_control_reader *reader, const char *text) {
  size_t length = strlen(text);
  if (length > reader->input.length - reader->offset ||
      memcmp(reader->input.data + reader->offset, text, length) != 0) return 0;
  reader->offset += length;
  return 1;
}

static size_t control_space(media_control_reader *reader) {
  size_t start = reader->offset;
  while (reader->offset < reader->input.length) {
    uint8_t value = reader->input.data[reader->offset];
    if (value != ' ' && value != '\t' && value != '\r' && value != '\n') break;
    ++reader->offset;
  }
  return reader->offset - start;
}

static int control_attribute(media_control_reader *reader, const char *name,
                               const char *first, const char *second) {
  uint8_t quote;
  if (!control_take(reader, name)) return 0;
  (void)control_space(reader);
  if (!control_take(reader, "=")) return 0;
  (void)control_space(reader);
  if (reader->offset == reader->input.length) return 0;
  quote = reader->input.data[reader->offset++];
  if (quote != '\'' && quote != '"') return 0;
  if (!control_take(reader, first) &&
      (second == NULL || !control_take(reader, second))) return 0;
  if (reader->offset == reader->input.length ||
      reader->input.data[reader->offset++] != quote) return 0;
  return 1;
}

static int control_declaration(media_control_reader *reader) {
  if (!control_take(reader, "<?xml")) return 1;
  if (control_space(reader) == 0U ||
      !control_attribute(reader, "version", "1.0", NULL)) return 0;
  if (control_space(reader) != 0U) {
    if (reader->offset < reader->input.length &&
        reader->input.data[reader->offset] == 'e') {
      if (!control_attribute(reader, "encoding", "utf-8", "UTF-8")) return 0;
      (void)control_space(reader);
    }
  }
  return control_take(reader, "?>");
}

static int control_tag(media_control_reader *reader, const char *prefix) {
  (void)control_space(reader);
  if (!control_take(reader, prefix)) return 0;
  (void)control_space(reader);
  return control_take(reader, ">");
}

static int control_picture(media_control_reader *reader) {
  (void)control_space(reader);
  if (!control_take(reader, "<picture_fast_update")) return 0;
  (void)control_space(reader);
  if (control_take(reader, "/>")) return 1;
  return control_take(reader, ">") &&
      control_tag(reader, "</picture_fast_update");
}

int aula_sip_media_control_is_picture_update(aula_bytes body) {
  media_control_reader reader = {body, 0U};
  if (body.data == NULL || body.length == 0U ||
      body.length > AULA_SIPD_MAX_SIP_MESSAGE_BYTES) return 0;
  (void)control_take(&reader, "\xef\xbb\xbf");
  (void)control_space(&reader);
  if (!control_declaration(&reader) ||
      !control_tag(&reader, "<media_control") ||
      !control_tag(&reader, "<vc_primitive") ||
      !control_tag(&reader, "<to_encoder") ||
      !control_picture(&reader) ||
      !control_tag(&reader, "</to_encoder") ||
      !control_tag(&reader, "</vc_primitive") ||
      !control_tag(&reader, "</media_control")) return 0;
  (void)control_space(&reader);
  return reader.offset == body.length;
}
