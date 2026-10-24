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
    domain/Bookmark.h
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
    accounts/LocationStore.h
    accounts/LocationStore.cpp
    accounts/RcloneConfigReader.h
    accounts/RcloneConfigReader.cpp
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
    controllers/AbortUploadsJob.h
    controllers/AbortUploadsJob.cpp
    controllers/AccountImporter.h
    controllers/AccountImporter.cpp
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
    controllers/OpenFileService.h
    controllers/OpenFileService.cpp
    controllers/ReplaceObjectJob.h
    controllers/ReplaceObjectJob.cpp
    controllers/RenameJob.h
    controllers/RenameJob.cpp
    controllers/DeleteVersionsJob.h
    controllers/DeleteVersionsJob.cpp
    controllers/DeleteBucketJob.h
    controllers/DeleteBucketJob.cpp
    controllers/CreateBucketJob.h
    controllers/CreateBucketJob.cpp
    controllers/InPlaceCopy.h
    controllers/InPlaceCopy.cpp
    controllers/ObjectCopyRunner.h
    controllers/ObjectCopyRunner.cpp
    controllers/RestoreVersionsJob.h
    controllers/RestoreVersionsJob.cpp
    controllers/SetBucketConfigJob.h
    controllers/SetBucketConfigJob.cpp
    controllers/SetBucketVersioningJob.h
    controllers/SetBucketVersioningJob.cpp
    controllers/VersionPurger.h
    controllers/VersionPurger.cpp
    controllers/VersionWalk.h
    controllers/VersionWalk.cpp
    controllers/CopyJob.h
    controllers/CopyJob.cpp
    controllers/ObjectSnapshot.h
    controllers/ObjectSnapshot.cpp
    controllers/EditObjectsJob.h
    controllers/EditObjectsJob.cpp
    controllers/ObjectLockJob.h
    controllers/ObjectLockJob.cpp
    controllers/ObjectPropertyJob.h
    controllers/ObjectPropertyJob.cpp
    controllers/PresignService.h
    controllers/PresignService.cpp
    controllers/FolderSizeTask.h
    controllers/FolderSizeTask.cpp
    controllers/CreateFolderJob.h
    controllers/CreateFolderJob.cpp
    controllers/DeleteJob.h
    controllers/DeleteJob.cpp
    controllers/DiscardTransfersJob.h
    controllers/DiscardTransfersJob.cpp
    controllers/DragStaging.h
    controllers/DragStaging.cpp
    controllers/DownloadJob.h
    controllers/DownloadJob.cpp
    controllers/ConnectionTester.h
    controllers/ConnectionTester.cpp
    controllers/SelfTest.h
    controllers/SelfTest.cpp
    controllers/SettingsController.h
    controllers/SettingsController.cpp
    controllers/TransferJobStore.h
    controllers/TransferJobStore.cpp
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
    models/ObjectSelection.h
    models/ObjectSelection.cpp
    models/AccountTreeModel.h
    models/AccountTreeModel.cpp
    models/BookmarkModel.h
    models/BookmarkModel.cpp
    models/MultipartUploadModel.h
    models/MultipartUploadModel.cpp
    models/ObjectTableModel.h
    models/ObjectTableModel.cpp
    models/TransferModel.h
    models/TransferModel.cpp
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
    ui/UndeleteDialog.h
    ui/UndeleteDialog.cpp
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
# --- Platform: what differs between Windows and Linux (drag out) -------------
# The only place with platform #ifdefs for features; the Windows sources
# (virtual-file drag) are added below.
add_library(qbucket_platform STATIC
    platform/Console.h
    platform/Console.cpp
    platform/KeychainService.h
    platform/KeychainService.cpp
    platform/ExternalDrag.h
    platform/ExternalDrag.cpp
    platform/StagedDrag.h
    platform/StagedDrag.cpp
    platform/StagingMimeData.h
    platform/StagingMimeData.cpp
    platform/VirtualFileReader.h
    platform/VirtualFileReader.cpp
    platform/VirtualFileSource.h
)
if(WIN32)
    # Explorer's virtual files: CFSTR_FILEDESCRIPTORW / CFSTR_FILECONTENTS.
    target_sources(qbucket_platform PRIVATE
        platform/win/DropSource.h
        platform/win/DropSource.cpp
        platform/win/RangeReaderStream.h
        platform/win/RangeReaderStream.cpp
        platform/win/VirtualFileDataObject.h
        platform/win/VirtualFileDataObject.cpp
        platform/win/VirtualFileDrag.h
        platform/win/VirtualFileDrag.cpp
    )
    target_compile_definitions(qbucket_platform PUBLIC NOMINMAX UNICODE _UNICODE _WIN32_WINNT=0x0A00 WINVER=0x0A00)
    target_link_libraries(qbucket_platform PUBLIC ole32 shell32 uuid)
