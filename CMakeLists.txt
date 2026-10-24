cmake_minimum_required(VERSION 3.24)

project(ENRAGE VERSION 0.1.0 LANGUAGES C CXX)

option(ENRAGE_BUILD_TESTS "Build ENRAGE regression tests" ON)

add_library(enrage_sdk INTERFACE)
target_include_directories(enrage_sdk INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)

add_library(enrage_core STATIC
    src/core/archive.cpp
    src/core/build_catalog.cpp
    src/core/build_profile_io.cpp
    src/core/inspection.cpp
    src/core/manifest.cpp
    src/core/mod_graph.cpp
    src/core/mod_registry.cpp
    src/core/overlay.cpp
    src/core/package.cpp
    src/core/plugin_host.cpp
    src/core/profile_authoring.cpp
    src/core/profile_matrix.cpp
    src/core/profile_propagation.cpp
    src/core/recovery.cpp
    src/core/runtime.cpp
    src/platform/dynamic_library.cpp
    src/platform/fingerprint.cpp
    src/platform/pe_image.cpp
    src/platform/scan_image.cpp
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

add_executable(enrage_inspect src/tools/inspect_main.cpp)
target_include_directories(enrage_inspect PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(enrage_inspect PRIVATE enrage_core)

add_executable(enrage_profile src/tools/profile_main.cpp)
target_include_directories(enrage_profile PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(enrage_profile PRIVATE enrage_core)

if(ENRAGE_BUILD_TESTS)
    enable_testing()

    function(enrage_keep_test_assertions target)
        if(MSVC)
            target_compile_options(${target} PRIVATE /UNDEBUG)
        else()
            target_compile_options(${target} PRIVATE -UNDEBUG)
        endif()
    endfunction()

    add_library(enrage_test_plugin MODULE tests/test_plugin.cpp)
    target_link_libraries(enrage_test_plugin PRIVATE enrage_sdk)
    set_target_properties(enrage_test_plugin PROPERTIES PREFIX "")

    add_library(enrage_fault_plugin MODULE tests/fault_plugin.cpp)
    target_link_libraries(enrage_fault_plugin PRIVATE enrage_sdk)
    set_target_properties(enrage_fault_plugin PROPERTIES PREFIX "")

    add_executable(enrage_crash_probe tests/crash_probe.cpp)
    target_include_directories(enrage_crash_probe PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(enrage_crash_probe PRIVATE enrage_core)

    add_executable(enrage_tests tests/runtime_tests.cpp)
    target_include_directories(enrage_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(enrage_tests PRIVATE enrage_core)
    enrage_keep_test_assertions(enrage_tests)

    add_executable(enrage_fingerprint_tests tests/fingerprint_tests.cpp)
    target_include_directories(enrage_fingerprint_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(enrage_fingerprint_tests PRIVATE enrage_core)
    enrage_keep_test_assertions(enrage_fingerprint_tests)

    add_executable(enrage_pe_tests tests/pe_tests.cpp)
    target_include_directories(enrage_pe_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(enrage_pe_tests PRIVATE enrage_core)
    enrage_keep_test_assertions(enrage_pe_tests)

    add_executable(enrage_environment_tests tests/environment_tests.cpp)
    target_include_directories(enrage_environment_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(enrage_environment_tests PRIVATE enrage_core)
    enrage_keep_test_assertions(enrage_environment_tests)

    add_executable(enrage_overlay_tests tests/overlay_tests.cpp)
    target_include_directories(enrage_overlay_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(enrage_overlay_tests PRIVATE enrage_core)
    enrage_keep_test_assertions(enrage_overlay_tests)

    add_executable(enrage_build_profile_tests tests/build_profile_tests.cpp)
    target_include_directories(enrage_build_profile_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(enrage_build_profile_tests PRIVATE enrage_core)
    enrage_keep_test_assertions(enrage_build_profile_tests)

    add_executable(enrage_recovery_tests tests/recovery_tests.cpp)
    target_include_directories(enrage_recovery_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(enrage_recovery_tests PRIVATE enrage_core)
    enrage_keep_test_assertions(enrage_recovery_tests)

    add_executable(enrage_recovery_compat_tests tests/recovery_compat_tests.cpp)
    target_include_directories(enrage_recovery_compat_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(enrage_recovery_compat_tests PRIVATE enrage_core)
    enrage_keep_test_assertions(enrage_recovery_compat_tests)

    add_executable(enrage_inspection_tests tests/inspection_tests.cpp)
    target_include_directories(enrage_inspection_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(enrage_inspection_tests PRIVATE enrage_core)
    enrage_keep_test_assertions(enrage_inspection_tests)

    add_executable(enrage_profile_authoring_tests tests/profile_authoring_tests.cpp)
    target_include_directories(enrage_profile_authoring_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(enrage_profile_authoring_tests PRIVATE enrage_core)
    enrage_keep_test_assertions(enrage_profile_authoring_tests)

    add_executable(enrage_profile_matrix_tests tests/profile_matrix_tests.cpp)
    target_include_directories(enrage_profile_matrix_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(enrage_profile_matrix_tests PRIVATE enrage_core)
    enrage_keep_test_assertions(enrage_profile_matrix_tests)

    add_executable(enrage_profile_propagation_tests tests/profile_propagation_tests.cpp)
    target_include_directories(enrage_profile_propagation_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(enrage_profile_propagation_tests PRIVATE enrage_core)
    enrage_keep_test_assertions(enrage_profile_propagation_tests)

    add_executable(enrage_scan_image_tests tests/scan_image_tests.cpp)
    target_include_directories(enrage_scan_image_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(enrage_scan_image_tests PRIVATE enrage_core)
    enrage_keep_test_assertions(enrage_scan_image_tests)

    add_executable(enrage_plugin_execution_tests tests/plugin_execution_tests.cpp)
    target_include_directories(enrage_plugin_execution_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(enrage_plugin_execution_tests PRIVATE enrage_core)
    enrage_keep_test_assertions(enrage_plugin_execution_tests)

    add_executable(enrage_process_recovery_tests tests/process_recovery_tests.cpp)
    target_include_directories(enrage_process_recovery_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(enrage_process_recovery_tests PRIVATE enrage_core)
    enrage_keep_test_assertions(enrage_process_recovery_tests)

    add_test(NAME enrage_tests COMMAND enrage_tests $<TARGET_FILE:enrage_test_plugin>)
    add_test(NAME enrage_fingerprint_tests COMMAND enrage_fingerprint_tests)
    add_test(NAME enrage_pe_tests COMMAND enrage_pe_tests)
    add_test(NAME enrage_environment_tests COMMAND enrage_environment_tests)
    add_test(NAME enrage_overlay_tests COMMAND enrage_overlay_tests)
    add_test(NAME enrage_build_profile_tests COMMAND enrage_build_profile_tests)
    add_test(NAME enrage_recovery_tests COMMAND enrage_recovery_tests)
    add_test(NAME enrage_recovery_compat_tests COMMAND enrage_recovery_compat_tests)
    add_test(NAME enrage_inspection_tests COMMAND enrage_inspection_tests)
    add_test(NAME enrage_profile_authoring_tests COMMAND enrage_profile_authoring_tests)
    add_test(NAME enrage_profile_matrix_tests COMMAND enrage_profile_matrix_tests)
    add_test(NAME enrage_profile_propagation_tests COMMAND enrage_profile_propagation_tests)
    add_test(NAME enrage_scan_image_tests COMMAND enrage_scan_image_tests)
    add_test(NAME enrage_plugin_execution_tests
        COMMAND enrage_plugin_execution_tests $<TARGET_FILE:enrage_test_plugin>)
    add_test(NAME enrage_process_recovery_tests
        COMMAND enrage_process_recovery_tests
            $<TARGET_FILE:enrage_crash_probe>
            $<TARGET_FILE:enrage_fault_plugin>)
endif()
