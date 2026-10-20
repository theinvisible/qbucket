find_package(Qt6 6.5 REQUIRED COMPONENTS Test)

# --- Helper for registering QtTest binaries --------------------------------
# qbucket_s3_add_test(<name> SOURCES <files...> [LIBS <targets...>] [LABELS <labels...>])
#
# One executable + one ctest registration per test, built with the library's
# own strictness (QT_NO_KEYWORDS: use Q_SLOTS / Q_SIGNALS in tests).
function(qbucket_s3_add_test name)
    cmake_parse_arguments(ARG "" "" "SOURCES;LIBS;LABELS" ${ARGN})

    add_executable(${name} ${ARG_SOURCES})
    set_target_properties(${name} PROPERTIES
        AUTOMOC ON
        # Own directory, away from any deployed app (see the top-level tests).
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    )
    target_include_directories(${name} PRIVATE ${PROJECT_SOURCE_DIR}/src)
    target_link_libraries(${name} PRIVATE qbucket::s3 Qt6::Test ${ARG_LIBS})
    target_compile_definitions(${name} PRIVATE
        TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data"
    )
    qbucket_s3_apply_build_options(${name})

    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES
        LABELS "s3;${ARG_LABELS}"
        TIMEOUT 120
    )
    if(WIN32)
        # Let the loader find Qt's DLLs (and ours, in a shared build) without
        # deploying. ENVIRONMENT_MODIFICATION, because a plain "PATH=a;b" in
        # ENVIRONMENT would be split at the semicolons as a CMake list.
        get_target_property(_qt_core_loc Qt6::Core LOCATION)
        get_filename_component(_qt_bin_dir "${_qt_core_loc}" DIRECTORY)
        set_tests_properties(${name} PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:${_qt_bin_dir};PATH=path_list_prepend:$<TARGET_FILE_DIR:qbucket_s3>")
    endif()
endfunction()

# --- Public header checks ----------------------------------------------------
# Every public header must compile for a consumer that does NOT use our
# strictness defines (keywords on, implicit casts allowed). The strict side is
# test_public_headers itself.
add_library(qbucket_s3_headers_relaxed OBJECT public_headers_relaxed.cpp)
target_link_libraries(qbucket_s3_headers_relaxed PRIVATE qbucket::s3)
qbucket_s3_apply_warnings(qbucket_s3_headers_relaxed)

qbucket_s3_add_test(test_public_headers
    SOURCES test_public_headers.cpp
    LIBS    qbucket_s3_headers_relaxed
)

# --- Unit tests ----------------------------------------------------------------
qbucket_s3_add_test(test_result SOURCES test_result.cpp)
qbucket_s3_add_test(test_s3uri  SOURCES test_s3uri.cpp)
qbucket_s3_add_test(test_sigv4  SOURCES test_sigv4.cpp)
qbucket_s3_add_test(test_xml_parsers SOURCES test_xml_parsers.cpp)
qbucket_s3_add_test(test_redaction   SOURCES test_redaction.cpp)
qbucket_s3_add_test(test_fake_s3_server
    SOURCES test_fake_s3_server.cpp
    LIBS    qbucket::s3_testsupport
)
qbucket_s3_add_test(test_s3client_read
    SOURCES test_s3client_read.cpp
    LIBS    qbucket::s3_testsupport
)
qbucket_s3_add_test(test_s3client_write
    SOURCES test_s3client_write.cpp
    LIBS    qbucket::s3_testsupport
)
qbucket_s3_add_test(test_retry_policy SOURCES test_retry_policy.cpp)
qbucket_s3_add_test(test_credentials  SOURCES test_credentials.cpp)
qbucket_s3_add_test(test_provider_generic SOURCES test_provider_generic.cpp)
qbucket_s3_add_test(test_provider_hetzner SOURCES test_provider_hetzner.cpp)
qbucket_s3_add_test(test_provider_aws     SOURCES test_provider_aws.cpp)
qbucket_s3_add_test(test_provider_garage  SOURCES test_provider_garage.cpp)
qbucket_s3_add_test(test_capabilities      SOURCES test_capabilities.cpp)
qbucket_s3_add_test(test_probe_interpreter SOURCES test_probe_interpreter.cpp)
qbucket_s3_add_test(test_capability_prober
    SOURCES test_capability_prober.cpp
    LIBS    qbucket::s3_testsupport
)
qbucket_s3_add_test(test_s3client_faults
    SOURCES test_s3client_faults.cpp
    LIBS    qbucket::s3_testsupport
)
qbucket_s3_add_test(test_session
    SOURCES test_session.cpp
    LIBS    qbucket::s3_testsupport
)
qbucket_s3_add_test(test_session_transfer
    SOURCES test_session_transfer.cpp
    LIBS    qbucket::s3_testsupport
)

# --- Integration tests (opt-in) --------------------------------------------------
# it_s3_backend runs against whatever QBUCKET_IT_* in the environment points at
# and skips when unset. With QBUCKET_S3_IT_LOCAL, one extra test per Docker
# backend (tests/docker, start with it-up.sh) gets its settings from env/*.env.
option(QBUCKET_S3_IT_LOCAL "Register it_s3_backend_<backend> for the Docker test backends" OFF)

qbucket_s3_add_test(it_s3_backend
    SOURCES it_s3_backend.cpp
    LABELS  integration
)
set_tests_properties(it_s3_backend PROPERTIES TIMEOUT 600)

if(QBUCKET_S3_IT_LOCAL)
    foreach(backend ceph versitygw garage)
        file(STRINGS ${CMAKE_CURRENT_SOURCE_DIR}/docker/env/${backend}.env _it_env REGEX "^QBUCKET_IT_")
        add_test(NAME it_s3_backend_${backend} COMMAND it_s3_backend)
        set_tests_properties(it_s3_backend_${backend} PROPERTIES
            ENVIRONMENT "${_it_env}"
            LABELS "s3;integration;docker"
            TIMEOUT 600
        )
    endforeach()
endif()
