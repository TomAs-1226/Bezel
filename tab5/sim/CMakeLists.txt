# Catalyst Tab — host simulator.
#
# Builds the same UI, renderer and NetworkTables client as the firmware against LVGL on Linux, with a
# headless HAL: frames land in memory, `shot` writes them as PNG, touches come from a script. Point it
# at tools/fake_robot.py (or a real robot) to exercise the whole stack without a Tab5.
#
#   cmake -S sim -B build/sim -DLVGL_DIR=/path/to/lvgl && cmake --build build/sim -j
#   ./build/sim/catalyst_tab_sim --script sim/tour.txt --out shots/
cmake_minimum_required(VERSION 3.16)
project(catalyst_tab_sim C)
set(CMAKE_C_STANDARD 11)
set(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(LVGL_DIR "/opt/lvgl" CACHE PATH "LVGL 9.3 source tree")
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

add_compile_definitions(LV_CONF_PATH="${ROOT}/lv_conf.h" _GNU_SOURCE)
set(LV_CONF_PATH "${ROOT}/lv_conf.h")
file(GLOB_RECURSE LVGL_SOURCES ${LVGL_DIR}/src/*.c)
add_library(lvgl STATIC ${LVGL_SOURCES})
target_include_directories(lvgl PUBLIC ${LVGL_DIR} ${LVGL_DIR}/src ${ROOT})
target_compile_options(lvgl PRIVATE -w)

file(GLOB APP_SOURCES
  ${ROOT}/components/nt4/src/*.c
  ${ROOT}/components/bezel/src/*.c
  ${ROOT}/components/bezel/fonts/*.c
  ${ROOT}/components/catalyst/src/*.c
  ${ROOT}/components/assist/src/*.c
  ${ROOT}/components/home/src/*.c
  ${ROOT}/components/ui/src/*.c)
add_executable(catalyst_tab_sim ${APP_SOURCES} sim_main.c hal_sim.c hal_sim_net.c png.c)
target_include_directories(catalyst_tab_sim PRIVATE
  ${ROOT}/components/nt4/include ${ROOT}/components/bezel/include
  ${ROOT}/components/catalyst/include ${ROOT}/components/assist/include ${ROOT}/components/ui/include
  ${ROOT}/components/tab_hal/include ${ROOT}/components/home/include
  ${LVGL_DIR})
target_compile_options(catalyst_tab_sim PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Wno-format-truncation)
find_package(ZLIB REQUIRED)
find_package(OpenSSL REQUIRED)
target_link_libraries(catalyst_tab_sim lvgl ZLIB::ZLIB OpenSSL::SSL OpenSSL::Crypto m pthread)

# The assistant end to end without the UI: this HAL, a robot over NT4, the Messages API (tools/fake_claude.py).
file(GLOB E2E_SOURCES
  ${ROOT}/components/nt4/src/*.c
  ${ROOT}/components/catalyst/src/*.c
  ${ROOT}/components/assist/src/*.c)
add_executable(assist_e2e ${E2E_SOURCES} assist_e2e.c hal_sim.c hal_sim_net.c png.c)
target_include_directories(assist_e2e PRIVATE
  ${ROOT}/components/nt4/include ${ROOT}/components/bezel/include ${ROOT}/components/catalyst/include
  ${ROOT}/components/assist/include ${ROOT}/components/tab_hal/include)
target_compile_options(assist_e2e PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Wno-format-truncation)
target_link_libraries(assist_e2e ZLIB::ZLIB OpenSSL::SSL OpenSSL::Crypto m pthread)

# Unit tests for the portable pieces: springs, msgpack, json, the Catalyst parsers.
file(GLOB TEST_SOURCES ${ROOT}/test/*.c)
if(TEST_SOURCES)
  add_executable(catalyst_tab_tests ${TEST_SOURCES}
    ${ROOT}/components/nt4/src/mpack_lite.c ${ROOT}/components/nt4/src/json_lite.c
    ${ROOT}/components/bezel/src/bz_motion.c
    ${ROOT}/components/catalyst/src/cat_can.c ${ROOT}/components/catalyst/src/cat_logs.c ${ROOT}/components/catalyst/src/cat_batt.c
    ${ROOT}/components/assist/src/as_json.c ${ROOT}/components/assist/src/as_sse.c
    ${ROOT}/components/assist/src/as_conv.c ${ROOT}/components/assist/src/as_tooldefs.c
    ${ROOT}/components/assist/src/as_oai.c ${ROOT}/components/assist/src/as_desk.c
    ${ROOT}/components/catalyst/src/cat_sc.c)
  target_include_directories(catalyst_tab_tests PRIVATE ${ROOT}/components/nt4/include
    ${ROOT}/components/bezel/include ${ROOT}/components/catalyst/include
    ${ROOT}/components/assist/include ${ROOT}/components/assist/src)
  target_link_libraries(catalyst_tab_tests m pthread)
endif()
