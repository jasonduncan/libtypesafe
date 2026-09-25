# A separate project that uses libtypesafe the way a stranger would: either from an installed
# package (find_package) or vendored (add_subdirectory). Driven by tests/consumer/run.cmake.
cmake_minimum_required(VERSION 3.24)
project(libtypesafe_consumer LANGUAGES CXX)

if(LIBTYPESAFE_SOURCE_DIR)
  add_subdirectory(${LIBTYPESAFE_SOURCE_DIR} libtypesafe)
else()
  find_package(libtypesafe 0.1 REQUIRED)
endif()

add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE libtypesafe::libtypesafe)
target_compile_features(consumer PRIVATE cxx_std_17)

enable_testing()
add_test(NAME consumer COMMAND consumer)
