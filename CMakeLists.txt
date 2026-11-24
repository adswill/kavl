# Copyright (c) 2026 Kristijonas Adomaitis (adswill). All rights reserved.
# Use is governed by the KAVL Source Licence (LICENSE).
#
# KAVL: the library (src, include), the command-line tools (tools), the tests (tests) and the app (app, macOS: KAVL.app).
#   cmake -S . -B build -G Ninja && cmake --build build && ctest --test-dir build
# The app needs (Homebrew): glfw dav1d opus hackrf libusb svt-av1; at run time rtl_tcp (librtlsdr) for RTL-SDR dongles. ffmpeg is bundled when
# tools/build_ffmpeg.sh has been run (else a Homebrew ffmpeg is used).
cmake_minimum_required(VERSION 3.20)
project(kavl VERSION 1.0.0 LANGUAGES C CXX)
# ccache, when installed: rebuilds after a clean or a branch switch only compile what really changed
find_program(KAVL_CCACHE ccache)
if(KAVL_CCACHE)
  set(CMAKE_C_COMPILER_LAUNCHER ${KAVL_CCACHE})
  set(CMAKE_CXX_COMPILER_LAUNCHER ${KAVL_CCACHE})
endif()
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
option(KAVL_BUILD_APP "Build the KAVL app (needs glfw, dav1d, opus, hackrf, libusb)" ON)
find_package(Threads REQUIRED)

# ---- library ------------------------------------------------------------------------------------------------------------------------------------------
add_library(kavlcore STATIC src/kavl_common.cpp src/kavl_tables.cpp src/kavl_ldpc.cpp src/kavl_resampler.cpp src/kavl_phy.cpp src/kavl_rx.cpp
  src/kavl_av.cpp src/kavl_media.cpp src/kavl_fec.cpp)
target_include_directories(kavlcore PUBLIC include PRIVATE src)
target_link_libraries(kavlcore PUBLIC Threads::Threads)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(kavlcore PRIVATE -Wall -Wextra)
endif()

# ---- tools ----------------------------------------------------------------------------------------------------------------------------------------------
foreach(t iqwav decwav avtx)
  add_executable(kavl_${t} tools/kavl_${t}.cpp)
  target_link_libraries(kavl_${t} PRIVATE kavlcore)
endforeach()

# ---- tests ----------------------------------------------------------------------------------------------------------------------------------------------
enable_testing()
foreach(t phy av thresholds)
  add_executable(test_kavl_${t} tests/test_kavl_${t}.cpp)
  target_include_directories(test_kavl_${t} PRIVATE tests)
  target_link_libraries(test_kavl_${t} PRIVATE kavlcore)
  add_test(NAME kavl_${t} COMMAND test_kavl_${t})
endforeach()
set_tests_properties(kavl_phy PROPERTIES TIMEOUT 900)

# ---- app ------------------------------------------------------------------------------------------------------------------------------------------------
if(KAVL_BUILD_APP)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(GLFW REQUIRED IMPORTED_TARGET glfw3)
  pkg_check_modules(DAV1D REQUIRED IMPORTED_TARGET dav1d)
  pkg_check_modules(OPUS REQUIRED IMPORTED_TARGET opus)
  pkg_check_modules(HACKRF REQUIRED IMPORTED_TARGET libhackrf)
  pkg_check_modules(LIBUSB REQUIRED IMPORTED_TARGET libusb-1.0)
  set(IMGUI ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui)
  set(FONTS ${CMAKE_CURRENT_SOURCE_DIR}/third_party/fonts/Inter-Regular.ttf ${CMAKE_CURRENT_SOURCE_DIR}/third_party/fonts/Inter-SemiBold.ttf
            ${CMAKE_CURRENT_SOURCE_DIR}/third_party/fonts/JetBrainsMono-Regular.ttf)
  add_executable(kavl MACOSX_BUNDLE app/main.cpp
    app/engine/player.cpp app/engine/rx_engine.cpp app/engine/tx_engine.cpp app/engine/sources.cpp app/engine/settings.cpp app/engine/live_feed.cpp
    app/engine/loopback.cpp app/engine/callsign.cpp app/engine/rig.cpp app/engine/sound_iq.cpp
    app/ui/app.cpp app/ui/widgets.cpp app/ui/picture.cpp app/ui/receive_panel.cpp app/ui/transmit_panel.cpp app/ui/callsign_editor.cpp app/ui/station.cpp
    app/ui/spectrum_view.cpp app/ui/video_gl.cpp app/ui/video_popout.cpp app/ui/rig_window.cpp
    ${IMGUI}/imgui.cpp ${IMGUI}/imgui_draw.cpp ${IMGUI}/imgui_tables.cpp ${IMGUI}/imgui_widgets.cpp
    ${IMGUI}/backends/imgui_impl_glfw.cpp ${IMGUI}/backends/imgui_impl_opengl3.cpp ${FONTS})
  set_source_files_properties(${FONTS} PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
  set(KAVL_VERSION ${PROJECT_VERSION})
  configure_file(app/Info.plist.in ${CMAKE_CURRENT_BINARY_DIR}/Info.plist @ONLY)
  set_target_properties(kavl PROPERTIES OUTPUT_NAME KAVL MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_BINARY_DIR}/Info.plist)
  # self-contained bundle: ffmpeg/ffprobe (tools/build_ffmpeg.sh, when built) and every non-system dylib copied in, signed ad hoc
  option(KAVL_BUNDLE "Copy ffmpeg and the dylibs into KAVL.app after each build" ON)
  if(KAVL_BUNDLE)
    add_custom_target(kavl_bundle ALL COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tools/bundle_app.sh $<TARGET_BUNDLE_DIR:kavl> DEPENDS kavl VERBATIM)
  endif()
  target_include_directories(kavl PRIVATE app app/engine app/ui ${IMGUI} third_party/miniaudio)
  target_compile_definitions(kavl PRIVATE KAVL_FONT_DIR="${CMAKE_CURRENT_SOURCE_DIR}/third_party/fonts" KAVL_VERSION="${PROJECT_VERSION}"
    GL_SILENCE_DEPRECATION)
  target_link_libraries(kavl PRIVATE kavlcore PkgConfig::GLFW PkgConfig::DAV1D PkgConfig::OPUS PkgConfig::HACKRF PkgConfig::LIBUSB
    "-framework OpenGL" "-framework Cocoa" "-framework IOKit" "-framework CoreAudio" "-framework AudioToolbox" "-framework CoreFoundation")
  # rig control against Hamlib's simulated rig (skips when Hamlib is not installed)
  add_executable(test_kavl_rig tests/test_kavl_rig.cpp app/engine/rig.cpp)
  target_include_directories(test_kavl_rig PRIVATE app/engine)
  add_test(NAME kavl_rig COMMAND test_kavl_rig)
  add_executable(test_kavl_soundiq tests/test_kavl_soundiq.cpp app/engine/sound_iq.cpp app/engine/player.cpp)
  target_include_directories(test_kavl_soundiq PRIVATE app/engine third_party/miniaudio)
  target_link_libraries(test_kavl_soundiq PRIVATE kavlcore PkgConfig::DAV1D PkgConfig::OPUS "-framework CoreAudio" "-framework AudioToolbox"
    "-framework CoreFoundation")
  add_test(NAME kavl_soundiq COMMAND test_kavl_soundiq)
endif()
