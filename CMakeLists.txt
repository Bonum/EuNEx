cmake_minimum_required(VERSION 3.16)
project(EuNEx VERSION 0.7.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ── Options ──────────────────────────────────────────────────────────
option(EUNEX_USE_SIMPLX "Build with real Simplx framework (OFF = embedded engine)" OFF)
option(EUNEX_USE_KAFKA  "Build with Kafka persistence (requires librdkafka)" OFF)
option(EUNEX_BUILD_TESTS "Build test binaries" ON)
option(EUNEX_BUILD_EXAMPLES "Build example binaries" ON)

# ── Simplx dependency ───────────────────────────────────────────────
if(EUNEX_USE_SIMPLX)
    find_path(SIMPLX_INCLUDE_DIR simplx.h
        HINTS ${SIMPLX_ROOT}/include /usr/local/include)
    if(NOT SIMPLX_INCLUDE_DIR)
        message(FATAL_ERROR "Simplx headers not found. Set SIMPLX_ROOT.")
    endif()
    include_directories(${SIMPLX_INCLUDE_DIR})
    add_compile_definitions(EUNEX_REAL_SIMPLX=1)
endif()

# ── Kafka dependency ────────────────────────────────────────────────
if(EUNEX_USE_KAFKA)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(RDKAFKA QUIET rdkafka rdkafka++)
    endif()

    if(NOT RDKAFKA_FOUND)
        find_library(RDKAFKA_LIB rdkafka HINTS /usr/lib /usr/local/lib)
        find_library(RDKAFKA_CXX_LIB rdkafka++ HINTS /usr/lib /usr/local/lib)
        find_path(RDKAFKA_INCLUDE_DIR librdkafka/rdkafkacpp.h
            HINTS /usr/include /usr/local/include)
        if(RDKAFKA_LIB AND RDKAFKA_CXX_LIB AND RDKAFKA_INCLUDE_DIR)
            set(RDKAFKA_LIBRARIES ${RDKAFKA_CXX_LIB} ${RDKAFKA_LIB})
            set(RDKAFKA_INCLUDE_DIRS ${RDKAFKA_INCLUDE_DIR})
        else()
            message(FATAL_ERROR "librdkafka not found. Install librdkafka-dev.")
        endif()
    endif()

    add_compile_definitions(EUNEX_USE_KAFKA=1)
    include_directories(${RDKAFKA_INCLUDE_DIRS})
endif()

include_directories(${CMAKE_SOURCE_DIR}/src)

# ── Core library ────────────────────────────────────────────────────
set(EUNEX_CORE_SOURCES
    src/common/Book.cpp
    src/actors/MECoreActor.cpp
    src/actors/OEGActor.cpp
    src/actors/MDGActor.cpp
    src/actors/ClearingHouseActor.cpp
    src/actors/FIXAcceptorActor.cpp
    src/actors/AITraderActor.cpp
    src/recovery/RecoveryProxy.cpp
    src/iaca/IacaAggregator.cpp
)

add_library(eunex_core STATIC ${EUNEX_CORE_SOURCES})
target_include_directories(eunex_core PUBLIC ${CMAKE_SOURCE_DIR}/src)

if(EUNEX_USE_KAFKA)
    target_link_libraries(eunex_core PUBLIC ${RDKAFKA_LIBRARIES})
endif()

# Thread support
find_package(Threads REQUIRED)
target_link_libraries(eunex_core PUBLIC Threads::Threads)

# Windows socket library
if(WIN32)
    target_link_libraries(eunex_core PUBLIC ws2_32)
endif()

# ── Main executable ─────────────────────────────────────────────────
add_executable(eunex_me src/main.cpp)
target_link_libraries(eunex_me PRIVATE eunex_core)

# ── Examples ────────────────────────────────────────────────────────
if(EUNEX_BUILD_EXAMPLES)
    add_executable(ping_pong examples/ping_pong.cpp)
    target_link_libraries(ping_pong PRIVATE eunex_core)

    add_executable(simple_match examples/simple_match.cpp)
    target_link_libraries(simple_match PRIVATE eunex_core)
endif()

# ── Tests ───────────────────────────────────────────────────────────
if(EUNEX_BUILD_TESTS)
    enable_testing()

    add_executable(test_orderbook tests/test_orderbook.cpp)
    target_link_libraries(test_orderbook PRIVATE eunex_core)
    add_test(NAME OrderBookTest COMMAND test_orderbook)

    add_executable(test_matching_engine tests/test_matching_engine.cpp)
    target_link_libraries(test_matching_engine PRIVATE eunex_core)
    add_test(NAME MatchingEngineTest COMMAND test_matching_engine)

    add_executable(test_threaded_engine tests/test_threaded_engine.cpp)
    target_link_libraries(test_threaded_engine PRIVATE eunex_core)
    add_test(NAME ThreadedEngineTest COMMAND test_threaded_engine)

    add_executable(test_clearing_house tests/test_clearing_house.cpp)
    target_link_libraries(test_clearing_house PRIVATE eunex_core)
    add_test(NAME ClearingHouseTest COMMAND test_clearing_house)

    add_executable(test_fix_gateway tests/test_fix_gateway.cpp)
    target_link_libraries(test_fix_gateway PRIVATE eunex_core)
    add_test(NAME FIXGatewayTest COMMAND test_fix_gateway)

    add_executable(test_ai_trader tests/test_ai_trader.cpp)
    target_link_libraries(test_ai_trader PRIVATE eunex_core)
    add_test(NAME AITraderTest COMMAND test_ai_trader)

    add_executable(test_stop_orders tests/test_stop_orders.cpp)
    target_link_libraries(test_stop_orders PRIVATE eunex_core)
    add_test(NAME StopOrdersTest COMMAND test_stop_orders)
endif()
