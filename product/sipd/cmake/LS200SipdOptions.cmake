include_guard(GLOBAL)

option(LS200_SIPD_BUILD_DAEMON
       "Build the daemon when all owned sources are present" ON)
option(LS200_SIPD_BUILD_TESTS "Build deterministic unit and integration tests"
       ON)
option(LS200_SIPD_BUILD_FUZZ "Build bounded parser fuzz targets" OFF)
option(LS200_SIPD_ENABLE_SANITIZERS
       "Enable host address and undefined behavior sanitizers" OFF)
option(LS200_SIPD_ENABLE_COVERAGE
       "Enable host compiler coverage instrumentation" OFF)
option(LS200_SIPD_ENABLE_PJSIP
       "Link a separately acquired, reviewed pjproject source build" OFF)
option(LS200_SIPD_ENABLE_PJSIP_TLS_TESTS
       "Run loopback TLS identity tests for a TLS-capable PJSIP build" OFF)
option(LS200_SIPD_ENABLE_FAAD2
       "Link an explicitly supplied FAAD2 2.11.2 AAC-LC decoder" OFF)
option(LS200_SIPD_ENABLE_SPEEXDSP
       "Link an explicitly supplied SpeexDSP 1.2.1 resampler/AEC" OFF)
option(LS200_SIPD_ENABLE_SRTP
       "Link an explicitly supplied libsrtp for SDES-SRTP transport protection"
       OFF)
set(LS200_SIPD_FAAD2_INCLUDE_DIR
    ""
    CACHE PATH
          "Existing FAAD2 2.11.2 include root; no package discovery or download"
)
set(LS200_SIPD_FAAD2_LIBRARY
    ""
    CACHE FILEPATH "Existing FAAD2 2.11.2 library; required only when enabled")
set(LS200_SIPD_SPEEXDSP_INCLUDE_DIR
    ""
    CACHE
      PATH
      "Existing SpeexDSP 1.2.1 include root; no package discovery or download")
set(LS200_SIPD_SPEEXDSP_LIBRARY
    ""
    CACHE FILEPATH
          "Existing SpeexDSP 1.2.1 library; required only when enabled")
set(LS200_SIPD_SRTP_INCLUDE_DIR
    ""
    CACHE
      PATH
      "Existing libsrtp include root; required only when LS200_SIPD_ENABLE_SRTP=ON"
)
set(LS200_SIPD_SRTP_LIBRARY
    ""
    CACHE
      FILEPATH
      "Existing libsrtp library; required only when LS200_SIPD_ENABLE_SRTP=ON")
set(LS200_SIPD_PJSIP_INCLUDE_DIR
    ""
    CACHE
      PATH
      "Existing pjproject include root; required only when LS200_SIPD_ENABLE_PJSIP=ON"
)
set(LS200_SIPD_PJSIP_LIBRARIES
    ""
    CACHE
      STRING
      "Existing pjproject libraries in link order; required only when LS200_SIPD_ENABLE_PJSIP=ON"
)
set(LS200_SIPD_PJSIP_UA_LIBRARY
    ""
    CACHE FILEPATH
          "Exact libpjsip-ua archive required for the PJSIP invite/timer usage")
set(LS200_SIPD_PJMEDIA_LIBRARY
    ""
    CACHE
      FILEPATH
      "Exact minimal libpjmedia archive required for PJSIP invite SDP handling")
set(LS200_SIPD_TARGET_PROFILE
    "host"
    CACHE STRING "Build profile: host or ls200-arm-eabi5")
set_property(CACHE LS200_SIPD_TARGET_PROFILE PROPERTY STRINGS host
                                                      ls200-arm-eabi5)

if(LS200_SIPD_ENABLE_PJSIP_TLS_TESTS AND NOT LS200_SIPD_ENABLE_PJSIP)
  message(FATAL_ERROR "PJSIP TLS tests require LS200_SIPD_ENABLE_PJSIP=ON")
endif()

if(LS200_SIPD_ENABLE_SRTP
   AND (NOT IS_DIRECTORY "${LS200_SIPD_SRTP_INCLUDE_DIR}"
        OR NOT EXISTS "${LS200_SIPD_SRTP_INCLUDE_DIR}/srtp2/srtp.h"
        OR NOT EXISTS "${LS200_SIPD_SRTP_LIBRARY}"))
  message(
    FATAL_ERROR
      "SRTP is opt-in: provide pre-acquired LS200_SIPD_SRTP_INCLUDE_DIR containing srtp2/srtp.h and LS200_SIPD_SRTP_LIBRARY; builds never search host packages or download dependencies"
  )
endif()

if(NOT LS200_SIPD_TARGET_PROFILE STREQUAL "host"
   AND NOT LS200_SIPD_TARGET_PROFILE STREQUAL "ls200-arm-eabi5")
  message(
    FATAL_ERROR "LS200_SIPD_TARGET_PROFILE must be host or ls200-arm-eabi5")
endif()

