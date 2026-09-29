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


message(STATUS "aula-sipd target profile: ${AULA_SIPD_TARGET_PROFILE}")
message(STATUS "aula-sipd C standard: C99, extensions disabled")
