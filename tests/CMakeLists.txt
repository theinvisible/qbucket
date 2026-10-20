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

qbucket_add_test(test_account_sidebar
    test_account_sidebar.cpp
    qbucket_app_ui
    qbucket::s3_testsupport
)

qbucket_add_test(test_account_dialog
    test_account_dialog.cpp
    qbucket_app_ui
    qbucket::s3_testsupport
)