add_library(ls200_sipd_build_options INTERFACE)
add_library(ls200_sipd::build_options ALIAS ls200_sipd_build_options)
target_compile_features(ls200_sipd_build_options INTERFACE c_std_99)
set_target_properties(ls200_sipd_build_options PROPERTIES C_EXTENSIONS OFF)

if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(
    ls200_sipd_build_options
    INTERFACE -Wall
              -Wextra
              -Wpedantic
              -Werror
              -Wcast-align
              -Wconversion
              -Wformat=2
              -Wmissing-prototypes
              -Wshadow
              -Wstrict-prototypes
              -Wundef)
elseif(MSVC)
  target_compile_options(ls200_sipd_build_options INTERFACE /W4 /WX)
else()
  message(WARNING "Unknown C compiler: strict warning policy must be reviewed")
endif()

if(LS200_SIPD_TARGET_PROFILE STREQUAL "ls200-arm-eabi5")
  # ARM attributes, dynamic loader, GLIBC ceiling, and syscall validation are
  # enforced by the later ARM ABI gate, never guessed from this host build.
  target_compile_definitions(ls200_sipd_build_options
                             INTERFACE LS200_SIPD_TARGET_ARM_EABI5=1)
endif()

if(LS200_SIPD_ENABLE_SANITIZERS AND NOT LS200_SIPD_TARGET_PROFILE STREQUAL
                                    "host")
  message(FATAL_ERROR "Sanitizers are a host-only gate")
endif()

if(LS200_SIPD_ENABLE_COVERAGE AND NOT LS200_SIPD_TARGET_PROFILE STREQUAL "host")
  message(FATAL_ERROR "Coverage instrumentation is a host-only gate")
endif()

if(LS200_SIPD_ENABLE_COVERAGE)
  if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    message(FATAL_ERROR "Coverage instrumentation requires Clang or GNU C")
  endif()
  if(CMAKE_C_COMPILER_ID MATCHES "Clang")
    # Keep Clang's profile data separate from GNU's gcda format.  The host
    # coverage runner merges the resulting profraw files with llvm-profdata.
    target_compile_options(
      ls200_sipd_build_options INTERFACE -fprofile-instr-generate
                                         -fcoverage-mapping)
    target_link_options(ls200_sipd_build_options INTERFACE
                        -fprofile-instr-generate)
  else()
    target_compile_options(ls200_sipd_build_options INTERFACE --coverage)
    target_link_options(ls200_sipd_build_options INTERFACE --coverage)
  endif()
endif()

if(LS200_SIPD_BUILD_FUZZ)
  if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "Fuzz targets require a Clang/libFuzzer toolchain")
  endif()
  include(CheckCSourceCompiles)
  set(_ls200_saved_required_flags "${CMAKE_REQUIRED_FLAGS}")
  set(_ls200_saved_required_link_options "${CMAKE_REQUIRED_LINK_OPTIONS}")
  set(CMAKE_REQUIRED_FLAGS
      "${CMAKE_REQUIRED_FLAGS} -fsanitize=fuzzer,address,undefined")
  set(CMAKE_REQUIRED_LINK_OPTIONS -fsanitize=fuzzer,address,undefined)
  check_c_source_compiles(
    "#include <stddef.h>\n#include <stdint.h>\nint LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {(void)data; (void)size; return 0;}"
    LS200_SIPD_HAVE_LIBFUZZER)
  set(CMAKE_REQUIRED_FLAGS "${_ls200_saved_required_flags}")
  set(CMAKE_REQUIRED_LINK_OPTIONS "${_ls200_saved_required_link_options}")
  unset(_ls200_saved_required_flags)
  unset(_ls200_saved_required_link_options)
  if(NOT LS200_SIPD_HAVE_LIBFUZZER)
    message(
      FATAL_ERROR
        "LS200_SIPD_BUILD_FUZZ requires a linkable Clang libFuzzer, ASan, and UBSan runtime"
    )
  endif()
endif()

if((LS200_SIPD_BUILD_TESTS OR LS200_SIPD_BUILD_FUZZ)
   AND NOT LS200_SIPD_BUILD_DAEMON)
  message(
    FATAL_ERROR "Tests and fuzz targets require LS200_SIPD_BUILD_DAEMON=ON")
endif()

function(ls200_sipd_require_sources label)
  foreach(source_file IN LISTS ARGN)
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${source_file}")
      message(FATAL_ERROR "Missing ${label} source: ${source_file}")
    endif()
  endforeach()
endfunction()

function(ls200_sipd_apply_fuzz_options target_name)
  if(CMAKE_C_COMPILER_ID MATCHES "Clang")
    target_compile_options(${target_name}
                           PRIVATE -fsanitize=fuzzer,address,undefined)
    target_link_options(${target_name} PRIVATE
                        -fsanitize=fuzzer,address,undefined)
  else()
    message(FATAL_ERROR "Fuzz targets require a Clang/libFuzzer toolchain")
  endif()
endfunction()
