cmake_minimum_required(VERSION 3.16)

project(
  aula_sipd
  VERSION 0.1.0
  LANGUAGES C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE
      "Debug"
      CACHE
        STRING
        "Choose the type of build: Debug, Release, RelWithDebInfo, or MinSizeRel"
        FORCE)
  set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release
                                               RelWithDebInfo MinSizeRel)
endif()

include(cmake/AulaSipdOptions.cmake)
include(cmake/AulaSipdPlatform.cmake)

add_library(aula_sipd_contract INTERFACE)
add_library(aula_sipd::contract ALIAS aula_sipd_contract)
target_include_directories(
  aula_sipd_contract
  INTERFACE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>)
target_compile_features(aula_sipd_contract INTERFACE c_std_99)
target_link_libraries(aula_sipd_contract INTERFACE aula_sipd_build_options)

# This is an explicit inventory, not a glob. Missing owned source files make
# configuration fail rather than silently yielding a partial daemon.
set(AULA_SIPD_CORE_SOURCES
    src/core/config.c
    src/core/config_loader.c
    src/core/config_parse.c
    src/core/config_persistence.c
    src/core/config_validation.c
    src/core/log.c
    src/core/call.c
    src/core/endpoint.c
    src/core/endpoint_settings.c
    src/core/endpoint_control.c
    src/core/endpoint_metrics.c
    src/core/endpoint_events.c
    src/core/endpoint_media.c
    src/core/endpoint_media_policy.c
    src/core/endpoint_source.c
    src/core/endpoint_media_session.c
    src/core/endpoint_status.c
    src/sip/adapter.c
    src/sip/adapter_media_control.c
    src/sip/adapter_events.c
    src/sip/syntax.c
    src/sip/media_control.c
    src/sip/driver.c
    src/sip/pjsip.c
    src/sip/crc_route.c
    src/sip/resolver.c
    src/sip/zoom.c
    src/sdp/session.c
    src/sdp/helpers.c
    src/sdp/crypto.c
    src/sdp/crypto_diagnostics.c
    src/sdp/codec_diagnostics.c
    src/sdp/validation.c
    src/sdp/parse.c
    src/sdp/feedback.c
    src/sdp/policy.c
    src/sdp/negotiation.c
    src/sdp/serialize.c
    src/rtp/session.c
    src/rtp/srtp.c
    src/rtp/transport.c
    src/rtp/wire.c
    src/rtp/pacing.c
    src/rtp/retransmit_private.c
    src/rtcp/session.c
    src/rtcp/framing.c
    src/rtcp/feedback.c
    src/rtcp/reporter.c
    src/media/h264.c
    src/media/h264_depacketizer.c
    src/media/h264_profile.c
    src/media/g711.c
    src/media/aac.c
    src/media/aec.c
    src/media/renderer.c
    src/media/receive_monitor.c
    src/media/receive_monitor_worker.c
    src/media/renderer_gate.c
    src/media/dtmf.c
    src/media/dtmf_state.c
    src/media/rx_shim.c
    src/media/rx_shim_accounting.c
    src/media/rx_shim_source.c
    src/media/rx_playout.c
    src/media/rx_render.c
    src/media/backend.c
    src/media/session.c
    src/media/session_prepare.c
    src/media/session_prepare_codecs.c
    src/media/session_prepare_finalize.c
    src/media/session_prepare_dtmf.c
    src/media/session_payloads.c
    src/media/session_pump.c
    src/media/session_feedback.c
    src/media/session_audio_pump.c
    src/media/session_poll.c
    src/media/session_lifecycle.c
    src/backends/fixture.c
    src/backends/rtsp_parser.c
    src/backends/rtsp_protocol_fixture.c
    src/backends/rtsp_native.c
    src/backends/rtsp_native_lifecycle.c
    src/backends/rtsp_native_reorder.c
    src/backends/rtsp_native_transport.c
    src/backends/rtsp_native_sdp.c
    src/platform/posix.c
    src/platform/child.c
    src/control/local.c
    src/control/framing.c
    src/control/dedupe_hash.c
    src/control/request_decode.c
    src/control/protocol.c)

set(AULA_SIPD_DAEMON_SOURCES src/main.c)

