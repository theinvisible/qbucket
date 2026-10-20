# --- Diagnostics (GUI-thread stall watchdog, logging categories) -----------
add_library(qbucket_diagnostics STATIC
    diagnostics/Logging.h
    diagnostics/Logging.cpp
    diagnostics/StallWatchdog.h
    diagnostics/StallWatchdog.cpp
)
target_include_directories(qbucket_diagnostics PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(qbucket_diagnostics
    PUBLIC  Qt6::Core
    PRIVATE qbucket::compile_options
)

# --- Domain: plain app-level value types -------------------------------------
add_library(qbucket_domain INTERFACE
    domain/Account.h
    domain/Location.h
)
target_include_directories(qbucket_domain INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(qbucket_domain INTERFACE qbucket::s3 Qt6::Core)

# --- Accounts: account store, keychain-backed secrets ------------------------
add_library(qbucket_accounts STATIC
    accounts/AccountStore.h
    accounts/AccountStore.cpp
    accounts/CapabilityStore.h
    accounts/CapabilityStore.cpp
    accounts/KeychainBackend.h
    accounts/KeychainCredentialsProvider.h
    accounts/KeychainCredentialsProvider.cpp
    accounts/QtKeychain.h
    accounts/QtKeychainBackend.h
    accounts/QtKeychainBackend.cpp
    accounts/SecretStore.h
    accounts/SecretStore.cpp
)
target_include_directories(qbucket_accounts PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(qbucket_accounts
    PUBLIC  qbucket_domain qbucket_diagnostics qbucket::s3 Qt6::Core
    PRIVATE qbucket::keychain qbucket::compile_options
)

# --- Controllers (application logic, no widgets) ---------------------------
add_library(qbucket_controllers STATIC
    controllers/AccountSessions.h
    controllers/AccountSessions.cpp
    controllers/ActivityLog.h
    controllers/ActivityLog.cpp
    controllers/BrowseController.h
    controllers/BrowseController.cpp
    controllers/CapabilityController.h
    controllers/CapabilityController.cpp
    controllers/Job.h
    controllers/Job.cpp
    controllers/Journal.h
    controllers/Journal.cpp
    controllers/LocalPathMapper.h
    controllers/LocalPathMapper.cpp
    controllers/LocalScanner.h
    controllers/LocalScanner.cpp
    controllers/OperationController.h
    controllers/OperationController.cpp
    controllers/PlanFile.h
    controllers/PlanFile.cpp
    controllers/RenameJob.h
    controllers/RenameJob.cpp
    controllers/CreateFolderJob.h
    controllers/CreateFolderJob.cpp
    controllers/DeleteJob.h
    controllers/DeleteJob.cpp
    controllers/DownloadJob.h
    controllers/DownloadJob.cpp
    controllers/ConnectionTester.h
    controllers/ConnectionTester.cpp
    controllers/SettingsController.h
    controllers/SettingsController.cpp
    controllers/TransferService.h
    controllers/TransferService.cpp
    controllers/UploadJob.h
    controllers/UploadJob.cpp
)
target_include_directories(qbucket_controllers PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(qbucket_controllers
    PUBLIC  qbucket_domain qbucket_accounts qbucket::s3 qbucket::s3_transfer Qt6::Core
    PRIVATE qbucket::compile_options
)

# --- Models (Qt item models over the controllers' data, no widgets) -------
add_library(qbucket_models STATIC
    models/AccountTreeModel.h
    models/AccountTreeModel.cpp
    models/ObjectTableModel.h
    models/ObjectTableModel.cpp
)
target_include_directories(qbucket_models PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(qbucket_models
    PUBLIC  qbucket_controllers Qt6::Core
    PRIVATE qbucket::compile_options
)

# --- UI foundation: design tokens, stylesheet, icons, theme switching ------
# Ported from xyz-profiler. Kept in its own library so the tests can check the
# design system without building the main window.
add_library(qbucket_ui_foundation STATIC
    ui/Theme.h
    ui/Theme.cpp
    ui/DarkFusionStyle.h
    ui/DarkFusionStyle.cpp
    ui/IconFactory.h
    ui/IconFactory.cpp
    ui/ThemeManager.h
    ui/ThemeManager.cpp
)
target_include_directories(qbucket_ui_foundation PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(qbucket_ui_foundation
    PUBLIC  Qt6::Gui Qt6::Widgets Qt6::Svg
    PRIVATE qbucket::compile_options
)

# --- Application windows & dialogs -----------------------------------------
# A library rather than part of the executable so the window smoke test can
# build the real MainWindow.
add_library(qbucket_app_ui STATIC
    ui/AccountDialog.h
    ui/AccountDialog.cpp
    ui/AccountSidebar.h
    ui/AccountSidebar.cpp
    ui/AccountTreeDelegate.h
    ui/AccountTreeDelegate.cpp
    ui/CapabilitiesDialog.h
    ui/CapabilitiesDialog.cpp
    ui/DetailPane.h
    ui/DetailPane.cpp
    ui/LogPanel.h
    ui/LogPanel.cpp
    ui/MainWindow.h
    ui/MainWindow.cpp
    ui/ObjectBrowser.h
    ui/ObjectBrowser.cpp
    ui/OperationPreviewDialog.h
    ui/OperationPreviewDialog.cpp
    ui/ObjectRowDelegate.h
    ui/ObjectRowDelegate.cpp
    ui/PathBar.h
    ui/PathBar.cpp
    ui/SettingsDialog.h
    ui/SettingsDialog.cpp
)
target_include_directories(qbucket_app_ui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(qbucket_app_ui PRIVATE
    QBUCKET_VERSION_STRING="${PROJECT_VERSION}"
)
target_link_libraries(qbucket_app_ui
    PUBLIC  qbucket_accounts qbucket_controllers qbucket_models qbucket_ui_foundation Qt6::Widgets
    PRIVATE qbucket::compile_options
)

# --- Application -------------------------------------------------------------
qt_add_executable(qbucket
    main.cpp
    ${CMAKE_SOURCE_DIR}/resources/app.qrc
    $<$<PLATFORM_ID:Windows>:${CMAKE_SOURCE_DIR}/resources/app.rc>
)

set_target_properties(qbucket PROPERTIES
    WIN32_EXECUTABLE TRUE
)

target_compile_definitions(qbucket PRIVATE
    QBUCKET_VERSION_STRING="${PROJECT_VERSION}"
    QBUCKET_APP_ID="${QBUCKET_APP_ID}"
)

target_link_libraries(qbucket PRIVATE
    qbucket_app_ui
    qbucket_diagnostics
    qbucket_controllers
    qbucket_ui_foundation
    qbucket::compile_options
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
)

# qbucket_en.ts holds the English plural forms only ("%n object(s)" →
# "1 object" / "2 objects"); without it Qt shows the source text as is.
if(Qt6_VERSION VERSION_GREATER_EQUAL 6.7)
    qt_add_translations(qbucket
        TS_FILES ${CMAKE_SOURCE_DIR}/translations/qbucket_de.ts
        PLURALS_TS_FILE ${CMAKE_SOURCE_DIR}/translations/qbucket_en.ts
        RESOURCE_PREFIX "/i18n"
    )
else()
    qt_add_translations(qbucket
        TS_FILES ${CMAKE_SOURCE_DIR}/translations/qbucket_de.ts ${CMAKE_SOURCE_DIR}/translations/qbucket_en.ts
        RESOURCE_PREFIX "/i18n"
    )
endif()

# --- Install (Linux desktop integration; used by the Flatpak build) ---------
install(TARGETS qbucket RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
if(UNIX AND NOT APPLE)
    install(FILES ${CMAKE_SOURCE_DIR}/packaging/linux/${QBUCKET_APP_ID}.desktop
            DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
    install(FILES ${CMAKE_SOURCE_DIR}/packaging/linux/${QBUCKET_APP_ID}.metainfo.xml
            DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo)
    install(FILES ${CMAKE_SOURCE_DIR}/resources/${QBUCKET_APP_ID}.svg
            DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
endif()

# --- Windows: deploy the Qt runtime next to the exe -------------------------
if(WIN32)
    # Prefer Qt's imported target: it always belongs to the Qt that
    # find_package() resolved. A cached find_program() result goes stale
    # after a Qt upgrade and keeps pointing at the removed install.
    if(TARGET Qt6::windeployqt)
        set(_windeployqt "$<TARGET_FILE:Qt6::windeployqt>")
    else()
        find_program(WINDEPLOYQT_EXE windeployqt
            HINTS "${QT6_INSTALL_PREFIX}/${QT6_INSTALL_BINS}")
        set(_windeployqt "${WINDEPLOYQT_EXE}")
    endif()
    if(_windeployqt)
        add_custom_command(TARGET qbucket POST_BUILD
            COMMAND "${_windeployqt}"
                    --translations de,en
                    --no-system-d3d-compiler
                    --no-opengl-sw
                    "$<TARGET_FILE:qbucket>"
            COMMENT "Deploying Qt runtime next to qbucket.exe"
            VERBATIM
        )
    else()
        message(WARNING
            "windeployqt not found — qbucket.exe will need Qt's bin/ on PATH.")
    endif()
endif()
