# ArduinoJson is header-only; pinned to the firmware's version (platformio.ini).
FetchContent_Declare(
  arduinojson
  GIT_REPOSITORY https://github.com/bblanchon/ArduinoJson.git
  GIT_TAG v7.4.2
)
FetchContent_GetProperties(arduinojson)
if(NOT arduinojson_POPULATED)
  FetchContent_Populate(arduinojson)
endif()

add_executable(TickerLogicTest
  TickerLogicTest.cpp
  ${REPO_ROOT}/src/ticker/TickerLogic.cpp
)

target_include_directories(TickerLogicTest PRIVATE
  ${REPO_ROOT}/src/ticker
  ${arduinojson_SOURCE_DIR}/src
)

target_compile_definitions(TickerLogicTest PRIVATE
  TICKER_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
)

target_link_libraries(TickerLogicTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(TickerLogicTest)
