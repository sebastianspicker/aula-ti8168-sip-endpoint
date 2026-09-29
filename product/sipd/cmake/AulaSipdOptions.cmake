include_guard(GLOBAL)

option(AULA_SIPD_BUILD_DAEMON
       "Build the daemon when all owned sources are present" ON)
option(AULA_SIPD_ENABLE_PJSIP
       "Link a separately acquired, reviewed pjproject source build" OFF)
option(AULA_SIPD_ENABLE_FAAD2
       "Link an explicitly supplied FAAD2 2.11.2 AAC-LC decoder" OFF)
option(AULA_SIPD_ENABLE_SPEEXDSP
       "Link an explicitly supplied SpeexDSP 1.2.1 resampler/AEC" OFF)
option(AULA_SIPD_ENABLE_SRTP
       "Link an explicitly supplied libsrtp for SDES-SRTP transport protection"
       OFF)
set(AULA_SIPD_FAAD2_INCLUDE_DIR
    ""
    CACHE PATH
          "Existing FAAD2 2.11.2 include root; no package discovery or download"
)
set(AULA_SIPD_FAAD2_LIBRARY
    ""
    CACHE FILEPATH "Existing FAAD2 2.11.2 library; required only when enabled")
set(AULA_SIPD_SPEEXDSP_INCLUDE_DIR
    ""
    CACHE
      PATH
      "Existing SpeexDSP 1.2.1 include root; no package discovery or download")
set(AULA_SIPD_SPEEXDSP_LIBRARY
    ""
    CACHE FILEPATH
          "Existing SpeexDSP 1.2.1 library; required only when enabled")
set(AULA_SIPD_SRTP_INCLUDE_DIR
    ""
    CACHE
      PATH
      "Existing libsrtp include root; required only when AULA_SIPD_ENABLE_SRTP=ON"
)
set(AULA_SIPD_SRTP_LIBRARY
    ""
    CACHE
      FILEPATH
      "Existing libsrtp library; required only when AULA_SIPD_ENABLE_SRTP=ON")
set(AULA_SIPD_PJSIP_INCLUDE_DIR
    ""
    CACHE
      PATH
      "Existing pjproject include root; required only when AULA_SIPD_ENABLE_PJSIP=ON"
)
set(AULA_SIPD_PJSIP_LIBRARIES
    ""
    CACHE
      STRING
      "Existing pjproject libraries in link order; required only when AULA_SIPD_ENABLE_PJSIP=ON"
)
set(AULA_SIPD_PJSIP_UA_LIBRARY
    ""
    CACHE FILEPATH
          "Exact libpjsip-ua archive required for the PJSIP invite/timer usage")
set(AULA_SIPD_PJMEDIA_LIBRARY
    ""
    CACHE
      FILEPATH
      "Exact minimal libpjmedia archive required for PJSIP invite SDP handling")
set(AULA_SIPD_TARGET_PROFILE
    "host"
    CACHE STRING "Build profile: host or aula-arm-eabi5")
set_property(CACHE AULA_SIPD_TARGET_PROFILE PROPERTY STRINGS host
                                                     aula-arm-eabi5)

if(AULA_SIPD_ENABLE_SRTP
   AND (NOT IS_DIRECTORY "${AULA_SIPD_SRTP_INCLUDE_DIR}"
        OR NOT EXISTS "${AULA_SIPD_SRTP_INCLUDE_DIR}/srtp2/srtp.h"
        OR NOT EXISTS "${AULA_SIPD_SRTP_LIBRARY}"))
  message(
    FATAL_ERROR
      "SRTP is opt-in: provide pre-acquired AULA_SIPD_SRTP_INCLUDE_DIR containing srtp2/srtp.h and AULA_SIPD_SRTP_LIBRARY; builds never search host packages or download dependencies"
  )
endif()

if(NOT AULA_SIPD_TARGET_PROFILE STREQUAL "host"
   AND NOT AULA_SIPD_TARGET_PROFILE STREQUAL "aula-arm-eabi5")
  message(FATAL_ERROR "AULA_SIPD_TARGET_PROFILE must be host or aula-arm-eabi5")
endif()

add_library(aula_sipd_build_options INTERFACE)
add_library(aula_sipd::build_options ALIAS aula_sipd_build_options)
target_compile_features(aula_sipd_build_options INTERFACE c_std_99)
set_target_properties(aula_sipd_build_options PROPERTIES C_EXTENSIONS OFF)

if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(
    aula_sipd_build_options
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
  target_compile_options(aula_sipd_build_options INTERFACE /W4 /WX)
else()
  message(WARNING "Unknown C compiler: strict warning policy must be reviewed")
endif()

if(AULA_SIPD_TARGET_PROFILE STREQUAL "aula-arm-eabi5")
  # ARM attributes, dynamic loader, GLIBC ceiling, and syscall validation are
  # enforced by the later ARM ABI gate, never guessed from this host build.
  target_compile_definitions(aula_sipd_build_options
                             INTERFACE AULA_SIPD_TARGET_ARM_EABI5=1)
endif()

function(aula_sipd_require_sources label)
  foreach(source_file IN LISTS ARGN)
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${source_file}")
      message(FATAL_ERROR "Missing ${label} source: ${source_file}")
    endif()
  endforeach()
endfunction()
