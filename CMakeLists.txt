cmake_minimum_required(VERSION 3.16)
project(EuNEx VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ── Options ──────────────────────────────────────────────────────────
option(EUNEX_USE_SIMPLX "Build with real Simplx framework (OFF = embedded shim)" OFF)
option(EUNEX_BUILD_TESTS "Build test binaries" ON)
option(EUNEX_BUILD_EXAMPLES "Build example binaries" ON)

# ── Simplx dependency ───────────────────────────────────────────────
if(EUNEX_USE_SIMPLX)
    # When you have the real Simplx checked out:
    #   cmake -DEUNEX_USE_SIMPLX=ON -DSIMPLX_ROOT=/path/to/simplx ..
    find_path(SIMPLX_INCLUDE_DIR simplx.h
        HINTS ${SIMPLX_ROOT}/include /usr/local/include)
    if(NOT SIMPLX_INCLUDE_DIR)
        message(FATAL_ERROR "Simplx headers not found. Set SIMPLX_ROOT.")
    endif()
    include_directories(${SIMPLX_INCLUDE_DIR})
    add_compile_definitions(EUNEX_REAL_SIMPLX=1)
else()
    # Use our lightweight Simplx-compatible shim for development
    include_directories(${CMAKE_SOURCE_DIR}/src/engine)
endif()

include_directories(${CMAKE_SOURCE_DIR}/src)

# ── Core library ────────────────────────────────────────────────────
add_library(eunex_core STATIC
    src/common/Types.hpp
    src/common/OrderBook.hpp
    src/common/OrderBook.cpp
    src/engine/SimplxShim.hpp
    src/actors/OrderBookActor.hpp
    src/actors/OrderBookActor.cpp
    src/actors/OEGatewayActor.hpp
    src/actors/OEGatewayActor.cpp
    src/actors/MarketDataActor.hpp
    src/actors/MarketDataActor.cpp
    src/recovery/RecoveryProxy.hpp
    src/recovery/RecoveryProxy.cpp
    src/iaca/Fragment.hpp
    src/iaca/IacaAggregator.hpp
    src/iaca/IacaAggregator.cpp
)

target_include_directories(eunex_core PUBLIC ${CMAKE_SOURCE_DIR}/src)

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
endif()
