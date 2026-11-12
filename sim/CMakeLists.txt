cmake_minimum_required(VERSION 3.16)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(vantage_sim LANGUAGES C)
  include(CTest)
  if(BUILD_TESTING)
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../tests/native" "${CMAKE_BINARY_DIR}/native")
  endif()
endif()
include("${CMAKE_CURRENT_LIST_DIR}/../cmake/VantageHost.cmake")
include("${VANTAGE_ROOT}/cmake/VantageSDL.cmake")
include("${VANTAGE_ROOT}/cmake/VantageTLS.cmake")

# Share application and kernel services; replace only the platform/transport.
add_library(vantage_sim_core STATIC
  "${VANTAGE_ROOT}/kernel/syscall/dispatch.c"
  "${VANTAGE_ROOT}/kernel/fs/files.c"
  "${VANTAGE_ROOT}/kernel/fs/storage.c"
  "${VANTAGE_ROOT}/kernel/fs/block_store.c"
  "${VANTAGE_ROOT}/kernel/fs/file_syscall.c"
  "${VANTAGE_ROOT}/kernel/net/dhcp.c"
  "${VANTAGE_ROOT}/kernel/net/dns.c"
  "${VANTAGE_ROOT}/kernel/net/ip.c"
  "${VANTAGE_ROOT}/kernel/net/net_syscall.c"
  "${VANTAGE_ROOT}/kernel/net/profile.c"
  "${VANTAGE_ROOT}/kernel/net/service.c"
  "${VANTAGE_ROOT}/kernel/net/sntp.c"
  "${VANTAGE_ROOT}/kernel/net/socket.c"
  "${VANTAGE_ROOT}/kernel/net/tcp.c"
  "${VANTAGE_ROOT}/kernel/ui/canvas.c"
  "${VANTAGE_ROOT}/kernel/ui/window_manager.c"
  "${VANTAGE_ROOT}/user/apps/terminal/terminal.c"
  "${VANTAGE_ROOT}/user/apps/terminal/app.c"
  "${VANTAGE_ROOT}/user/apps/terminal/net_cmd.c"
  "${VANTAGE_ROOT}/user/apps/terminal/net_get.c"
  "${VANTAGE_ROOT}/user/apps/terminal/net_profiles.c"
  "${VANTAGE_ROOT}/user/apps/terminal/net_tools.c"
  "${VANTAGE_ROOT}/user/lib/user_lib.c"
  platform/display_sim.c
  platform/net_peer.c
  platform/net_sim.c
  platform/random_host.c)
vantage_host_target(vantage_sim_core)
target_link_libraries(vantage_sim_core PUBLIC vantage_sdl vantage_tls)
if(WIN32)
  target_link_libraries(vantage_sim_core PUBLIC bcrypt)
endif()

add_executable(vantage_sim main.c platform/storage_host.c)
vantage_host_target(vantage_sim)
target_link_libraries(vantage_sim PRIVATE vantage_sim_core)
vantage_copy_sdl_runtime(vantage_sim)

if(BUILD_TESTING)
  foreach(target vantage_sim_smoke terminal_smoke editor_touch)
    if(target STREQUAL "vantage_sim_smoke")
      set(source smoke)
    else()
      set(source ${target})
    endif()
    add_executable(${target} "tests/${source}.c")
    vantage_test_target(${target})
    target_link_libraries(${target} PRIVATE vantage_sim_core)
    vantage_copy_sdl_runtime(${target})
    if(target STREQUAL "vantage_sim_smoke")
      set(name simulator_smoke)
    else()
      set(name ${target})
    endif()
    add_test(NAME ${name} COMMAND ${target})
    set_tests_properties(${name} PROPERTIES LABELS simulator
      ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_RENDER_DRIVER=software" TIMEOUT 15)
  endforeach()
  set_tests_properties(simulator_smoke PROPERTIES TIMEOUT 10)

  add_executable(terminal_storage_test tests/terminal_storage.c platform/storage_host.c)
  vantage_test_target(terminal_storage_test)
  target_link_libraries(terminal_storage_test PRIVATE vantage_sim_core)
  vantage_copy_sdl_runtime(terminal_storage_test)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  add_test(NAME terminal_storage_reboot COMMAND "${Python3_EXECUTABLE}" -B
    "${VANTAGE_ROOT}/tests/tools/terminal_storage_roundtrip.py" "$<TARGET_FILE:terminal_storage_test>")
  set_tests_properties(terminal_storage_reboot PROPERTIES LABELS "simulator;storage" TIMEOUT 120)
  add_executable(terminal_capture tests/terminal_capture.c platform/storage_host.c)
  vantage_test_target(terminal_capture)
  target_link_libraries(terminal_capture PRIVATE vantage_sim_core)
  vantage_copy_sdl_runtime(terminal_capture)
  add_test(NAME terminal_capture_reboot COMMAND "${Python3_EXECUTABLE}" -B
    "${VANTAGE_ROOT}/tests/tools/terminal_capture_roundtrip.py" "$<TARGET_FILE:terminal_capture>")
  set_tests_properties(terminal_capture_reboot PROPERTIES LABELS "simulator;storage" TIMEOUT 120)

  add_executable(terminal_network tests/terminal_network.c platform/storage_host.c)
  vantage_test_target(terminal_network)
  target_link_libraries(terminal_network PRIVATE vantage_sim_core)
  vantage_copy_sdl_runtime(terminal_network)
  add_test(NAME terminal_network COMMAND "${Python3_EXECUTABLE}" -B
    "${VANTAGE_ROOT}/tests/tools/terminal_network_storage.py" "$<TARGET_FILE:terminal_network>")
  set_tests_properties(terminal_network PROPERTIES LABELS "simulator;network;storage"
    ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_RENDER_DRIVER=software" TIMEOUT 60)

  add_executable(parallel_test tests/parallel.c
    "${VANTAGE_ROOT}/kernel/parallel/worker.c" "${VANTAGE_ROOT}/kernel/ui/raster.c")
  vantage_test_target(parallel_test)
  target_link_libraries(parallel_test PRIVATE vantage_sdl)
  vantage_copy_sdl_runtime(parallel_test)
  add_test(NAME parallel_batches COMMAND parallel_test)
  add_test(NAME parallel_missing_worker COMMAND parallel_test --missing)
  add_test(NAME parallel_completion_timeout COMMAND parallel_test --timeout)
  set_tests_properties(parallel_batches parallel_missing_worker parallel_completion_timeout
    PROPERTIES LABELS simulator TIMEOUT 20)
endif()
