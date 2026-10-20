# s3cp — upload with qbucket-s3's transfer engine (multipart, verification,
# resume after a crash). Builds in-tree or against an installed package:
#   cmake -S libs/qbucket-s3/examples/s3cp -B build-s3cp -DCMAKE_PREFIX_PATH="<stage>;<Qt>"
# The library's tests use it as the process they kill (test_transfer_crash).
cmake_minimum_required(VERSION 3.22)
project(s3cp LANGUAGES CXX)

if(NOT TARGET qbucket::s3_transfer)
    find_package(QBucketS3 0.1 REQUIRED)
endif()

add_executable(s3cp main.cpp)
target_compile_features(s3cp PRIVATE cxx_std_20)
target_link_libraries(s3cp PRIVATE qbucket::s3_transfer)

if(COMMAND qbucket_s3_apply_build_options)
    qbucket_s3_apply_build_options(s3cp)
endif()
