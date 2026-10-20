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
