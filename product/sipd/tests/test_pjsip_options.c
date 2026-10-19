/* PJSIP driver option validation. It runs in every build: the options are
 * validated before any PJSIP type is touched. */
#include "aula_sipd/pjsip.h"
#include "aula_test.h"

static aula_status configure(const char *bind_address) {
  aula_pjsip_driver_options options;
  aula_sip_driver_config config;
  (void)memset(&options, 0, sizeof(options));
  options.profile = AULA_ZOOM_PROFILE_PRIVATE_LAB;
  options.local_uri = "sip:aula@127.0.0.1";
  options.bind_address = bind_address;
  return aula_pjsip_driver_configure(&options, &config);
}

static void test_bind_address_is_an_ipv4_literal(void) {
  AULA_CHECK_STATUS(configure(NULL), AULA_STATUS_OK);
  AULA_CHECK_STATUS(configure("127.0.0.1"), AULA_STATUS_OK);
  AULA_CHECK_STATUS(configure("192.168.10.20"), AULA_STATUS_OK);
  AULA_CHECK_STATUS(configure(""), AULA_STATUS_INVALID_DATA);
  AULA_CHECK_STATUS(configure("localhost"), AULA_STATUS_INVALID_DATA);
  AULA_CHECK_STATUS(configure("::1"), AULA_STATUS_INVALID_DATA);
  AULA_CHECK_STATUS(configure("127.0.0.1:5060"), AULA_STATUS_INVALID_DATA);
  AULA_CHECK_STATUS(configure("1111.2222.3333.4444"), AULA_STATUS_INVALID_DATA);
}

int main(void) {
  test_bind_address_is_an_ipv4_literal();
  return aula_test_result("test_pjsip_options");
}
