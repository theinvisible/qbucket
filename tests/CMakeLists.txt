# --- Helper for registering QtTest binaries --------------------------------
# One executable + one ctest registration per test. All tests run on the
# offscreen platform so they work headless (CI, Flatpak builder, SSH).
function(qbucket_add_test name source)
    qt_add_executable(${name} ${source})

    target_link_libraries(${name} PRIVATE Qt6::Test qbucket::compile_options ${ARGN})
    target_compile_definitions(${name} PRIVATE
        TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data"
    )

    # Keep test binaries out of the app's output directory. On Windows,
    # windeployqt copies a trimmed Qt (DLLs + only the `windows` platform
    # plugin) next to qbucket.exe; a test placed there loads that copy and
    # cannot find the `offscreen` plugin — Qt then aborts with a modal
    # "no Qt platform plugin" debug dialog. In their own directory the tests
    # use the full Qt install via PATH.
    set_target_properties(${name} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")

    add_test(NAME ${name} COMMAND ${name})

    set_tests_properties(${name} PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    if(WIN32)
        # Let the loader find Qt's DLLs without windeployqt. Prepend via
        # ENVIRONMENT_MODIFICATION: a plain "PATH=a;b" in ENVIRONMENT would be
        # split at the semicolons as a CMake list.
        get_target_property(_qt_core_loc Qt6::Core LOCATION)
        get_filename_component(_qt_bin_dir "${_qt_core_loc}" DIRECTORY)
        set_tests_properties(${name} PROPERTIES
            ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${_qt_bin_dir}")
    endif()
endfunction()

# --- Test binaries ---------------------------------------------------------
qbucket_add_test(test_ui_foundation
    test_ui_foundation.cpp
    qbucket_ui_foundation
)

qbucket_add_test(test_settings_controller
    test_settings_controller.cpp
    qbucket_controllers
)

qbucket_add_test(test_stall_watchdog
    test_stall_watchdog.cpp
    qbucket_diagnostics
)

qbucket_add_test(test_main_window
    test_main_window.cpp
    qbucket_app_ui
)

qbucket_add_test(test_account_store
    test_account_store.cpp
    qbucket_accounts
)

qbucket_add_test(test_secret_store
    test_secret_store.cpp
    qbucket_accounts
)

qbucket_add_test(test_keychain_credentials_provider
    test_keychain_credentials_provider.cpp
    qbucket_accounts
)

qbucket_add_test(test_connection_tester
    test_connection_tester.cpp
    qbucket_controllers
    qbucket::s3_testsupport
)

qbucket_add_test(test_capability_store
    test_capability_store.cpp
    qbucket_accounts
)

qbucket_add_test(test_account_sessions
    test_account_sessions.cpp
    qbucket_controllers
    qbucket::s3_testsupport
)

qbucket_add_test(test_account_tree_model
    test_account_tree_model.cpp
    qbucket_models
    qbucket::s3_testsupport
)

qbucket_add_test(test_capability_controller
    test_capability_controller.cpp
    qbucket_controllers
    qbucket::s3_testsupport
)

qbucket_add_test(test_browse_controller
    test_browse_controller.cpp
    qbucket_controllers
)

qbucket_add_test(test_object_table_model
    test_object_table_model.cpp
    qbucket_models
    qbucket::s3_testsupport
)

qbucket_add_test(test_account_sidebar
    test_account_sidebar.cpp
    qbucket_app_ui
    qbucket::s3_testsupport
)

qbucket_add_test(test_object_browser
    test_object_browser.cpp
    qbucket_app_ui
    qbucket::s3_testsupport
)

qbucket_add_test(test_detail_pane
    test_detail_pane.cpp
    qbucket_app_ui
    qbucket::s3_testsupport
)

qbucket_add_test(test_capabilities_dialog
    test_capabilities_dialog.cpp
    qbucket_app_ui
    qbucket::s3_testsupport
)

qbucket_add_test(test_operation_controller
    test_operation_controller.cpp
    qbucket_app_ui
    qbucket::s3_testsupport
)

qbucket_add_test(test_local_path_mapper
    test_local_path_mapper.cpp
    qbucket_controllers
)

qbucket_add_test(test_local_scanner
    test_local_scanner.cpp
    qbucket_controllers
)

qbucket_add_test(test_transfer_job_store
    test_transfer_job_store.cpp
    qbucket_controllers
)

qbucket_add_test(test_transfer_service
    test_transfer_service.cpp
    qbucket_controllers
)

qbucket_add_test(test_download_job
    test_download_job.cpp
    qbucket_controllers
    qbucket::s3_testsupport
)

qbucket_add_test(test_incomplete_uploads
    test_incomplete_uploads.cpp
    qbucket_app_ui
    qbucket::s3_testsupport
)

qbucket_add_test(test_transfer_panel
    test_transfer_panel.cpp
    qbucket_app_ui
    qbucket::s3_testsupport
)

qbucket_add_test(test_upload_job
    test_upload_job.cpp
    qbucket_app_ui
    qbucket::s3_testsupport
)

qbucket_add_test(test_create_folder_job
    test_create_folder_job.cpp
    qbucket_controllers
    qbucket::s3_testsupport
)

qbucket_add_test(test_delete_job
    test_delete_job.cpp
    qbucket_controllers
    qbucket::s3_testsupport
)

qbucket_add_test(test_main_window_operations
    test_main_window_operations.cpp
    qbucket_app_ui
    qbucket::s3_testsupport
)

qbucket_add_test(test_rename_job
    test_rename_job.cpp
    qbucket_controllers
    qbucket::s3_testsupport
)

qbucket_add_test(test_account_dialog
    test_account_dialog.cpp
    qbucket_app_ui
    qbucket::s3_testsupport
)

# --- Integration: the app's jobs against a real S3 server --------------------
# Opt-in like the library's it_s3_backend (QBUCKET_IT_* in the environment;
# skipped when unset). With QBUCKET_S3_IT_LOCAL one test per Docker backend.
qbucket_add_test(it_operations
    it_operations.cpp
    qbucket_controllers
    qbucket::s3_testsupport
)
set_tests_properties(it_operations PROPERTIES LABELS integration TIMEOUT 600)
# The real window, read-only, against the same server.
qbucket_add_test(it_browser
    it_browser.cpp
    qbucket_app_ui
)
set_tests_properties(it_browser PROPERTIES LABELS integration TIMEOUT 300)
target_compile_definitions(it_browser PRIVATE QBUCKET_QM_DIR="${CMAKE_BINARY_DIR}/src")
# Screenshots with the English plural forms ("1 file", not "1 file(s)").
foreach(_shot test_transfer_panel test_upload_job)
    target_compile_definitions(${_shot} PRIVATE QBUCKET_QM_DIR="${CMAKE_BINARY_DIR}/src")
endforeach()
add_dependencies(it_browser qbucket)
if(QBUCKET_S3_IT_LOCAL)
    foreach(backend ceph versitygw garage)
        file(STRINGS ${CMAKE_SOURCE_DIR}/libs/qbucket-s3/tests/docker/env/${backend}.env _it_env REGEX "^QBUCKET_IT_")
        add_test(NAME it_operations_${backend} COMMAND it_operations)
        set_tests_properties(it_operations_${backend} PROPERTIES
            ENVIRONMENT "${_it_env}"
            LABELS "integration;docker"
            TIMEOUT 600
        )
    endforeach()
endif()

# The compiled .qm files of the app target.
qbucket_add_test(test_translations test_translations.cpp)
target_compile_definitions(test_translations PRIVATE QBUCKET_QM_DIR="${CMAKE_BINARY_DIR}/src")
add_dependencies(test_translations qbucket)

# --- Static checks -----------------------------------------------------------
# The mutation gate (PLAN.md §12.1): writing S3 calls only in src/controllers/.
if(UNIX)
    add_test(NAME mutation_gate
             COMMAND sh ${CMAKE_SOURCE_DIR}/tools/check-mutation-gate.sh
             WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
endif()
