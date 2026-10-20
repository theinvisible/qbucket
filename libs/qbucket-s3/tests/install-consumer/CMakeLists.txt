# Builds against an *installed* qbucket-s3 via find_package — proves the
# exported CMake package works. Used by the s3-standalone CI job:
#   cmake --install build-s3 --prefix stage
#   cmake -S libs/qbucket-s3/tests/install-consumer -B build-consumer \
#         -DCMAKE_PREFIX_PATH="$PWD/stage;<Qt prefix>"
cmake_minimum_required(VERSION 3.22)
project(qbucket-s3-install-consumer LANGUAGES CXX)

find_package(QBucketS3 0.1 REQUIRED)

add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE qbucket::s3)
