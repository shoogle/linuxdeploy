cmake_minimum_required(VERSION 3.2)

project(linuxdeploy C CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(LINUXDEPLOY_VERSION 0.1-alpha-1)
add_definitions(-DLINUXDEPLOY_VERSION="${LINUXDEPLOY_VERSION}")

add_subdirectory(lib)

add_subdirectory(src)
