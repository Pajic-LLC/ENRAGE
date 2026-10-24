cmake_minimum_required(VERSION 3.24)

project(ENRAGE VERSION 0.1.0 LANGUAGES CXX)

option(ENRAGE_BUILD_TESTS "Build ENRAGE regression tests" ON)

add_library(enrage_sdk INTERFACE)
target_include_directories(enrage_sdk INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(enrage_sdk INTERFACE cxx_std_23)

add_library(enrage_core STATIC
    src/core/mod_registry.cpp
    src/core/runtime.cpp
    src/platform/signature/pattern.cpp
    src/adapters/null_adapter.cpp
)

target_include_directories(enrage_core
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(enrage_core PUBLIC enrage_sdk)
target_compile_features(enrage_core PUBLIC cxx_std_23)

if(MSVC)
    target_compile_options(enrage_core PRIVATE /W4 /permissive-)
else()
    target_compile_options(enrage_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(enrage_host src/host/main.cpp)
target_include_directories(enrage_host PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(enrage_host PRIVATE enrage_core)

if(ENRAGE_BUILD_TESTS)
    enable_testing()
    add_executable(enrage_tests tests/runtime_tests.cpp)
    target_include_directories(enrage_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(enrage_tests PRIVATE enrage_core)
    add_test(NAME enrage_tests COMMAND enrage_tests)
endif()
