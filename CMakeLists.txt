cmake_minimum_required(VERSION 3.24)
project(libtypesafe VERSION 0.1.0 LANGUAGES CXX)

option(LIBTYPESAFE_BUILD_EXAMPLES "Build the examples" ${PROJECT_IS_TOP_LEVEL})
option(LIBTYPESAFE_BUILD_TESTS "Build the tests" ${PROJECT_IS_TOP_LEVEL})
option(LIBTYPESAFE_BUILD_API_CHECKS "Compile the examples in C++17/20, with and without exceptions and RTTI" ${PROJECT_IS_TOP_LEVEL})
option(LIBTYPESAFE_INSTALL "Generate install rules and a CMake package" ${PROJECT_IS_TOP_LEVEL})
option(LIBTYPESAFE_FETCH_DEPS "Fetch nlohmann_json when find_package cannot find it" ON)
option(LIBTYPESAFE_WITH_CURL "Build the default libcurl transport" ON)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# The version in the public header must match the project version.
file(STRINGS include/libtypesafe/libtypesafe.hpp _version_line REGEX "std::string_view version = ")
string(REGEX MATCH "\"([0-9]+\\.[0-9]+\\.[0-9]+)\"" _ "${_version_line}")
if(NOT CMAKE_MATCH_1 STREQUAL PROJECT_VERSION)
  message(FATAL_ERROR "libtypesafe.hpp says version ${CMAKE_MATCH_1}; CMakeLists.txt says ${PROJECT_VERSION}")
endif()

# --- Dependencies -----------------------------------------------------------

find_package(nlohmann_json 3.11 QUIET)
if(NOT nlohmann_json_FOUND)
  if(NOT LIBTYPESAFE_FETCH_DEPS)
    message(FATAL_ERROR "nlohmann_json not found; install it or set LIBTYPESAFE_FETCH_DEPS=ON")
  endif()
  # Installed alongside libtypesafe so the installed package can find it.
  set(JSON_Install ${LIBTYPESAFE_INSTALL} CACHE BOOL "" FORCE)
  include(FetchContent)
  FetchContent_Declare(nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
    DOWNLOAD_EXTRACT_TIMESTAMP ON)
  FetchContent_MakeAvailable(nlohmann_json)
endif()

if(LIBTYPESAFE_WITH_CURL)
  find_package(CURL 7.84 REQUIRED)  # thread-safe curl_global_init
endif()

# --- Compiler flags -----------------------------------------------------------

if(MSVC)
  # "strict": how the API checks compile without exceptions or RTTI.
  set(_strict_flags /EHs-c- /D_HAS_EXCEPTIONS=0 /GR-)
  # The library itself never throws, but keeps MSVC's exception model: _HAS_EXCEPTIONS=0 changes
  # the STL and would clash with callers built normally.
  set(_library_flags /GR-)
  set(_rtti_on /GR)
  set(_warnings /W4 /permissive-)
else()
  set(_strict_flags -fno-exceptions -fno-rtti)
  set(_library_flags -fno-exceptions -fno-rtti)
  set(_rtti_on -frtti)
  set(_warnings -Wall -Wextra -Wpedantic)
endif()

# --- Public headers -----------------------------------------------------------

add_library(libtypesafe_headers INTERFACE)
add_library(libtypesafe::headers ALIAS libtypesafe_headers)
set_target_properties(libtypesafe_headers PROPERTIES EXPORT_NAME headers)
target_compile_features(libtypesafe_headers INTERFACE cxx_std_17)
target_include_directories(libtypesafe_headers INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(libtypesafe_headers INTERFACE nlohmann_json::nlohmann_json)

# --- Library ----------------------------------------------------------------

# Static only for now: a shared build needs export macros on the public classes.
add_library(libtypesafe STATIC
  src/client.cpp
  src/config.cpp
  src/headers.cpp
  src/logging.cpp
  src/questions.cpp
  src/result.cpp
  src/transport.cpp
  src/util.cpp
  src/wire.cpp)
add_library(libtypesafe::libtypesafe ALIAS libtypesafe)
target_link_libraries(libtypesafe PUBLIC libtypesafe_headers)
target_include_directories(libtypesafe PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_options(libtypesafe PRIVATE ${_library_flags} ${_warnings})
if(MSVC)
  target_compile_definitions(libtypesafe PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
if(WIN32)
  # curl.h includes windows.h, whose min/max macros break std::min/std::max.
  target_compile_definitions(libtypesafe PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
# transport.cpp holds Transport's key function, so its vtable and typeinfo are emitted there. With
# RTTI on for that one file, callers built with RTTI (the default) can subclass Transport and link.
set_source_files_properties(src/transport.cpp PROPERTIES COMPILE_OPTIONS "${_rtti_on}")
if(LIBTYPESAFE_WITH_CURL)
  target_sources(libtypesafe PRIVATE src/curl_transport.cpp)
  target_link_libraries(libtypesafe PRIVATE CURL::libcurl)
  target_compile_definitions(libtypesafe PRIVATE LIBTYPESAFE_WITH_CURL)
endif()
set_target_properties(libtypesafe PROPERTIES
  OUTPUT_NAME typesafe  # libtypesafe.a / typesafe.lib
  EXPORT_NAME libtypesafe
  VERSION ${PROJECT_VERSION}
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON)

# --- Examples and API checks ------------------------------------------------

set(LIBTYPESAFE_EXAMPLES quickstart triage async game_loop)

if(LIBTYPESAFE_BUILD_API_CHECKS)
  # Compiles, without linking, the examples against the public headers in C++17 and C++20, each
  # with ("default") and without ("strict") exceptions and RTTI.
  foreach(std 17 20)
    foreach(mode default strict)
      set(check libtypesafe_api_check_cxx${std}_${mode})
      list(TRANSFORM LIBTYPESAFE_EXAMPLES PREPEND examples/ OUTPUT_VARIABLE _sources)
      list(TRANSFORM _sources APPEND .cpp)
      add_library(${check} OBJECT ${_sources})
      target_link_libraries(${check} PRIVATE libtypesafe_headers)
      set_target_properties(${check} PROPERTIES CXX_STANDARD ${std} CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
      if(mode STREQUAL "strict")
        target_compile_options(${check} PRIVATE ${_strict_flags})
      endif()
    endforeach()
  endforeach()
endif()

if(LIBTYPESAFE_BUILD_EXAMPLES)
  foreach(example ${LIBTYPESAFE_EXAMPLES})
    add_executable(example_${example} examples/${example}.cpp)
    target_link_libraries(example_${example} PRIVATE libtypesafe::libtypesafe)
  endforeach()
endif()

# --- Install ------------------------------------------------------------------

if(LIBTYPESAFE_INSTALL)
  set(_config_dir ${CMAKE_INSTALL_LIBDIR}/cmake/libtypesafe)
  install(TARGETS libtypesafe libtypesafe_headers EXPORT libtypesafeTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
  install(DIRECTORY include/libtypesafe DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
  install(EXPORT libtypesafeTargets NAMESPACE libtypesafe:: DESTINATION ${_config_dir})
  configure_package_config_file(cmake/libtypesafeConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/libtypesafeConfig.cmake
    INSTALL_DESTINATION ${_config_dir})
  # 0.x: minor versions may break the API.
  write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/libtypesafeConfigVersion.cmake
    COMPATIBILITY SameMinorVersion)
  install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/libtypesafeConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/libtypesafeConfigVersion.cmake
    DESTINATION ${_config_dir})
  install(FILES LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/libtypesafe)
endif()

# --- Tests --------------------------------------------------------------------

if(LIBTYPESAFE_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