if(AULA_SIPD_BUILD_DAEMON)
  aula_sipd_require_sources("core" ${AULA_SIPD_CORE_SOURCES})
  aula_sipd_require_sources("daemon" ${AULA_SIPD_DAEMON_SOURCES})
  add_library(aula_sipd_core STATIC ${AULA_SIPD_CORE_SOURCES})
  target_link_libraries(aula_sipd_core PUBLIC aula_sipd::contract)
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # Resolve this system library through the configured toolchain/sysroot.
    target_link_libraries(aula_sipd_core PRIVATE resolv)
  endif()
  if(AULA_SIPD_BUILD_TESTS)
    # Keep deterministic persistence-fault injection out of release and ARM
    # payload binaries.  The matching declaration is visible only to the unit
    # target below.
    target_compile_definitions(aula_sipd_core PRIVATE AULA_SIPD_TEST_FAULTS=1)
  endif()
  if(AULA_SIPD_ENABLE_FAAD2 OR AULA_SIPD_ENABLE_SPEEXDSP)
    if(NOT AULA_SIPD_ENABLE_FAAD2
       OR NOT AULA_SIPD_ENABLE_SPEEXDSP
       OR NOT IS_DIRECTORY "${AULA_SIPD_FAAD2_INCLUDE_DIR}"
       OR NOT EXISTS "${AULA_SIPD_FAAD2_INCLUDE_DIR}/neaacdec.h"
       OR NOT EXISTS "${AULA_SIPD_FAAD2_LIBRARY}"
       OR NOT IS_DIRECTORY "${AULA_SIPD_SPEEXDSP_INCLUDE_DIR}"
       OR NOT EXISTS "${AULA_SIPD_SPEEXDSP_INCLUDE_DIR}/speex/speex_resampler.h"
       OR NOT EXISTS "${AULA_SIPD_SPEEXDSP_INCLUDE_DIR}/speex/speex_echo.h"
       OR NOT EXISTS "${AULA_SIPD_SPEEXDSP_LIBRARY}")
      message(
        FATAL_ERROR
          "Native AAC requires explicit pre-acquired FAAD2 2.11.2 and SpeexDSP 1.2.1 include directories and libraries; CMake never searches host packages or downloads dependencies"
      )
    endif()
    target_include_directories(
      aula_sipd_core PRIVATE "${AULA_SIPD_FAAD2_INCLUDE_DIR}"
                             "${AULA_SIPD_SPEEXDSP_INCLUDE_DIR}")
    target_link_libraries(
      aula_sipd_core PRIVATE "${AULA_SIPD_FAAD2_LIBRARY}"
                             "${AULA_SIPD_SPEEXDSP_LIBRARY}")
    target_compile_definitions(
      aula_sipd_core PRIVATE AULA_SIPD_HAVE_NATIVE_AAC=1
                             AULA_SIPD_HAVE_NATIVE_AEC=1)
  else()
    target_compile_definitions(
      aula_sipd_core PRIVATE AULA_SIPD_HAVE_NATIVE_AAC=0
                             AULA_SIPD_HAVE_NATIVE_AEC=0)
  endif()
  if(AULA_SIPD_ENABLE_SRTP)
    target_include_directories(aula_sipd_core SYSTEM
                               PRIVATE "${AULA_SIPD_SRTP_INCLUDE_DIR}")
    target_link_libraries(aula_sipd_core PRIVATE "${AULA_SIPD_SRTP_LIBRARY}")
    target_compile_definitions(aula_sipd_core PRIVATE AULA_SIPD_HAVE_SRTP=1)
  else()
    target_compile_definitions(aula_sipd_core PRIVATE AULA_SIPD_HAVE_SRTP=0)
  endif()
  if(AULA_SIPD_ENABLE_PJSIP)
    if(NOT IS_DIRECTORY "${AULA_SIPD_PJSIP_INCLUDE_DIR}"
       OR NOT EXISTS "${AULA_SIPD_PJSIP_INCLUDE_DIR}/pjsip.h"
       OR AULA_SIPD_PJSIP_LIBRARIES STREQUAL ""
       OR NOT EXISTS "${AULA_SIPD_PJSIP_UA_LIBRARY}"
       OR NOT EXISTS "${AULA_SIPD_PJMEDIA_LIBRARY}")
      message(
        FATAL_ERROR
          "PJSIP is opt-in: provide PJSIP headers, base libraries, explicit libpjsip-ua, and minimal libpjmedia; builds never download pjproject"
      )
    endif()
    # Treat reviewed third-party headers as system headers so the project's
    # stricter warning policy remains focused on owned adapter code.
    target_include_directories(aula_sipd_core SYSTEM
                               PRIVATE "${AULA_SIPD_PJSIP_INCLUDE_DIR}")
    # Source-tree builds keep sibling public include roots, while a reviewed
    # sysroot installs them below one pjproject root. Support both forms.
    get_filename_component(_aula_pjsip_source_root
                           "${AULA_SIPD_PJSIP_INCLUDE_DIR}/../.." ABSOLUTE)
    foreach(_aula_pjsip_component pjlib pjlib-util pjmedia)
      if(IS_DIRECTORY
         "${_aula_pjsip_source_root}/${_aula_pjsip_component}/include")
        target_include_directories(
          aula_sipd_core SYSTEM
          PRIVATE "${_aula_pjsip_source_root}/${_aula_pjsip_component}/include")
      endif()
    endforeach()
    if(CMAKE_C_COMPILER_ID MATCHES "Clang")
      target_compile_options(aula_sipd_core PRIVATE -Wno-typedef-redefinition)
    endif()
    target_link_libraries(
      aula_sipd_core
      PRIVATE "${AULA_SIPD_PJSIP_UA_LIBRARY}" ${AULA_SIPD_PJSIP_LIBRARIES}
              "${AULA_SIPD_PJMEDIA_LIBRARY}")
    # pjproject's supported configure build emits pj/compat/os_auto.h and
    # pj/compat/m_auto.h; consumers must select those generated headers too.
    target_compile_definitions(aula_sipd_core PRIVATE AULA_SIPD_HAVE_PJSIP=1
                                                      PJ_AUTOCONF=1)
  else()
    target_compile_definitions(aula_sipd_core PRIVATE AULA_SIPD_HAVE_PJSIP=0)
  endif()
  aula_sipd_apply_platform_libraries(aula_sipd_core)
  add_executable(aula-sipd ${AULA_SIPD_DAEMON_SOURCES})
  target_link_libraries(aula-sipd PRIVATE aula_sipd_core)
  aula_sipd_apply_platform_libraries(aula-sipd)
  install(TARGETS aula-sipd RUNTIME DESTINATION bin)
