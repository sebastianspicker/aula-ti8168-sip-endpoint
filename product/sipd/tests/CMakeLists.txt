# Unit tests. Cross execution requires the explicit emulator opt-in. Each test
# is a standalone C program linked against the
# core library with the project's warning policy; a non-zero exit fails it.
# This is an explicit inventory, not a glob, like the core source list.
set(AULA_SIPD_TESTS
    test_control_frame
    test_control_protocol
    test_config_diagnostics
    test_config_parse
    test_request_decode
    test_sdp
    test_pjsip_options)

foreach(_aula_test IN LISTS AULA_SIPD_TESTS)
  add_executable(${_aula_test} ${_aula_test}.c)
  target_include_directories(${_aula_test} PRIVATE "${PROJECT_SOURCE_DIR}/src"
                                                   "${CMAKE_CURRENT_SOURCE_DIR}")
  target_link_libraries(${_aula_test} PRIVATE aula_sipd_core)
  aula_sipd_apply_platform_libraries(${_aula_test})
  add_test(NAME ${_aula_test} COMMAND ${_aula_test})
endforeach()

# The shipped example names /run and /var/lib/aula-sipd paths that a
# developer host lacks; whatever the host, a rejected configuration must say
# which setting failed instead of exiting silently.
add_test(NAME check_config_explains_rejection
         COMMAND aula-sipd --check-config
                 "${PROJECT_SOURCE_DIR}/config/aula-sipd.example.conf")
set_tests_properties(
  check_config_explains_rejection
  PROPERTIES PASS_REGULAR_EXPRESSION
             "configuration: valid|aula-sipd: configuration rejected: .+")
