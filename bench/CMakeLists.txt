# Benchmarks (PLAN.md §13.4) — built with the tests so they don't rot, never
# run by ctest. Meaningful only in release builds.
qt_add_executable(bench_listing bench_listing.cpp)
target_include_directories(bench_listing PRIVATE ${CMAKE_SOURCE_DIR}/tests)
target_link_libraries(bench_listing PRIVATE
    qbucket_app_ui
    qbucket::s3_testsupport
    qbucket::compile_options
)
if(WIN32)
    target_link_libraries(bench_listing PRIVATE psapi)
endif()
set_target_properties(bench_listing PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
