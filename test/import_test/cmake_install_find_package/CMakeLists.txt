cmake_minimum_required(VERSION 3.16)
project(my_project LANGUAGES CXX)

# Look for ncaf (installed with CMake package config files)
find_package(ncaf REQUIRED)

add_executable(main main.cc)

# Link with ncaf::ncaf target provided by the package
target_link_libraries(main PRIVATE ncaf::ncaf)
