# 3.5 is required for imported boost targets
# 3.6 is required for the PkgConfig module's IMPORTED_TARGET library feature
cmake_minimum_required(VERSION 3.6)

# include headers to make CLion happy
file(GLOB HEADERS ${PROJECT_SOURCE_DIR}/include/linuxdeploy/core/*.h)

find_package(Boost REQUIRED COMPONENTS filesystem regex)
find_package(Threads)

find_package(PkgConfig)
pkg_check_modules(magick++ REQUIRED IMPORTED_TARGET Magick++)

message(STATUS "Generating excludelist")
execute_process(
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/generate-excludelist.sh
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)

add_library(core elf.cpp log.cpp  appdir.cpp desktopfile.cpp ${HEADERS})
target_link_libraries(core Boost::filesystem Boost::regex subprocess cpp-feather-ini-parser PkgConfig::magick++ ${CMAKE_THREAD_LIBS_INIT})
target_include_directories(core PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
target_compile_definitions(core PUBLIC -DBOOST_NO_CXX11_SCOPED_ENUMS)

add_executable(linuxdeploy main.cpp)
target_link_libraries(linuxdeploy core args)

set_target_properties(linuxdeploy PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
