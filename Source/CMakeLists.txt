cmake_minimum_required(VERSION 3.16)
project(TwoStorms VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(TS_BUILD_TESTS "Build the test programs (engine runner, test host)" ON)
option(TS_ZIG "Building with Zig (cmake/zig-linux.cmake): libc++ is linked in by Zig" OFF)

set(TS_CORE
  src/core/act.cpp
  src/core/act_view.cpp
  src/core/params.cpp
  src/core/plugin.cpp
  src/core/presets.cpp
  src/core/reaper_bridge.cpp
  src/core/shm.cpp
  src/core/eelrand.cpp
  src/core/filters.cpp
  src/core/fft.cpp
  src/core/pvshift.cpp
  src/core/pitch.cpp
)
set(TS_GUI
  src/gui/canvas.cpp
  src/gui/common.cpp
  src/gui/editor.cpp
  src/gui/effect_view.cpp
  src/gui/analyzer.cpp
  src/gui/modslots.cpp
  src/gui/font.cpp
  src/gui/fonts_data.cpp
  src/gui/platform_win32.cpp
  src/gui/platform_x11.cpp
  src/gui/ui.cpp
)
# one folder per plug-in; add the next ones here and to src/entry.cpp
set(TS_PLUGINS
  src/plugins/compressor/comp_engine.cpp
  src/plugins/compressor/comp_params.cpp
  src/plugins/compressor/comp_plugin.cpp
  src/plugins/act/act_shaper.cpp
  src/plugins/act/act_plugin.cpp
  src/plugins/gainstage/gs_plugin.cpp
  src/plugins/gate/gate_plugin.cpp
  src/plugins/tremolo/tremolo_plugin.cpp
  src/plugins/chorus/chorus_plugin.cpp
  src/plugins/delay/delay_engine.cpp
  src/plugins/delay/delay_plugin.cpp
  src/plugins/wah/wah_plugin.cpp
  src/plugins/defitcher/defitcher_plugin.cpp
  src/plugins/saturator/sat_engine.cpp
  src/plugins/saturator/sat_plugin.cpp
  src/plugins/eq/eq_engine.cpp
  src/plugins/eq/eq_plugin.cpp
  src/plugins/multiband/mb_engine.cpp
  src/plugins/multiband/mb_plugin.cpp
  src/plugins/irloader/ir_engine.cpp
  src/plugins/irloader/ir_plugin.cpp
  src/plugins/tuner/tuner_plugin.cpp
)

# everything but the entry point, compiled once: each plug-in's .clap links only what its own entry needs
add_library(ts_all STATIC ${TS_CORE} ${TS_GUI} ${TS_PLUGINS})
target_include_directories(ts_all PUBLIC external/clap-include external/stb src)
target_compile_definitions(ts_all PUBLIC TS_VERSION="${PROJECT_VERSION}")
set_target_properties(ts_all PROPERTIES CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
if(NOT WIN32 AND NOT APPLE)
  # X11 headers are in external/ (types only: libX11 and libXext are loaded at run time)
  target_include_directories(ts_all PRIVATE external/x11-include)
endif()

# the engines must compute exactly what the JSFX computes: no fused multiply-adds, no reordering
function(ts_flags t)
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(${t} PRIVATE -O2 -ffp-contract=off -Wall -Wno-unused-parameter)
  elseif(MSVC)
    target_compile_options(${t} PRIVATE /O2 /fp:precise /W3)
  endif()
endfunction()
ts_flags(ts_all)

# one .clap: its file name, and the plug-in it holds (empty: all of them, the bundle the tests load)
function(ts_bundle target outname kind)
  add_library(${target} MODULE src/entry.cpp)
  target_link_libraries(${target} PRIVATE ts_all)
  if(kind)
    target_compile_definitions(${target} PRIVATE TS_ONLY_KIND=${kind})
  endif()
  set_target_properties(${target} PROPERTIES PREFIX "" SUFFIX ".clap" OUTPUT_NAME "${outname}"
                        CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
  ts_flags(${target})
  # release binaries without symbols
  if(CMAKE_BUILD_TYPE STREQUAL "Release" AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_link_options(${target} PRIVATE -s)
  endif()
  if(WIN32)
    target_link_libraries(${target} PRIVATE user32 gdi32 shell32)
    if(MINGW)
      # no runtime DLLs to ship next to the .clap
      target_link_options(${target} PRIVATE -static -static-libgcc -static-libstdc++ -Wl,--exclude-all-symbols)
    endif()
  elseif(APPLE)
    message(WARNING "macOS has no window code yet")
  else()
    target_link_libraries(${target} PRIVATE pthread rt dl)
    # only clap_entry is exported
    target_link_options(${target} PRIVATE -Wl,--version-script=${CMAKE_SOURCE_DIR}/cmake/exports.map -Wl,--no-undefined)
    if(NOT TS_ZIG)
      # libstdc++ statically, so the .clap doesn't need a newer one than the system has
      target_link_options(${target} PRIVATE -static-libstdc++ -static-libgcc)
    endif()
  endif()
endfunction()

ts_bundle(TwoStorms "TwoStorms" "")
ts_bundle(ts_act "TwoStorms Act" ts::actp::kind)
ts_bundle(ts_gainstage "TwoStorms Gain Stage" ts::gs::kind)
ts_bundle(ts_compressor "TwoStorms Compressor" ts::comp::kind)
ts_bundle(ts_eq "TwoStorms EQ" ts::peq::kind)
ts_bundle(ts_multiband "TwoStorms Multiband" ts::mb::kind)
ts_bundle(ts_irloader "TwoStorms IR Loader" ts::irl::kind)
ts_bundle(ts_saturator "TwoStorms Saturator" ts::sat::kind)
ts_bundle(ts_delay "TwoStorms Delay" ts::delay::kind)
ts_bundle(ts_chorus "TwoStorms Chorus" ts::chorus::kind)
ts_bundle(ts_tremolo "TwoStorms Tremolo" ts::trem::kind)
ts_bundle(ts_wah "TwoStorms Wah" ts::wah::kind)
ts_bundle(ts_defitcher "TwoStorms Defitcher" ts::defitcher::kind)
ts_bundle(ts_gate "TwoStorms Gate" ts::gate::kind)
ts_bundle(ts_tuner "TwoStorms Tuner" ts::tuner::kind)

if(TS_BUILD_TESTS AND NOT WIN32)
  add_executable(engine_run tests/engine_run.cpp src/core/act.cpp src/core/shm.cpp src/core/params.cpp src/core/presets.cpp
    src/plugins/compressor/comp_engine.cpp src/plugins/compressor/comp_params.cpp)
  target_include_directories(engine_run PRIVATE src)
  target_compile_options(engine_run PRIVATE -O2 -ffp-contract=off)
  target_link_libraries(engine_run PRIVATE pthread rt)

  find_package(X11 REQUIRED)
  add_executable(tuner_test tests/tuner_test.cpp src/core/pitch.cpp src/core/fft.cpp)
  target_include_directories(tuner_test PRIVATE src)
  add_executable(clap_host_test tests/clap_host_test.cpp src/core/shm.cpp src/core/reaper_bridge.cpp)
  target_include_directories(clap_host_test PRIVATE external/clap-include external/stb src)
  target_link_libraries(clap_host_test PRIVATE dl pthread ${X11_X11_LIB})
endif()
