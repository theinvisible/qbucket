# QtKeychain for the app (never for libs/qbucket-s3) → target qbucket::keychain.
#
# QBUCKET_QTKEYCHAIN_PROVIDER:
#   AUTO   - an installed Qt6Keychain if it was built against the same Qt
#            prefix as the Qt we use, else FETCH. (Mixing a distro qtkeychain
#            with an aqtinstall Qt would load two different QtCore libraries.)
#   SYSTEM - an installed Qt6Keychain, no questions asked (Flatpak: the
#            manifest builds it as a module, the build is offline).
#   FETCH  - build the pinned release with FetchContent, static. On Linux
#            this needs libsecret-1 (pkg-config) — QtKeychain fails loudly
#            without it instead of silently leaving GNOME unsupported.
#
# Secrets never fall back to plaintext: the app does not call
# setInsecureFallback().

set(QBUCKET_QTKEYCHAIN_PROVIDER "AUTO" CACHE STRING "Where QtKeychain comes from: AUTO, SYSTEM or FETCH")
set_property(CACHE QBUCKET_QTKEYCHAIN_PROVIDER PROPERTY STRINGS AUTO SYSTEM FETCH)

set(_qbucket_keychain_target "")
set(_qbucket_keychain_fetched FALSE)

if(NOT QBUCKET_QTKEYCHAIN_PROVIDER STREQUAL "FETCH")
    find_package(Qt6Keychain CONFIG QUIET)
    if(Qt6Keychain_FOUND)
        get_filename_component(_kc_cmake_dir "${Qt6Keychain_DIR}" DIRECTORY)
        get_filename_component(_qt_cmake_dir "${Qt6_DIR}" DIRECTORY)
        if(QBUCKET_QTKEYCHAIN_PROVIDER STREQUAL "SYSTEM" OR _kc_cmake_dir STREQUAL _qt_cmake_dir)
            set(_qbucket_keychain_target qt6keychain)
            message(STATUS "QtKeychain: installed package (${Qt6Keychain_DIR})")
        else()
            message(STATUS "QtKeychain: installed package belongs to another Qt (${_kc_cmake_dir}), building our own")
        endif()
    elseif(QBUCKET_QTKEYCHAIN_PROVIDER STREQUAL "SYSTEM")
        message(FATAL_ERROR "QBUCKET_QTKEYCHAIN_PROVIDER=SYSTEM but no Qt6Keychain package was found")
    endif()
endif()

# A function, so BUILD_SHARED_LIBS & co. stay local to QtKeychain.
function(qbucket_fetch_qtkeychain)
    include(FetchContent)
    if(UNIX AND NOT APPLE)
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(QBUCKET_LIBSECRET REQUIRED libsecret-1)
    endif()
    set(BUILD_WITH_QT6 ON)
    set(BUILD_SHARED_LIBS OFF)
    set(BUILD_TRANSLATIONS OFF)
    set(BUILD_TEST_APPLICATION OFF)
    FetchContent_Declare(qtkeychain
        GIT_REPOSITORY https://github.com/frankosterfeld/qtkeychain.git
        GIT_TAG        ad7344c45a86a4f66cbafc4b081b5f7b876cb0b7   # 0.15.0
        GIT_SHALLOW    FALSE
        EXCLUDE_FROM_ALL
    )
    FetchContent_MakeAvailable(qtkeychain)
endfunction()

if(NOT _qbucket_keychain_target)
    qbucket_fetch_qtkeychain()
    set(_qbucket_keychain_target qt6keychain)
    set(_qbucket_keychain_fetched TRUE)
    message(STATUS "QtKeychain: built from source (0.15.0, static)")
endif()

add_library(qbucket_keychain INTERFACE)
add_library(qbucket::keychain ALIAS qbucket_keychain)
target_link_libraries(qbucket_keychain INTERFACE ${_qbucket_keychain_target})
# The include path differs: <qt6keychain/keychain.h> installed,
# <qtkeychain/keychain.h> from source. src/accounts/QtKeychain.h hides it.
if(_qbucket_keychain_fetched)
    target_compile_definitions(qbucket_keychain INTERFACE QBUCKET_QTKEYCHAIN_FETCHED)
endif()