endif()

install(DIRECTORY include/ DESTINATION include)
install(
  FILES config/aula-sipd.example.conf
  DESTINATION share/aula-sipd
  RENAME aula-sipd.conf.example)

if(AULA_SIPD_BUILD_TESTS)
  set(AULA_SIPD_UNIT_TEST_SOURCES
      tests/unit/test_main.c
      tests/unit/test_contracts.c
      tests/unit/test_h264_packetization.c
      tests/unit/test_call_matrix.c
      tests/unit/test_dtmf_state.c
      tests/unit/test_rtcp_reporter.c
      tests/unit/test_rtcp_feedback.c
      tests/unit/test_media_feedback.c
      tests/unit/test_rtp_policy.c
      tests/unit/test_rtp_retransmit.c
      tests/unit/test_rtp_transport.c
      tests/unit/test_sdp_policy.c
      tests/unit/test_sdp_feedback.c
      tests/unit/test_sdp_negotiation.c
      tests/unit/test_sip_scope.c
      tests/unit/test_platform_child.c
      tests/unit/test_config_snapshot.c
      tests/unit/test_control_server.c
      tests/unit/test_endpoint.c
      tests/unit/test_endpoint_lifecycle.c
      tests/unit/test_sip_driver_conformance.c
      tests/unit/test_sip_transactions.c
      tests/unit/test_media_control.c
      tests/unit/test_zoom_dial.c
      tests/unit/test_pjsip_source_contract.c
      tests/unit/test_pjsip_uri_destination.c
      tests/unit/test_resolver.c
      tests/unit/test_resolver_libc.c
      tests/unit/test_rtsp_backend.c
      tests/unit/test_media_session.c
      tests/support/sip_driver_harness.c)
  set(AULA_SIPD_INTEGRATION_TEST_SOURCES
      tests/integration/test_fixture_call.c
      tests/integration/test_authorized_rebind.c)
  set(AULA_SIPD_OFFLINE_SIMULATOR_SOURCES
      tests/integration/test_offline_call_simulator.c
      tests/support/sip_driver_harness.c)
  set(AULA_SIPD_CORPUS_REPLAY_SOURCES tests/corpus_replay/test_corpus_replay.c)
  aula_sipd_require_sources("unit tests" ${AULA_SIPD_UNIT_TEST_SOURCES})
  aula_sipd_require_sources("integration tests"
                            ${AULA_SIPD_INTEGRATION_TEST_SOURCES})
  aula_sipd_require_sources("offline call simulator"
                            ${AULA_SIPD_OFFLINE_SIMULATOR_SOURCES})
  aula_sipd_require_sources("corpus replay tests"
                            ${AULA_SIPD_CORPUS_REPLAY_SOURCES})
  enable_testing()
  add_executable(
    aula-sipd-receive-monitor-unit
    tests/unit/test_receive_monitor.c src/media/receive_monitor.c
    src/media/receive_monitor_worker.c)
  target_link_libraries(aula-sipd-receive-monitor-unit
                        PRIVATE aula_sipd::contract)
  add_test(NAME aula-sipd.receive-monitor-unit
           COMMAND aula-sipd-receive-monitor-unit)
  add_executable(aula-sipd-unit ${AULA_SIPD_UNIT_TEST_SOURCES})
  target_link_libraries(aula-sipd-unit PRIVATE aula_sipd_core)
  target_compile_definitions(
    aula-sipd-unit PRIVATE AULA_SIPD_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
                           AULA_SIPD_TEST_FAULTS=1)
  aula_sipd_apply_platform_libraries(aula-sipd-unit)
  add_test(NAME aula-sipd.unit COMMAND aula-sipd-unit)
  add_executable(
    aula-sipd-srtp-sdp-unit
    tests/unit/test_sdp_srtp_enabled.c
    src/sdp/session.c
    src/sdp/helpers.c
    src/sdp/crypto.c
    src/sdp/crypto_diagnostics.c
    src/sdp/codec_diagnostics.c
    src/sdp/validation.c
    src/sdp/parse.c
    src/sdp/feedback.c
    src/sdp/policy.c
    src/sdp/negotiation.c
    src/media/h264_profile.c
    src/sdp/serialize.c)
  target_link_libraries(aula-sipd-srtp-sdp-unit PRIVATE aula_sipd::contract)
  target_compile_definitions(aula-sipd-srtp-sdp-unit
                             PRIVATE AULA_SIPD_HAVE_SRTP=1)
  add_test(NAME aula-sipd.srtp-sdp-unit COMMAND aula-sipd-srtp-sdp-unit)
  add_executable(
    aula-sipd-srtp-limit-unit tests/unit/test_srtp_limits.c
                              tests/support/srtp_stub.c src/rtp/srtp.c)
  target_include_directories(aula-sipd-srtp-limit-unit BEFORE
                             PRIVATE tests/support)
  target_link_libraries(aula-sipd-srtp-limit-unit PRIVATE aula_sipd::contract)
  target_compile_definitions(aula-sipd-srtp-limit-unit
                             PRIVATE AULA_SIPD_HAVE_SRTP=1)
  aula_sipd_apply_platform_libraries(aula-sipd-srtp-limit-unit)
  add_test(NAME aula-sipd.srtp-limit-unit COMMAND aula-sipd-srtp-limit-unit)
  add_executable(aula-sipd-integration ${AULA_SIPD_INTEGRATION_TEST_SOURCES})
  target_link_libraries(aula-sipd-integration PRIVATE aula_sipd_core)
  aula_sipd_apply_platform_libraries(aula-sipd-integration)
  add_test(NAME aula-sipd.integration COMMAND aula-sipd-integration)
  add_executable(aula-sipd-offline-call-simulator
                 ${AULA_SIPD_OFFLINE_SIMULATOR_SOURCES})
  target_link_libraries(aula-sipd-offline-call-simulator PRIVATE aula_sipd_core)
  aula_sipd_apply_platform_libraries(aula-sipd-offline-call-simulator)
  add_test(NAME aula-sipd.offline-call-simulator
           COMMAND aula-sipd-offline-call-simulator)
  add_test(NAME aula-sipd.fixture-call COMMAND aula-sipd --fixture-call)
  set(AULA_SIPD_CLI_CONTRACT_SCRIPT
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli/test_cli_contract.cmake")
  set(AULA_SIPD_HOST_TEST_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/host-test")
  file(MAKE_DIRECTORY "${AULA_SIPD_HOST_TEST_OUTPUT_DIR}")
  set(AULA_SIPD_HOST_TEST_CONFIG
      "${CMAKE_CURRENT_BINARY_DIR}/config/aula-sipd.host-test.conf")
  configure_file(config/aula-sipd.host-test.conf.in
                 "${AULA_SIPD_HOST_TEST_CONFIG}" @ONLY)
  foreach(cli_case IN ITEMS version self-test check-config invalid-and-usage
                            foreground-refusal)
    add_test(
      NAME "aula-sipd.cli-${cli_case}"
      COMMAND
        "${CMAKE_COMMAND}" "-DAULA_SIPD_BINARY=$<TARGET_FILE:aula-sipd>"
        "-DAULA_SIPD_HOST_CONFIG=${AULA_SIPD_HOST_TEST_CONFIG}"
        "-DAULA_SIPD_TEST_PJSIP=${AULA_SIPD_ENABLE_PJSIP}"
        "-DAULA_SIPD_CLI_CASE=${cli_case}" -P
        "${AULA_SIPD_CLI_CONTRACT_SCRIPT}")
  endforeach()
  add_executable(aula-sipd-corpus-replay ${AULA_SIPD_CORPUS_REPLAY_SOURCES})
  target_link_libraries(aula-sipd-corpus-replay PRIVATE aula_sipd_core)
  aula_sipd_apply_platform_libraries(aula-sipd-corpus-replay)
  # Corpus files are test data, so this inventory intentionally refreshes when a
  # seed is added.  The runner fails on an unclassified seed.
  file(
    GLOB_RECURSE AULA_SIPD_CORPUS_SEEDS CONFIGURE_DEPENDS
    RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/*")
  list(FILTER AULA_SIPD_CORPUS_SEEDS EXCLUDE REGEX "^SEED_MANIFEST\\.txt$")
  list(SORT AULA_SIPD_CORPUS_SEEDS)
  add_test(
    NAME aula-sipd.corpus-replay
    COMMAND
      aula-sipd-corpus-replay "${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus"
      ${AULA_SIPD_CORPUS_SEEDS})
  if(AULA_SIPD_ENABLE_PJSIP)
    add_executable(aula-sipd-pjsip-address tests/unit/test_pjsip_address.c)
    target_link_libraries(aula-sipd-pjsip-address PRIVATE aula_sipd_core)
    target_include_directories(
      aula-sipd-pjsip-address SYSTEM
      PRIVATE $<TARGET_PROPERTY:aula_sipd_core,INCLUDE_DIRECTORIES>)
    target_compile_definitions(aula-sipd-pjsip-address PRIVATE PJ_AUTOCONF=1)
    aula_sipd_apply_platform_libraries(aula-sipd-pjsip-address)
    add_test(NAME aula-sipd.pjsip-numeric-peer COMMAND aula-sipd-pjsip-address)
    find_package(
      Python3
      COMPONENTS Interpreter
      QUIET)
    if(Python3_Interpreter_FOUND)
      add_test(
        NAME aula-sipd.pjsip-private-lab
        COMMAND
          "${Python3_EXECUTABLE}"
          "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/test_pjsip_private_lab.py"
          "$<TARGET_FILE:aula-sipd>")
      set_tests_properties(aula-sipd.pjsip-private-lab PROPERTIES TIMEOUT 20)
      if(AULA_SIPD_ENABLE_PJSIP_TLS_TESTS)
        add_test(
          NAME aula-sipd.pjsip-tls-private-lab
          COMMAND
            "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/test_pjsip_tls_private_lab.py"
            "$<TARGET_FILE:aula-sipd>")
        set_tests_properties(aula-sipd.pjsip-tls-private-lab PROPERTIES TIMEOUT
                                                                        25)
        add_test(
          NAME aula-sipd.pjsip-proxy-registration
          COMMAND
            "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/test_pjsip_proxy_registration.py"
            "$<TARGET_FILE:aula-sipd>")
        set_tests_properties(aula-sipd.pjsip-proxy-registration
                             PROPERTIES TIMEOUT 25)
      endif()
    endif()
  endif()
endif()

if(AULA_SIPD_BUILD_FUZZ)
  set(AULA_SIPD_FUZZ_SOURCES
      tests/fuzz/fuzz_sip.c
      tests/fuzz/fuzz_sdp.c
      tests/fuzz/fuzz_rtp.c
      tests/fuzz/fuzz_rtcp.c
      tests/fuzz/fuzz_h264.c
      tests/fuzz/fuzz_rtsp.c
      tests/fuzz/fuzz_config.c)
  aula_sipd_require_sources("fuzz targets" ${AULA_SIPD_FUZZ_SOURCES})
  foreach(fuzz_source IN LISTS AULA_SIPD_FUZZ_SOURCES)
    get_filename_component(fuzz_target "${fuzz_source}" NAME_WE)
    add_executable(${fuzz_target} ${fuzz_source})
    target_link_libraries(${fuzz_target} PRIVATE aula_sipd_core)
    aula_sipd_apply_platform_libraries(${fuzz_target})
    aula_sipd_apply_fuzz_options(${fuzz_target})
  endforeach()
endif()

message(STATUS "aula-sipd target profile: ${AULA_SIPD_TARGET_PROFILE}")
message(STATUS "aula-sipd C standard: C99, extensions disabled")
