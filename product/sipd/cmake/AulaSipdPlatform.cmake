include_guard(GLOBAL)
include(CheckSymbolExists)

check_symbol_exists(clock_gettime "time.h" AULA_SIPD_HAVE_CLOCK_GETTIME)
check_symbol_exists(getrandom "sys/random.h" AULA_SIPD_HAVE_GETRANDOM)
check_symbol_exists(posix_spawn "spawn.h" AULA_SIPD_HAVE_POSIX_SPAWN)

function(aula_sipd_apply_platform_libraries target_name)
  if(UNIX)
    find_package(Threads REQUIRED)
    target_link_libraries(${target_name} PRIVATE Threads::Threads m)
    if(NOT AULA_SIPD_HAVE_CLOCK_GETTIME)
      target_link_libraries(${target_name} PRIVATE rt)
    endif()
  endif()

  target_compile_definitions(
    ${target_name}
    PRIVATE AULA_SIPD_HAVE_CLOCK_GETTIME=$<BOOL:${AULA_SIPD_HAVE_CLOCK_GETTIME}>
            AULA_SIPD_HAVE_GETRANDOM=$<BOOL:${AULA_SIPD_HAVE_GETRANDOM}>
            AULA_SIPD_HAVE_POSIX_SPAWN=$<BOOL:${AULA_SIPD_HAVE_POSIX_SPAWN}>)
endfunction()
