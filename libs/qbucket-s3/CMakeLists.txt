# qbucket-s3 — reusable S3 client library (Qt6 Core + Network).
#
# Builds standalone:
#   cmake -S libs/qbucket-s3 -B build-s3 -DCMAKE_PREFIX_PATH=<Qt prefix>
# or as part of a larger project via add_subdirectory(); the parent can preset
# the QBUCKET_S3_* options with plain set() before adding it (CMP0077).
#
# Rules (PLAN.md §3.0): Qt Core / Network / Concurrent only, no app types, no
# QSettings, no hard-coded paths. Public API = include/qbucket/s3/**.
cmake_minimum_required(VERSION 3.22)

project(qbucket-s3
    VERSION 0.1.0
    DESCRIPTION "Threaded, provider-aware S3 client library for Qt 6"
    HOMEPAGE_URL "https://github.com/theinvisible/qbucket"
    LANGUAGES CXX
)

# PROJECT_IS_TOP_LEVEL needs CMake 3.21 — covered by the minimum above.
option(QBUCKET_S3_BUILD_SHARED        "Build qbucket-s3 as a shared library"            OFF)
option(QBUCKET_S3_BUILD_TESTS         "Build the qbucket-s3 test suite"                 ${PROJECT_IS_TOP_LEVEL})
option(QBUCKET_S3_BUILD_EXAMPLES      "Build the examples (s3ls)"                       ${PROJECT_IS_TOP_LEVEL})
option(QBUCKET_S3_INSTALL             "Generate install rules and the CMake package"    ${PROJECT_IS_TOP_LEVEL})
option(QBUCKET_S3_WARNINGS_AS_ERRORS  "Treat compiler warnings as errors"               OFF)
set(QBUCKET_S3_SANITIZE "" CACHE STRING
    "Sanitizers for GCC/Clang: empty, 'address,undefined' or 'thread'")

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Network Concurrent)

include(GNUInstallDirs)
include(GenerateExportHeader)
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/QBucketS3BuildOptions.cmake)

set(_s3_generated_include "${CMAKE_CURRENT_BINARY_DIR}/include")

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/include/qbucket/s3/Version.h.in
    ${_s3_generated_include}/qbucket/s3/Version.h
    @ONLY
)

# --- qbucket::s3 -------------------------------------------------------------
if(QBUCKET_S3_BUILD_SHARED)
    set(_s3_lib_type SHARED)
else()
    set(_s3_lib_type STATIC)
endif()

add_library(qbucket_s3 ${_s3_lib_type}
    include/qbucket/s3/Capabilities.h
    include/qbucket/s3/Credentials.h
    include/qbucket/s3/Endpoint.h
    include/qbucket/s3/Error.h
    include/qbucket/s3/ListObjectsPager.h
    include/qbucket/s3/ListObjectVersionsPager.h
    include/qbucket/s3/Logging.h
    include/qbucket/s3/Operation.h
    include/qbucket/s3/Presigner.h
    include/qbucket/s3/RequestObserver.h
    include/qbucket/s3/Result.h
    include/qbucket/s3/Session.h
    include/qbucket/s3/Types.h
    include/qbucket/s3/BucketConfig.h
    include/qbucket/s3/providers/AwsProfile.h
    include/qbucket/s3/providers/CephProfile.h
    include/qbucket/s3/providers/GarageProfile.h
    include/qbucket/s3/providers/GenericS3Profile.h
    include/qbucket/s3/providers/HetznerProfile.h
    include/qbucket/s3/providers/ProviderProfile.h
    include/qbucket/s3/providers/ProviderRegistry.h
    src/capabilities/Capabilities.cpp
    src/capabilities/CapabilityProber.cpp
    src/capabilities/ProbeInterpreter.h
    src/capabilities/ProbeInterpreter.cpp
    src/client/FileRangeDevice.h
    src/client/FileRangeDevice.cpp
    src/client/RequestPipeline.h
    src/client/RequestPipeline.cpp
    src/client/RetryPolicy.h
    src/client/RetryPolicy.cpp
    src/client/S3Client.h
    src/client/S3Client.cpp
    src/config/BucketConfig.cpp
    src/core/Credentials.cpp
    src/core/Endpoint.cpp
    src/core/Error.cpp
    src/core/ErrorMapping.h
    src/core/ErrorMapping.cpp
    src/core/HttpDate.h
    src/core/HttpDate.cpp
    src/core/Mailbox.h
    src/core/Mailbox.cpp
    src/core/Operation.cpp
    src/core/RequestObserver.cpp
    src/http/S3Uri.h
    src/http/S3Uri.cpp
    src/http/UriEncoding.h
    src/http/UriEncoding.cpp
    src/signing/Presigner.cpp
    src/signing/SigV4Signer.h
    src/logging/Logging.cpp
    src/logging/Redaction.h
    src/logging/Redaction.cpp
    src/providers/AwsProfile.cpp
    src/providers/CephProfile.cpp
    src/providers/GarageProfile.cpp
    src/providers/HetznerProfile.cpp
    src/providers/ProviderProfile.cpp
    src/providers/ProviderRegistry.cpp
    src/session/ListObjectVersionsPager.cpp
    src/session/ListObjectsPager.cpp
    src/session/Session.cpp
    src/signing/SigV4Signer.cpp
    src/xml/XmlParsers.h
    src/xml/XmlParsers.cpp
    src/xml/XmlWriters.h
    src/xml/XmlWriters.cpp
)
add_library(qbucket::s3 ALIAS qbucket_s3)

