# BearSSL and the TLS client for host builds: vantage_tls holds user/lib/tls.c
# and the virtual network's HTTPS server (sim/platform/tls_peer.c), which also
# supplies the simulator's trust anchor. Kernel builds configure BearSSL in the
# Makefile; keep the BR_* settings below in step with it.
include("${CMAKE_CURRENT_LIST_DIR}/VantageHost.cmake")
include_guard(GLOBAL)

find_package(Python3 3.11 REQUIRED COMPONENTS Interpreter)
set(BEARSSL_DIR "" CACHE PATH "Local source tree at the pinned BearSSL revision")
set(bearssl_fetch "${VANTAGE_ROOT}/tools/fetch_bearssl.py")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${bearssl_fetch}" "${VANTAGE_ROOT}/tools/bearssl-pin.mk")
execute_process(COMMAND "${Python3_EXECUTABLE}" -B "${bearssl_fetch}" "--source-dir=${BEARSSL_DIR}"
  OUTPUT_VARIABLE bearssl_source OUTPUT_STRIP_TRAILING_WHITESPACE
  RESULT_VARIABLE bearssl_result)
if(NOT bearssl_result STREQUAL "0")
  message(FATAL_ERROR "BearSSL provisioning failed (${bearssl_result}). See docs/TLS.md.")
endif()

file(GLOB_RECURSE VANTAGE_BEARSSL_SOURCES CONFIGURE_DEPENDS "${bearssl_source}/src/*.c")
add_library(vantage_bearssl STATIC ${VANTAGE_BEARSSL_SOURCES})
target_include_directories(vantage_bearssl SYSTEM PUBLIC "${bearssl_source}/inc")
target_include_directories(vantage_bearssl PRIVATE "${bearssl_source}/src")
# No OS entropy or clock, and the same portable code paths as the Pi.
target_compile_definitions(vantage_bearssl PRIVATE BR_USE_URANDOM=0 BR_USE_GETENTROPY=0
  BR_USE_WIN32_RAND=0 BR_RDRAND=0 BR_USE_UNIX_TIME=0 BR_USE_WIN32_TIME=0
  BR_AES_X86NI=0 BR_SSE2=0 BR_POWER8=0)
# Unmodified third-party code: its warnings are not ours to fix.
if(MSVC)
  target_compile_options(vantage_bearssl PRIVATE /w)
else()
  target_compile_options(vantage_bearssl PRIVATE -w)
endif()

add_library(vantage_tls STATIC "${VANTAGE_ROOT}/user/lib/tls.c" "${VANTAGE_ROOT}/sim/platform/tls_peer.c")
vantage_host_target(vantage_tls)
target_include_directories(vantage_tls PRIVATE "${VANTAGE_ROOT}")
target_link_libraries(vantage_tls PUBLIC vantage_bearssl)