endif()
if(UNIX AND NOT APPLE)
    # Asks the session bus whether a keychain service exists (KeychainService).
    target_link_libraries(qbucket_platform PRIVATE Qt6::DBus)
endif()
target_include_directories(qbucket_platform PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(qbucket_platform
    PUBLIC  qbucket_controllers Qt6::Gui
    PRIVATE qbucket::compile_options
)

add_library(qbucket_app_ui STATIC
    ui/AccountDialog.h
    ui/AccountDialog.cpp
    ui/AccountSidebar.h
    ui/AccountSidebar.cpp
    ui/AccountTreeDelegate.h
    ui/AccountTreeDelegate.cpp
    ui/BucketPropertiesDialog.h
    ui/AclSection.h
    ui/AclSection.cpp
    ui/BucketConfigPage.h
    ui/BucketConfigPage.cpp
    ui/BucketPropertiesDialog.cpp
    ui/CreateBucketDialog.h
    ui/CreateBucketDialog.cpp
    ui/CodeEditor.h
    ui/CodeEditor.cpp
    ui/CodeHighlighter.h
    ui/CodeHighlighter.cpp
    ui/CorsPage.h
    ui/CorsPage.cpp
    ui/LifecyclePage.h
    ui/LifecyclePage.cpp
    ui/ObjectLockPage.h
    ui/ObjectLockPage.cpp
    ui/OtherConfigPage.h
    ui/OtherConfigPage.cpp
    ui/PolicyPage.h
    ui/PolicyPage.cpp
    ui/PublicAccessBlockSection.h
    ui/PublicAccessBlockSection.cpp
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
    ui/EditMetadataDialog.h
    ui/EditMetadataDialog.cpp
    ui/PresignDialog.h
    ui/PresignDialog.cpp
    ui/OpenFilesDialog.h
    ui/OpenFilesDialog.cpp
    ui/OperationPreviewDialog.h
    ui/OperationPreviewDialog.cpp
    ui/ObjectTableView.h
    ui/ObjectTableView.cpp
    ui/ObjectRowDelegate.h
    ui/ObjectRowDelegate.cpp
    ui/PathBar.h
    ui/PathBar.cpp
    ui/SettingsDialog.h
    ui/SettingsDialog.cpp
    ui/TransferPanel.h
    ui/TransferPanel.cpp
    ui/ImportAccountsDialog.h
    ui/ImportAccountsDialog.cpp
    ui/IncompleteUploadsDialog.h
    ui/IncompleteUploadsDialog.cpp
    ui/ManageBookmarksDialog.h
    ui/ManageBookmarksDialog.cpp
)
target_include_directories(qbucket_app_ui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(qbucket_app_ui PRIVATE
    QBUCKET_VERSION_STRING="${PROJECT_VERSION}"
)
target_link_libraries(qbucket_app_ui
    PUBLIC  qbucket_accounts qbucket_controllers qbucket_models qbucket_platform qbucket_ui_foundation Qt6::Widgets
    PRIVATE qbucket::compile_options
)

# --- Application -------------------------------------------------------------
# The Windows resource carries the icon and the version Explorer shows.
if(WIN32)
    configure_file(${CMAKE_SOURCE_DIR}/resources/app.rc.in ${CMAKE_CURRENT_BINARY_DIR}/app.rc @ONLY)
endif()
qt_add_executable(qbucket
    main.cpp
    ${CMAKE_SOURCE_DIR}/resources/app.qrc
    $<$<PLATFORM_ID:Windows>:${CMAKE_CURRENT_BINARY_DIR}/app.rc>
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
