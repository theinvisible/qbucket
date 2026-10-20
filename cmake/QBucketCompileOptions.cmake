# Project-wide compile options, attached to every first-party target by
# linking `qbucket::compile_options`. Third-party code (Qt headers, later
# QtKeychain) stays out of the warning set: Qt's imported targets are SYSTEM
# includes on GCC/Clang, and MSVC gets /external:anglebrackets.

add_library(qbucket_compile_options INTERFACE)
add_library(qbucket::compile_options ALIAS qbucket_compile_options)

# Qt strictness: no implicit const char* <-> QString/QByteArray conversions,
# no narrowing in signal/slot connections, no APIs deprecated before our
# minimum Qt version.
target_compile_definitions(qbucket_compile_options INTERFACE
    QT_NO_CAST_FROM_ASCII
    QT_NO_CAST_TO_ASCII
    QT_NO_CAST_FROM_BYTEARRAY
    QT_NO_URL_CAST_FROM_STRING
    QT_NO_NARROWING_CONVERSIONS_IN_CONNECT
    QT_NO_FOREACH
    QT_DISABLE_DEPRECATED_UP_TO=0x060500
)

if(MSVC)
    target_compile_options(qbucket_compile_options INTERFACE
        /W4
        /permissive-
        /utf-8
        /Zc:__cplusplus
        /external:anglebrackets
        /external:W0
    )
    if(QBUCKET_WARNINGS_AS_ERRORS)
        target_compile_options(qbucket_compile_options INTERFACE /WX)
    endif()
else()
    target_compile_options(qbucket_compile_options INTERFACE
        -Wall
        -Wextra
        -Wnon-virtual-dtor
        -Wimplicit-fallthrough
    )
    if(QBUCKET_WARNINGS_AS_ERRORS)
        target_compile_options(qbucket_compile_options INTERFACE -Werror)
    endif()
    if(QBUCKET_SANITIZERS)
        target_compile_options(qbucket_compile_options INTERFACE
            -fsanitize=address,undefined
            -fno-omit-frame-pointer
            -fno-sanitize-recover=undefined
        )
        target_link_options(qbucket_compile_options INTERFACE
            -fsanitize=address,undefined
        )
    endif()
endif()
