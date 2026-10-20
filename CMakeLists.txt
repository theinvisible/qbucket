cmake_minimum_required(VERSION 3.22)

project(qbucket
    VERSION 0.1.0
    DESCRIPTION "Desktop browser for S3 and S3-compatible object storage"
    HOMEPAGE_URL "https://github.com/theinvisible/qbucket"
    LANGUAGES CXX
)

set(PROJECT_LICENSE "MIT")
# Reverse-DNS id shared by the .desktop file, AppStream metainfo, the icon and
# the Flatpak manifest. Permanent once published on Flathub — never change it.
set(QBUCKET_APP_ID "io.github.theinvisible.QBucket")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# clang-tidy (CI) reads the compilation database.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(QBUCKET_BUILD_TESTS          "Build the test suite"                          ON)
option(QBUCKET_WARNINGS_AS_ERRORS   "Treat compiler warnings as errors (CI)"        OFF)
option(QBUCKET_SANITIZERS           "AddressSanitizer + UBSan (GCC/Clang only)"     OFF)

set(_qbucket_qt_components Core Gui Widgets Svg Network LinguistTools)
if(QBUCKET_BUILD_TESTS)
    list(APPEND _qbucket_qt_components Test)
endif()

find_package(Qt6 6.5 QUIET COMPONENTS ${_qbucket_qt_components})

if(NOT Qt6_FOUND)
    set(_qt_search_roots "H:/Qt" "C:/Qt")
    set(_qt_found_dir "")
    foreach(_qt_root IN LISTS _qt_search_roots)
        file(GLOB _qt_versions LIST_DIRECTORIES true "${_qt_root}/6.*")
        # NATURAL, or "6.9.x" would sort above "6.12.x".
        list(SORT _qt_versions COMPARE NATURAL ORDER DESCENDING)
        foreach(_qt_ver IN LISTS _qt_versions)
            file(GLOB _qt_kits LIST_DIRECTORIES true
                 "${_qt_ver}/msvc*_64" "${_qt_ver}/mingw*_64")
            foreach(_qt_kit IN LISTS _qt_kits)
                if(EXISTS "${_qt_kit}/lib/cmake/Qt6/Qt6Config.cmake")
                    set(_qt_found_dir "${_qt_kit}")
                    break()
                endif()
            endforeach()
            if(_qt_found_dir)
                break()
            endif()
        endforeach()
        if(_qt_found_dir)
            break()
        endif()
    endforeach()
    if(_qt_found_dir)
        set(Qt6_DIR "${_qt_found_dir}/lib/cmake/Qt6" CACHE PATH "Qt6 config dir" FORCE)
        # Drop prefixes whose Qt install has since been removed (e.g. the
        # previous version after an upgrade) so they don't pile up in the cache.
        set(_qt_prefixes "")
        foreach(_p IN LISTS CMAKE_PREFIX_PATH)
            if(EXISTS "${_p}")
                list(APPEND _qt_prefixes "${_p}")
            endif()
        endforeach()
        set(CMAKE_PREFIX_PATH ${_qt_prefixes})
        list(PREPEND CMAKE_PREFIX_PATH "${_qt_found_dir}")
        list(REMOVE_DUPLICATES CMAKE_PREFIX_PATH)
        set(CMAKE_PREFIX_PATH "${CMAKE_PREFIX_PATH}" CACHE STRING "" FORCE)
        message(STATUS "Auto-detected Qt6 at ${_qt_found_dir}")
        find_package(Qt6 6.5 REQUIRED COMPONENTS ${_qbucket_qt_components})
    else()
        message(FATAL_ERROR
            "Qt6 not found — set Qt6_DIR / CMAKE_PREFIX_PATH or drop a Qt "
            "install under H:/Qt or C:/Qt.")
    endif()
endif()

qt_standard_project_setup()

include(GNUInstallDirs)
include(cmake/QBucketCompileOptions.cmake)
include(cmake/QBucketQtKeychain.cmake)

if(QBUCKET_BUILD_TESTS)
    enable_testing()
endif()

# --- S3 library (libs/qbucket-s3, standalone-capable) ----------------------
# Forward our build settings; the library's own options default to "off"
# when it is not the top-level project. Never installed with the app.
set(QBUCKET_S3_BUILD_TESTS ${QBUCKET_BUILD_TESTS})
set(QBUCKET_S3_WARNINGS_AS_ERRORS ${QBUCKET_WARNINGS_AS_ERRORS})
set(QBUCKET_S3_INSTALL OFF)
if(QBUCKET_SANITIZERS)
    set(QBUCKET_S3_SANITIZE "address,undefined")
endif()
add_subdirectory(libs/qbucket-s3)

add_subdirectory(src)

if(QBUCKET_BUILD_TESTS)
    add_subdirectory(tests)
    add_subdirectory(bench)
endif()
