cmake_minimum_required(VERSION 3.20)
project(LegoArkham1 CXX)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
  message(FATAL_ERROR "LEGO Batman is a 32-bit game, configure with -A Win32")
endif()

add_library(dinput8 SHARED
  src/main.cpp
  src/hook.cpp
  src/frame.cpp
  src/alt_f4.cpp
  src/borderless.cpp
  src/vsync_default.cpp
  src/device_scan.cpp
  src/player2.cpp
  src/mouse.cpp
  src/mouse_controls.cpp
  src/keybinds.cpp
  src/glide.cpp
  src/grapple.cpp
  src/vehicle.cpp
  src/studs.cpp
  src/clips.cpp
  src/orbit_camera.cpp
  src/pause_save.cpp
  src/dinput8.def)

target_compile_features(dinput8 PRIVATE cxx_std_17)
target_compile_definitions(dinput8 PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(dinput8 PRIVATE xinput9_1_0)
set_target_properties(dinput8 PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_custom_command(TARGET dinput8 POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_SOURCE_DIR}/LegoArkham.ini $<TARGET_FILE_DIR:dinput8>
  COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_SOURCE_DIR}/assets/LegoArkham_glide_batman.an3
          ${CMAKE_SOURCE_DIR}/assets/LegoArkham_glide_batgirl.an3 ${CMAKE_SOURCE_DIR}/assets/LegoArkham_glide_robin.an3
          $<TARGET_FILE_DIR:dinput8>)
