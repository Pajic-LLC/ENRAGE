cmake_minimum_required(VERSION 3.24)

project(ENRAGE VERSION 0.1.0 LANGUAGES C CXX)

option(ENRAGE_BUILD_TESTS "Build ENRAGE regression tests" ON)

add_library(enrage_sdk INTERFACE)
target_include_directories(enrage_sdk INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)

add_library(enrage_core STATIC
    src/core/archive.cpp
    src/core/build_catalog.cpp
    src/core/manifest.cpp
    src/core/mod_graph.cpp
    src/core/mod_registry.cpp
    src/core/overlay.cpp
    src/core/package.cpp
    src/core/plugin_host.cpp
    src/core/runtime.cpp
    src/platform/dynamic_library.cpp
    src/platform/fingerprint.cpp
    src/platform/pe_image.cpp
    src/platform/signature/pattern.cpp
    src/platform/signature/resolver.cpp
    src/adapters/null_adapter.cpp
)

target_include_directories(enrage_core
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(enrage_core PUBLIC enrage_sdk ${CMAKE_DL_LIBS})
target_compile_features(enrage_core PUBLIC cxx_std_23)

if(MSVC)
    target_compile_options(enrage_core PRIVATE /W4 /permissive-)
else()
    target_compile_options(enrage_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(enrage_host src/host/main.cpp)
target_include_directories(enrage_host PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(enrage_host PRIVATE enrage_core)

add_executable(enrage_pack src/tools/pack_main.cpp)
target_include_directories(enrage_pack PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(enrage_pack PRIVATE enrage_core)

if(ENRAGE_BUILD_TESTS)
    enable_testing()

    add_library(enrage_test_plugin MODULE tests/test_plugin.cpp)
    target_link_libraries(enrage_test_plugin PRIVATE enrage_sdk)
    set_target_properties(enrage_test_plugin PROPERTIES PREFIX "")

    add_executable(enrage_tests tests/runtime_tests.cpp)
    target_include_directories(enrage_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(enrage_tests PRIVATE enrage_core)

    add_executable(enrage_fingerprint_tests tests/fingerprint_tests.cpp)
    target_include_directories(enrage_fingerprint_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(enrage_fingerprint_tests PRIVATE enrage_core)

    add_executable(enrage_pe_tests tests/pe_tests.cpp)
    target_include_directories(enrage_pe_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(enrage_pe_tests PRIVATE enrage_core)

    add_executable(enrage_environment_tests tests/environment_tests.cpp)
    target_include_directories(enrage_environment_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(enrage_environment_tests PRIVATE enrage_core)

    add_test(NAME enrage_tests COMMAND enrage_tests $<TARGET_FILE:enrage_test_plugin>)
    add_test(NAME enrage_fingerprint_tests COMMAND enrage_fingerprint_tests)
    add_test(NAME enrage_pe_tests COMMAND enrage_pe_tests)
    add_test(NAME enrage_environment_tests COMMAND enrage_environment_tests)
endif()