generate_export_header(qbucket_s3
    BASE_NAME QBUCKET_S3
    EXPORT_FILE_NAME ${_s3_generated_include}/qbucket/s3/Export.h
)

set_target_properties(qbucket_s3 PROPERTIES
    EXPORT_NAME s3
    OUTPUT_NAME qbucket-s3
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}
    AUTOMOC ON
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
)

target_compile_features(qbucket_s3 PUBLIC cxx_std_20)
if(NOT QBUCKET_S3_BUILD_SHARED)
    target_compile_definitions(qbucket_s3 PUBLIC QBUCKET_S3_STATIC_DEFINE)
endif()

target_include_directories(qbucket_s3
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${_s3_generated_include}>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(qbucket_s3 PUBLIC Qt6::Core Qt6::Network)
qbucket_s3_apply_build_options(qbucket_s3)

# --- qbucket::s3_transfer ----------------------------------------------------
# Transfers on top of the public Session API: part planning, hashing,
# adaptive concurrency, resume persistence, uploader / downloader / copier.
add_library(qbucket_s3_transfer ${_s3_lib_type}
    include/qbucket/s3/transfer/AdaptiveConcurrency.h
    include/qbucket/s3/transfer/Copier.h
    include/qbucket/s3/transfer/Downloader.h
    include/qbucket/s3/transfer/Hasher.h
    include/qbucket/s3/transfer/JsonFileResumeStore.h
    include/qbucket/s3/transfer/PartPlanner.h
    include/qbucket/s3/transfer/ResumeStore.h
    include/qbucket/s3/transfer/Uploader.h
    src/transfer/AdaptiveConcurrency.cpp
    src/transfer/Async.h
    src/transfer/Async.cpp
    src/transfer/Common.h
    src/transfer/Copier.cpp
    src/transfer/Downloader.cpp
    src/transfer/Hasher.cpp
    src/transfer/PartPlanner.cpp
    src/transfer/ResumeStore.cpp
    src/transfer/Uploader.cpp
)
add_library(qbucket::s3_transfer ALIAS qbucket_s3_transfer)

generate_export_header(qbucket_s3_transfer
    BASE_NAME QBUCKET_S3_TRANSFER
    EXPORT_FILE_NAME ${_s3_generated_include}/qbucket/s3/transfer/Export.h
)

set_target_properties(qbucket_s3_transfer PROPERTIES
    EXPORT_NAME s3_transfer
    OUTPUT_NAME qbucket-s3-transfer
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}
    AUTOMOC ON
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
)
if(NOT QBUCKET_S3_BUILD_SHARED)
    target_compile_definitions(qbucket_s3_transfer PUBLIC QBUCKET_S3_TRANSFER_STATIC_DEFINE)
endif()
target_include_directories(qbucket_s3_transfer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(qbucket_s3_transfer PUBLIC qbucket::s3 Qt6::Core Qt6::Concurrent)
qbucket_s3_apply_build_options(qbucket_s3_transfer)

# --- qbucket::s3_testsupport -------------------------------------------------
# FakeS3Server for this library's tests and for consumers' own tests. Always
# static: it is test code, and that spares it an export macro of its own.
add_library(qbucket_s3_testsupport STATIC
    include/qbucket/s3/testing/FakeObjectStore.h
    include/qbucket/s3/testing/FakeS3Server.h
    testsupport/FakeObjectStore.cpp
    testsupport/FakeS3Server.cpp
)
add_library(qbucket::s3_testsupport ALIAS qbucket_s3_testsupport)
set_target_properties(qbucket_s3_testsupport PROPERTIES
    EXPORT_NAME s3_testsupport
    OUTPUT_NAME qbucket-s3-testsupport
    AUTOMOC ON
    POSITION_INDEPENDENT_CODE ON
)
target_include_directories(qbucket_s3_testsupport PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(qbucket_s3_testsupport PUBLIC qbucket::s3 Qt6::Network)
qbucket_s3_apply_build_options(qbucket_s3_testsupport)

# --- Examples ----------------------------------------------------------------
if(QBUCKET_S3_BUILD_EXAMPLES)
    add_subdirectory(examples/s3ls)
    add_subdirectory(examples/s3cp)
endif()

# --- Tests -------------------------------------------------------------------
if(QBUCKET_S3_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# --- Install + CMake package -------------------------------------------------
if(QBUCKET_S3_INSTALL)
    include(CMakePackageConfigHelpers)

    install(TARGETS qbucket_s3 qbucket_s3_transfer qbucket_s3_testsupport
        EXPORT QBucketS3Targets
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    )
    install(DIRECTORY include/ ${_s3_generated_include}/
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
        FILES_MATCHING PATTERN "*.h"
    )

    set(_s3_cmake_dir ${CMAKE_INSTALL_LIBDIR}/cmake/QBucketS3)
    install(EXPORT QBucketS3Targets
        NAMESPACE qbucket::
        DESTINATION ${_s3_cmake_dir}
    )
    configure_package_config_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/cmake/QBucketS3Config.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/QBucketS3Config.cmake
        INSTALL_DESTINATION ${_s3_cmake_dir}
    )
    # Pre-1.0: a minor bump may break the API, so only the same minor matches.
    write_basic_package_version_file(
        ${CMAKE_CURRENT_BINARY_DIR}/QBucketS3ConfigVersion.cmake
        COMPATIBILITY SameMinorVersion
    )
    install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/QBucketS3Config.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/QBucketS3ConfigVersion.cmake
        DESTINATION ${_s3_cmake_dir}
    )
endif()
