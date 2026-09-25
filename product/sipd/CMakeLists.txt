cmake_minimum_required(VERSION 3.16)

project(
  ls200_sipd
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

include(cmake/LS200SipdOptions.cmake)
include(cmake/LS200SipdPlatform.cmake)

add_library(ls200_sipd_contract INTERFACE)
add_library(ls200_sipd::contract ALIAS ls200_sipd_contract)
target_include_directories(
  ls200_sipd_contract
  INTERFACE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>)
target_compile_features(ls200_sipd_contract INTERFACE c_std_99)
target_link_libraries(ls200_sipd_contract INTERFACE ls200_sipd_build_options)

# This is an explicit inventory, not a glob. Missing owned source files make
# configuration fail rather than silently yielding a partial daemon.
set(LS200_SIPD_CORE_SOURCES
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
    src/control/protocol.c
    src/control/ls200_bridge.c)

set(LS200_SIPD_DAEMON_SOURCES src/main.c)

if(LS200_SIPD_BUILD_DAEMON)
  ls200_sipd_require_sources("core" ${LS200_SIPD_CORE_SOURCES})
  ls200_sipd_require_sources("daemon" ${LS200_SIPD_DAEMON_SOURCES})
  add_library(ls200_sipd_core STATIC ${LS200_SIPD_CORE_SOURCES})
  target_link_libraries(ls200_sipd_core PUBLIC ls200_sipd::contract)
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # Resolve this system library through the configured toolchain/sysroot.
    target_link_libraries(ls200_sipd_core PRIVATE resolv)
  endif()
  if(LS200_SIPD_BUILD_TESTS)
    # Keep deterministic persistence-fault injection out of release and ARM
    # payload binaries.  The matching declaration is visible only to the unit
    # target below.
    target_compile_definitions(ls200_sipd_core PRIVATE LS200_SIPD_TEST_FAULTS=1)
  endif()
  if(LS200_SIPD_ENABLE_FAAD2 OR LS200_SIPD_ENABLE_SPEEXDSP)
    if(NOT LS200_SIPD_ENABLE_FAAD2
       OR NOT LS200_SIPD_ENABLE_SPEEXDSP
       OR NOT IS_DIRECTORY "${LS200_SIPD_FAAD2_INCLUDE_DIR}"
       OR NOT EXISTS "${LS200_SIPD_FAAD2_INCLUDE_DIR}/neaacdec.h"
       OR NOT EXISTS "${LS200_SIPD_FAAD2_LIBRARY}"
       OR NOT IS_DIRECTORY "${LS200_SIPD_SPEEXDSP_INCLUDE_DIR}"
       OR NOT EXISTS
          "${LS200_SIPD_SPEEXDSP_INCLUDE_DIR}/speex/speex_resampler.h"
       OR NOT EXISTS "${LS200_SIPD_SPEEXDSP_INCLUDE_DIR}/speex/speex_echo.h"
       OR NOT EXISTS "${LS200_SIPD_SPEEXDSP_LIBRARY}")
      message(
        FATAL_ERROR
          "Native AAC requires explicit pre-acquired FAAD2 2.11.2 and SpeexDSP 1.2.1 include directories and libraries; CMake never searches host packages or downloads dependencies"
      )
    endif()
    target_include_directories(
      ls200_sipd_core PRIVATE "${LS200_SIPD_FAAD2_INCLUDE_DIR}"
                              "${LS200_SIPD_SPEEXDSP_INCLUDE_DIR}")
    target_link_libraries(
      ls200_sipd_core PRIVATE "${LS200_SIPD_FAAD2_LIBRARY}"
                              "${LS200_SIPD_SPEEXDSP_LIBRARY}")
    target_compile_definitions(
      ls200_sipd_core PRIVATE LS200_SIPD_HAVE_NATIVE_AAC=1
                              LS200_SIPD_HAVE_NATIVE_AEC=1)
  else()
    target_compile_definitions(
      ls200_sipd_core PRIVATE LS200_SIPD_HAVE_NATIVE_AAC=0
                              LS200_SIPD_HAVE_NATIVE_AEC=0)
  endif()
  if(LS200_SIPD_ENABLE_SRTP)
    target_include_directories(ls200_sipd_core SYSTEM
                               PRIVATE "${LS200_SIPD_SRTP_INCLUDE_DIR}")
    target_link_libraries(ls200_sipd_core PRIVATE "${LS200_SIPD_SRTP_LIBRARY}")
    target_compile_definitions(ls200_sipd_core PRIVATE LS200_SIPD_HAVE_SRTP=1)
  else()
    target_compile_definitions(ls200_sipd_core PRIVATE LS200_SIPD_HAVE_SRTP=0)
  endif()
  if(LS200_SIPD_ENABLE_PJSIP)
    if(NOT IS_DIRECTORY "${LS200_SIPD_PJSIP_INCLUDE_DIR}"
       OR NOT EXISTS "${LS200_SIPD_PJSIP_INCLUDE_DIR}/pjsip.h"
       OR LS200_SIPD_PJSIP_LIBRARIES STREQUAL ""
       OR NOT EXISTS "${LS200_SIPD_PJSIP_UA_LIBRARY}"
       OR NOT EXISTS "${LS200_SIPD_PJMEDIA_LIBRARY}")
      message(
        FATAL_ERROR
          "PJSIP is opt-in: provide PJSIP headers, base libraries, explicit libpjsip-ua, and minimal libpjmedia; builds never download pjproject"
      )
    endif()
    # Treat reviewed third-party headers as system headers so the project's
    # stricter warning policy remains focused on owned adapter code.
    target_include_directories(ls200_sipd_core SYSTEM
                               PRIVATE "${LS200_SIPD_PJSIP_INCLUDE_DIR}")
    # Source-tree builds keep sibling public include roots, while a reviewed
    # sysroot installs them below one pjproject root. Support both forms.
    get_filename_component(_ls200_pjsip_source_root
                           "${LS200_SIPD_PJSIP_INCLUDE_DIR}/../.." ABSOLUTE)
    foreach(_ls200_pjsip_component pjlib pjlib-util pjmedia)
      if(IS_DIRECTORY
         "${_ls200_pjsip_source_root}/${_ls200_pjsip_component}/include")
        target_include_directories(
          ls200_sipd_core SYSTEM
          PRIVATE
            "${_ls200_pjsip_source_root}/${_ls200_pjsip_component}/include")
      endif()
    endforeach()
    if(CMAKE_C_COMPILER_ID MATCHES "Clang")
      target_compile_options(ls200_sipd_core PRIVATE -Wno-typedef-redefinition)
    endif()
    target_link_libraries(
      ls200_sipd_core
      PRIVATE "${LS200_SIPD_PJSIP_UA_LIBRARY}" ${LS200_SIPD_PJSIP_LIBRARIES}
              "${LS200_SIPD_PJMEDIA_LIBRARY}")
    # pjproject's supported configure build emits pj/compat/os_auto.h and
    # pj/compat/m_auto.h; consumers must select those generated headers too.
    target_compile_definitions(ls200_sipd_core PRIVATE LS200_SIPD_HAVE_PJSIP=1
                                                       PJ_AUTOCONF=1)
  else()
    target_compile_definitions(ls200_sipd_core PRIVATE LS200_SIPD_HAVE_PJSIP=0)
  endif()
  ls200_sipd_apply_platform_libraries(ls200_sipd_core)
  add_executable(ls200-sipd ${LS200_SIPD_DAEMON_SOURCES})
  target_link_libraries(ls200-sipd PRIVATE ls200_sipd_core)
  ls200_sipd_apply_platform_libraries(ls200-sipd)
  install(TARGETS ls200-sipd RUNTIME DESTINATION bin)
endif()

install(DIRECTORY include/ DESTINATION include)
install(
  FILES config/ls200-sipd.example.conf
  DESTINATION share/ls200-sipd
  RENAME ls200-sipd.conf.example)

if(LS200_SIPD_BUILD_TESTS)
  set(LS200_SIPD_UNIT_TEST_SOURCES
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
      tests/unit/test_ls200_bridge.c
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
  set(LS200_SIPD_INTEGRATION_TEST_SOURCES
      tests/integration/test_fixture_call.c
      tests/integration/test_authorized_rebind.c)
  set(LS200_SIPD_OFFLINE_SIMULATOR_SOURCES
      tests/integration/test_offline_call_simulator.c
      tests/support/sip_driver_harness.c)
  set(LS200_SIPD_CORPUS_REPLAY_SOURCES tests/corpus_replay/test_corpus_replay.c)
  ls200_sipd_require_sources("unit tests" ${LS200_SIPD_UNIT_TEST_SOURCES})
  ls200_sipd_require_sources("integration tests"
                             ${LS200_SIPD_INTEGRATION_TEST_SOURCES})
  ls200_sipd_require_sources("offline call simulator"
                             ${LS200_SIPD_OFFLINE_SIMULATOR_SOURCES})
  ls200_sipd_require_sources("corpus replay tests"
                             ${LS200_SIPD_CORPUS_REPLAY_SOURCES})
  enable_testing()
  add_executable(
    ls200-sipd-receive-monitor-unit
    tests/unit/test_receive_monitor.c src/media/receive_monitor.c
    src/media/receive_monitor_worker.c)
  target_link_libraries(ls200-sipd-receive-monitor-unit
                        PRIVATE ls200_sipd::contract)
  add_test(NAME ls200-sipd.receive-monitor-unit
           COMMAND ls200-sipd-receive-monitor-unit)
  add_executable(ls200-sipd-unit ${LS200_SIPD_UNIT_TEST_SOURCES})
  target_link_libraries(ls200-sipd-unit PRIVATE ls200_sipd_core)
  target_compile_definitions(
    ls200-sipd-unit PRIVATE LS200_SIPD_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
                            LS200_SIPD_TEST_FAULTS=1)
  ls200_sipd_apply_platform_libraries(ls200-sipd-unit)
  add_test(NAME ls200-sipd.unit COMMAND ls200-sipd-unit)
  add_executable(
    ls200-sipd-srtp-sdp-unit
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
  target_link_libraries(ls200-sipd-srtp-sdp-unit PRIVATE ls200_sipd::contract)
  target_compile_definitions(ls200-sipd-srtp-sdp-unit
                             PRIVATE LS200_SIPD_HAVE_SRTP=1)
  add_test(NAME ls200-sipd.srtp-sdp-unit COMMAND ls200-sipd-srtp-sdp-unit)
  add_executable(
    ls200-sipd-srtp-limit-unit tests/unit/test_srtp_limits.c
                               tests/support/srtp_stub.c src/rtp/srtp.c)
  target_include_directories(ls200-sipd-srtp-limit-unit BEFORE
                             PRIVATE tests/support)
  target_link_libraries(ls200-sipd-srtp-limit-unit PRIVATE ls200_sipd::contract)
  target_compile_definitions(ls200-sipd-srtp-limit-unit
                             PRIVATE LS200_SIPD_HAVE_SRTP=1)
  ls200_sipd_apply_platform_libraries(ls200-sipd-srtp-limit-unit)
  add_test(NAME ls200-sipd.srtp-limit-unit COMMAND ls200-sipd-srtp-limit-unit)
  add_executable(ls200-sipd-integration ${LS200_SIPD_INTEGRATION_TEST_SOURCES})
  target_link_libraries(ls200-sipd-integration PRIVATE ls200_sipd_core)
  ls200_sipd_apply_platform_libraries(ls200-sipd-integration)
  add_test(NAME ls200-sipd.integration COMMAND ls200-sipd-integration)
  add_executable(ls200-sipd-offline-call-simulator
                 ${LS200_SIPD_OFFLINE_SIMULATOR_SOURCES})
  target_link_libraries(ls200-sipd-offline-call-simulator
                        PRIVATE ls200_sipd_core)
  ls200_sipd_apply_platform_libraries(ls200-sipd-offline-call-simulator)
  add_test(NAME ls200-sipd.offline-call-simulator
           COMMAND ls200-sipd-offline-call-simulator)
  add_test(NAME ls200-sipd.fixture-call COMMAND ls200-sipd --fixture-call)
  set(LS200_SIPD_CLI_CONTRACT_SCRIPT
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli/test_cli_contract.cmake")
  set(LS200_SIPD_HOST_TEST_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/host-test")
  file(MAKE_DIRECTORY "${LS200_SIPD_HOST_TEST_OUTPUT_DIR}")
  set(LS200_SIPD_HOST_TEST_CONFIG
      "${CMAKE_CURRENT_BINARY_DIR}/config/ls200-sipd.host-test.conf")
  configure_file(config/ls200-sipd.host-test.conf.in
                 "${LS200_SIPD_HOST_TEST_CONFIG}" @ONLY)
  foreach(cli_case IN ITEMS version self-test check-config invalid-and-usage
                            foreground-refusal)
    add_test(
      NAME "ls200-sipd.cli-${cli_case}"
      COMMAND
        "${CMAKE_COMMAND}" "-DLS200_SIPD_BINARY=$<TARGET_FILE:ls200-sipd>"
        "-DLS200_SIPD_HOST_CONFIG=${LS200_SIPD_HOST_TEST_CONFIG}"
        "-DLS200_SIPD_TEST_PJSIP=${LS200_SIPD_ENABLE_PJSIP}"
        "-DLS200_SIPD_CLI_CASE=${cli_case}" -P
        "${LS200_SIPD_CLI_CONTRACT_SCRIPT}")
  endforeach()
  add_executable(ls200-sipd-corpus-replay ${LS200_SIPD_CORPUS_REPLAY_SOURCES})
  target_link_libraries(ls200-sipd-corpus-replay PRIVATE ls200_sipd_core)
  ls200_sipd_apply_platform_libraries(ls200-sipd-corpus-replay)
  # Corpus files are test data, so this inventory intentionally refreshes when a
  # seed is added.  The runner fails on an unclassified seed.
  file(
    GLOB_RECURSE LS200_SIPD_CORPUS_SEEDS CONFIGURE_DEPENDS
    RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/*")
  list(FILTER LS200_SIPD_CORPUS_SEEDS EXCLUDE REGEX "^SEED_MANIFEST\\.txt$")
  list(SORT LS200_SIPD_CORPUS_SEEDS)
  add_test(
    NAME ls200-sipd.corpus-replay
    COMMAND
      ls200-sipd-corpus-replay "${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus"
      ${LS200_SIPD_CORPUS_SEEDS})
  if(LS200_SIPD_ENABLE_PJSIP)
    add_executable(ls200-sipd-pjsip-address tests/unit/test_pjsip_address.c)
    target_link_libraries(ls200-sipd-pjsip-address PRIVATE ls200_sipd_core)
    target_include_directories(
      ls200-sipd-pjsip-address SYSTEM
      PRIVATE $<TARGET_PROPERTY:ls200_sipd_core,INCLUDE_DIRECTORIES>)
    target_compile_definitions(ls200-sipd-pjsip-address PRIVATE PJ_AUTOCONF=1)
    ls200_sipd_apply_platform_libraries(ls200-sipd-pjsip-address)
    add_test(NAME ls200-sipd.pjsip-numeric-peer
             COMMAND ls200-sipd-pjsip-address)
    find_package(
      Python3
      COMPONENTS Interpreter
      QUIET)
    if(Python3_Interpreter_FOUND)
      add_test(
        NAME ls200-sipd.pjsip-private-lab
        COMMAND
          "${Python3_EXECUTABLE}"
          "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/test_pjsip_private_lab.py"
          "$<TARGET_FILE:ls200-sipd>")
      set_tests_properties(ls200-sipd.pjsip-private-lab PROPERTIES TIMEOUT 20)
      if(LS200_SIPD_ENABLE_PJSIP_TLS_TESTS)
        add_test(
          NAME ls200-sipd.pjsip-tls-private-lab
          COMMAND
            "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/test_pjsip_tls_private_lab.py"
            "$<TARGET_FILE:ls200-sipd>")
        set_tests_properties(ls200-sipd.pjsip-tls-private-lab PROPERTIES TIMEOUT
                                                                         25)
        add_test(
          NAME ls200-sipd.pjsip-proxy-registration
          COMMAND
            "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/test_pjsip_proxy_registration.py"
            "$<TARGET_FILE:ls200-sipd>")
        set_tests_properties(ls200-sipd.pjsip-proxy-registration
                             PROPERTIES TIMEOUT 25)
      endif()
    endif()
  endif()
endif()

if(LS200_SIPD_BUILD_FUZZ)
  set(LS200_SIPD_FUZZ_SOURCES
      tests/fuzz/fuzz_sip.c
      tests/fuzz/fuzz_sdp.c
      tests/fuzz/fuzz_rtp.c
      tests/fuzz/fuzz_rtcp.c
      tests/fuzz/fuzz_h264.c
      tests/fuzz/fuzz_rtsp.c
      tests/fuzz/fuzz_config.c)
  ls200_sipd_require_sources("fuzz targets" ${LS200_SIPD_FUZZ_SOURCES})
  foreach(fuzz_source IN LISTS LS200_SIPD_FUZZ_SOURCES)
    get_filename_component(fuzz_target "${fuzz_source}" NAME_WE)
    add_executable(${fuzz_target} ${fuzz_source})
    target_link_libraries(${fuzz_target} PRIVATE ls200_sipd_core)
    ls200_sipd_apply_platform_libraries(${fuzz_target})
    ls200_sipd_apply_fuzz_options(${fuzz_target})
  endforeach()
endif()

message(STATUS "ls200-sipd target profile: ${LS200_SIPD_TARGET_PROFILE}")
message(STATUS "ls200-sipd C standard: C99, extensions disabled")
