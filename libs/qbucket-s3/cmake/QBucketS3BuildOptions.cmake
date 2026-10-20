# Warning, strictness and sanitizer setup for qbucket-s3's own targets.
#
# Functions rather than an INTERFACE target: a static library that links a
# PRIVATE interface target still exports it as a $<LINK_ONLY:...> dependency,
# and install(EXPORT) then fails because that target is in no export set.
# Everything here is PRIVATE — consumers never inherit our flags.

# Compiler warnings only (also used for the "relaxed" public-header check).
function(qbucket_s3_apply_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE
            /W4
            /permissive-
            /utf-8
            /Zc:__cplusplus
            /external:anglebrackets
            /external:W0
        )
        if(QBUCKET_S3_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${target} PRIVATE
            -Wall
            -Wextra
            -Wpedantic
            -Wnon-virtual-dtor
            -Wimplicit-fallthrough
        )
        if(QBUCKET_S3_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()

# Warnings + Qt strictness defines + sanitizers.
function(qbucket_s3_apply_build_options target)
    qbucket_s3_apply_warnings(${target})

    # Same strictness as the app (cmake/QBucketCompileOptions.cmake), plus
    # QT_NO_KEYWORDS: public headers must work for consumers that define it.
    target_compile_definitions(${target} PRIVATE
        QT_NO_CAST_FROM_ASCII
        QT_NO_CAST_TO_ASCII
        QT_NO_CAST_FROM_BYTEARRAY
        QT_NO_URL_CAST_FROM_STRING
        QT_NO_NARROWING_CONVERSIONS_IN_CONNECT
        QT_NO_FOREACH
        QT_NO_KEYWORDS
        QT_DISABLE_DEPRECATED_UP_TO=0x060500
    )

    if(QBUCKET_S3_SANITIZE AND NOT MSVC)
        target_compile_options(${target} PRIVATE
            -fsanitize=${QBUCKET_S3_SANITIZE}
            -fno-omit-frame-pointer
        )
        if(QBUCKET_S3_SANITIZE MATCHES "undefined")
            target_compile_options(${target} PRIVATE -fno-sanitize-recover=undefined)
        endif()
        target_link_options(${target} PRIVATE -fsanitize=${QBUCKET_S3_SANITIZE})
    endif()
endfunction()
