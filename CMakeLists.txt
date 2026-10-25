cmake_minimum_required(VERSION 3.24)

project(ENRAGE VERSION 0.1.0 LANGUAGES C CXX)

option(ENRAGE_BUILD_TESTS "Build ENRAGE regression tests" ON)
find_package(Threads REQUIRED)

add_library(enrage_sdk INTERFACE)
target_include_directories(enrage_sdk INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)

add_library(enrage_core STATIC
    src/core/archive.cpp
    src/core/build_catalog.cpp
    src/core/build_profile_io.cpp
    src/core/build_profile_v3.cpp
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
    src/core/profile_support.cpp
    src/core/recovery.cpp
    src/core/runtime.cpp
    src/core/status.cpp
    src/platform/dynamic_library.cpp
    src/platform/fingerprint.cpp
    src/platform/pe_image.cpp
    src/platform/scan_image.cpp
    src/platform/signature/pattern.cpp
    src/platform/signature/resolver.cpp
    src/adapters/image_provider.cpp
    src/adapters/lab_adapter.cpp
    src/adapters/null_adapter.cpp
    src/adapters/profile_adapter.cpp
    src/adapters/profile_runtime.cpp
    src/adapters/signature_health.cpp
)

target_include_directories(enrage_core
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(enrage_core PUBLIC enrage_sdk ${CMAKE_DL_LIBS} Threads::Threads)
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

add_executable(enrage_profile src/tools/profile_cli.cpp)
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

    add_library(enrage_state_reject_plugin MODULE tests/state_reject_plugin.cpp)
    target_link_libraries(enrage_state_reject_plugin PRIVATE enrage_sdk)
    set_target_properties(enrage_state_reject_plugin PROPERTIES PREFIX "")

    add_library(enrage_fault_plugin MODULE tests/fault_plugin.cpp)
    target_link_libraries(enrage_fault_plugin PRIVATE enrage_sdk)
    set_target_properties(enrage_fault_plugin PROPERTIES PREFIX "")

    add_library(enrage_capability_probe_plugin MODULE tests/capability_probe_plugin.cpp)
    target_link_libraries(enrage_capability_probe_plugin PRIVATE enrage_sdk)
    set_target_properties(enrage_capability_probe_plugin PROPERTIES PREFIX "")

    add_library(enrage_vfs_probe_plugin MODULE tests/vfs_probe_plugin.cpp)
    target_link_libraries(enrage_vfs_probe_plugin PRIVATE enrage_sdk)
    set_target_properties(enrage_vfs_probe_plugin PROPERTIES PREFIX "")

    add_library(enrage_worker_plugin MODULE tests/worker_plugin.cpp)
    target_link_libraries(enrage_worker_plugin PRIVATE enrage_sdk Threads::Threads)
    set_target_properties(enrage_worker_plugin PROPERTIES PREFIX "")

    add_executable(enrage_crash_probe tests/crash_probe.cpp)
    target_include_directories(enrage_crash_probe PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(enrage_crash_probe PRIVATE enrage_core)

    add_executable(enrage_sdk_c_tests tests/sdk_c_tests.c)
    target_link_libraries(enrage_sdk_c_tests PRIVATE enrage_sdk)

    add_executable(enrage_tests tests/runtime_tests.cpp)
    target_include_directories(enrage_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(enrage_tests PRIVATE enrage_core)
    enrage_keep_test_assertions(enrage_tests)

    add_executable(enrage_archive_integrity_tests tests/archive_integrity_tests.cpp)
    target_include_directories(enrage_archive_integrity_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(enrage_archive_integrity_tests PRIVATE enrage_core Threads::Threads)
    enrage_keep_test_assertions(enrage_archive_integrity_tests)

    add_executable(enrage_vfs_integration_tests tests/vfs_integration_tests.cpp)
    target_include_directories(enrage_vfs_integration_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(enrage_vfs_integration_tests PRIVATE enrage_core)
    enrage_keep_test_assertions(enrage_vfs_integration_tests)

    add_executable(enrage_mod_reload_tests tests/mod_reload_tests.cpp)
    target_include_directories(enrage_mod_reload_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(enrage_mod_reload_tests PRIVATE enrage_core Threads::Threads)
    enrage_keep_test_assertions(enrage_mod_reload_tests)

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
    target_link_libraries(enrage_overlay_tests PRIVATE enrage_core Threads::Threads)
    enrage_keep_test_assertions(enrage_overlay_tests)

    add_executable(enrage_build_profile_tests tests/build_profile_tests.cpp)
    target_include_directories(enrage_build_profile_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(enrage_build_profile_tests PRIVATE enrage_core)
    enrage_keep_test_assertions(enrage_build_profile_tests)

    add_executable(enrage_profile_v3_tests tests/profile_v3_tests.cpp)
    target_include_directories(enrage_profile_v3_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(enrage_profile_v3_tests PRIVATE enrage_core)
    enrage_keep_test_assertions(enrage_profile_v3_tests)

    add_executable(enrage_profile_adapter_tests tests/profile_adapter_tests.cpp)
    target_include_directories(enrage_profile_adapter_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(enrage_profile_adapter_tests PRIVATE enrage_core)
    enrage_keep_test_assertions(enrage_profile_adapter_tests)

    add_executable(enrage_image_provider_tests tests/image_provider_tests.cpp)
    target_include_directories(enrage_image_provider_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(enrage_image_provider_tests PRIVATE enrage_core)
    enrage_keep_test_assertions(enrage_image_provider_tests)

    add_executable(enrage_profile_reload_tests tests/profile_reload_tests.cpp)
    target_include_directories(enrage_profile_reload_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(enrage_profile_reload_tests PRIVATE enrage_core)
    enrage_keep_test_assertions(enrage_profile_reload_tests)

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

    add_executable(enrage_profile_support_tests tests/profile_support_tests.cpp)
    target_include_directories(enrage_profile_support_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(enrage_profile_support_tests PRIVATE enrage_core)
    enrage_keep_test_assertions(enrage_profile_support_tests)

    add_executable(enrage_scan_image_tests tests/scan_image_tests.cpp)
    target_include_directories(enrage_scan_image_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(enrage_scan_image_tests PRIVATE enrage_core)
    enrage_keep_test_assertions(enrage_scan_image_tests)

    add_executable(enrage_lab_adapter_tests tests/lab_adapter_tests.cpp)
    target_include_directories(enrage_lab_adapter_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(enrage_lab_adapter_tests PRIVATE enrage_core)
    enrage_keep_test_assertions(enrage_lab_adapter_tests)

    add_executable(enrage_signature_health_tests tests/signature_health_tests.cpp)
    target_include_directories(enrage_signature_health_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(enrage_signature_health_tests PRIVATE enrage_core)
    enrage_keep_test_assertions(enrage_signature_health_tests)

    add_executable(enrage_plugin_execution_tests tests/plugin_execution_tests.cpp)
    target_include_directories(enrage_plugin_execution_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(enrage_plugin_execution_tests PRIVATE enrage_core Threads::Threads)
    enrage_keep_test_assertions(enrage_plugin_execution_tests)

    add_executable(enrage_worker_lifetime_tests tests/worker_lifetime_tests.cpp)
    target_include_directories(enrage_worker_lifetime_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(enrage_worker_lifetime_tests PRIVATE enrage_core Threads::Threads)
    enrage_keep_test_assertions(enrage_worker_lifetime_tests)

    add_executable(enrage_process_recovery_tests tests/process_recovery_tests.cpp)
    target_include_directories(enrage_process_recovery_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(enrage_process_recovery_tests PRIVATE enrage_core)
    enrage_keep_test_assertions(enrage_process_recovery_tests)

    add_test(NAME enrage_sdk_c_tests COMMAND enrage_sdk_c_tests)
    add_test(NAME enrage_tests COMMAND enrage_tests $<TARGET_FILE:enrage_test_plugin>)
    add_test(NAME enrage_archive_integrity_tests COMMAND enrage_archive_integrity_tests)
    add_test(NAME enrage_vfs_integration_tests
        COMMAND enrage_vfs_integration_tests $<TARGET_FILE:enrage_vfs_probe_plugin>)
    add_test(NAME enrage_mod_reload_tests
        COMMAND enrage_mod_reload_tests $<TARGET_FILE:enrage_test_plugin>)
    add_test(NAME enrage_fingerprint_tests COMMAND enrage_fingerprint_tests)
    add_test(NAME enrage_pe_tests COMMAND enrage_pe_tests)
    add_test(NAME enrage_environment_tests COMMAND enrage_environment_tests)
    add_test(NAME enrage_overlay_tests COMMAND enrage_overlay_tests)
    add_test(NAME enrage_build_profile_tests COMMAND enrage_build_profile_tests)
    add_test(NAME enrage_profile_v3_tests COMMAND enrage_profile_v3_tests)
    add_test(NAME enrage_profile_adapter_tests COMMAND enrage_profile_adapter_tests)
    add_test(NAME enrage_image_provider_tests COMMAND enrage_image_provider_tests)
    add_test(NAME enrage_profile_reload_tests COMMAND enrage_profile_reload_tests)
    add_test(NAME enrage_recovery_tests COMMAND enrage_recovery_tests)
    add_test(NAME enrage_recovery_compat_tests COMMAND enrage_recovery_compat_tests)
    add_test(NAME enrage_inspection_tests COMMAND enrage_inspection_tests)
    add_test(NAME enrage_profile_authoring_tests COMMAND enrage_profile_authoring_tests)
    add_test(NAME enrage_profile_matrix_tests COMMAND enrage_profile_matrix_tests)
    add_test(NAME enrage_profile_propagation_tests COMMAND enrage_profile_propagation_tests)
    add_test(NAME enrage_profile_support_tests COMMAND enrage_profile_support_tests)
    add_test(NAME enrage_scan_image_tests COMMAND enrage_scan_image_tests)
    add_test(NAME enrage_lab_adapter_tests
        COMMAND enrage_lab_adapter_tests $<TARGET_FILE:enrage_capability_probe_plugin>)
    add_test(NAME enrage_signature_health_tests
        COMMAND enrage_signature_health_tests $<TARGET_FILE:enrage_capability_probe_plugin>)
    add_test(NAME enrage_plugin_execution_tests
        COMMAND enrage_plugin_execution_tests
            $<TARGET_FILE:enrage_test_plugin>
            $<TARGET_FILE:enrage_state_reject_plugin>)
    add_test(NAME enrage_worker_lifetime_tests
        COMMAND enrage_worker_lifetime_tests $<TARGET_FILE:enrage_worker_plugin>)
    add_test(NAME enrage_process_recovery_tests
        COMMAND enrage_process_recovery_tests
            $<TARGET_FILE:enrage_crash_probe>
            $<TARGET_FILE:enrage_fault_plugin>)

    set(enrage_regression_tests
        enrage_sdk_c_tests
        enrage_tests
        enrage_archive_integrity_tests
        enrage_vfs_integration_tests
        enrage_mod_reload_tests
        enrage_fingerprint_tests
        enrage_pe_tests
        enrage_environment_tests
        enrage_overlay_tests
        enrage_build_profile_tests
        enrage_profile_v3_tests
        enrage_profile_adapter_tests
        enrage_image_provider_tests
        enrage_profile_reload_tests
        enrage_recovery_tests
        enrage_recovery_compat_tests
        enrage_inspection_tests
        enrage_profile_authoring_tests
        enrage_profile_matrix_tests
        enrage_profile_propagation_tests
        enrage_profile_support_tests
        enrage_scan_image_tests
        enrage_lab_adapter_tests
        enrage_signature_health_tests
        enrage_plugin_execution_tests
        enrage_worker_lifetime_tests
        enrage_process_recovery_tests)
    set_tests_properties(${enrage_regression_tests} PROPERTIES TIMEOUT 30)
endif()
