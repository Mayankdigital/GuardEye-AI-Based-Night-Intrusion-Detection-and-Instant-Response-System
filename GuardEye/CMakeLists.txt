cmake_minimum_required(VERSION 3.16)
project(GuardEye VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(GUARDEYE_ENABLE_GPIOD "Use libgpiod (v1.x API) for real GPIO" ON)
option(GUARDEYE_BUILD_TESTS  "Build unit tests" ON)

find_package(Threads REQUIRED)
find_package(OpenCV REQUIRED COMPONENTS core imgproc imgcodecs videoio dnn)
find_package(SQLite3 REQUIRED)
find_package(CURL)
find_package(PkgConfig QUIET)

set(GUARDEYE_DEFS "")
if(CURL_FOUND)
  list(APPEND GUARDEYE_DEFS HAVE_CURL)
else()
  message(WARNING "libcurl not found: Telegram alerts will be log-only")
endif()

set(GPIOD_LIBS "")
if(GUARDEYE_ENABLE_GPIOD AND PkgConfig_FOUND)
  pkg_check_modules(GPIOD QUIET libgpiod)
  if(GPIOD_FOUND AND GPIOD_VERSION VERSION_LESS "2.0")
    list(APPEND GUARDEYE_DEFS HAVE_GPIOD)
    set(GPIOD_LIBS ${GPIOD_LIBRARIES})
    message(STATUS "libgpiod ${GPIOD_VERSION} found: real GPIO enabled")
  else()
    message(STATUS "libgpiod 1.x not found: GPIO falls back to simulation")
  endif()
endif()

add_library(guardeye_core STATIC
  src/config.cpp
  src/state_machine.cpp
  src/motion_gate.cpp
  src/detector.cpp
  src/capture.cpp
  src/clip_recorder.cpp
  src/alerts.cpp
  src/event_store.cpp
  src/controller.cpp
  src/vision.cpp
  src/http_server.cpp
  src/sysinfo.cpp
  src/hal/gpio.cpp
  src/hal/i2c_bus.cpp
  src/hal/ds3231.cpp
  src/hal/ina219.cpp
  src/hal/ssd1306.cpp
  src/hal/hardware.cpp
)
target_include_directories(guardeye_core PUBLIC src third_party ${OpenCV_INCLUDE_DIRS})
target_compile_definitions(guardeye_core PUBLIC ${GUARDEYE_DEFS} GUARDEYE_VERSION="${PROJECT_VERSION}")
target_compile_options(guardeye_core PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(guardeye_core PUBLIC
  ${OpenCV_LIBS} SQLite::SQLite3 Threads::Threads ${GPIOD_LIBS})
if(CURL_FOUND)
  target_link_libraries(guardeye_core PUBLIC CURL::libcurl)
endif()

add_executable(guardeye src/main.cpp)
target_link_libraries(guardeye PRIVATE guardeye_core)
install(TARGETS guardeye RUNTIME DESTINATION bin)
install(DIRECTORY web DESTINATION share/guardeye)
install(FILES config/guardeye.json DESTINATION /etc/guardeye)

if(GUARDEYE_BUILD_TESTS)
  enable_testing()
  foreach(t test_zone test_state_machine test_schedule test_postprocess test_motion_gate)
    add_executable(${t} tests/${t}.cpp)
    target_link_libraries(${t} PRIVATE guardeye_core)
    add_test(NAME ${t} COMMAND ${t})
  endforeach()
endif()
