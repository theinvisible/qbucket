# s3ls — a tiny CLI over qbucket-s3's public API; documentation that compiles.
# Builds in-tree (as part of qbucket-s3) or on its own against an installed
# package:
#   cmake -S libs/qbucket-s3/examples/s3ls -B build-s3ls -DCMAKE_PREFIX_PATH="<stage>;<Qt>"
cmake_minimum_required(VERSION 3.22)
project(s3ls LANGUAGES CXX)

if(NOT TARGET qbucket::s3)
    find_package(QBucketS3 0.1 REQUIRED)
endif()

add_executable(s3ls main.cpp)
target_compile_features(s3ls PRIVATE cxx_std_20)
target_link_libraries(s3ls PRIVATE qbucket::s3)

# In-tree: same warnings / strictness / sanitizers as the library (a
# sanitized static library can't be linked without them).
if(COMMAND qbucket_s3_apply_build_options)
    qbucket_s3_apply_build_options(s3ls)
endif()
