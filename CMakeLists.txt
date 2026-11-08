cmake_minimum_required(VERSION 3.20)
project(FISO VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# Release binaries carry their C/C++ runtime so that they run without extra installs:
# static MSVC runtime on Windows, static libstdc++/libgcc on Linux.
if(MSVC)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  add_compile_options(/utf-8)
endif()
if(WIN32)
  add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
endif()

option(FISO_USE_SYSTEM_FLAC "Link against the system libFLAC instead of building it" OFF)
option(FISO_BUILD_SHARED "Build libfiso as a shared library" OFF)
option(FISO_BUILD_TESTS "Build the test suite" ON)

if(FISO_USE_SYSTEM_FLAC)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(FLAC REQUIRED IMPORTED_TARGET flac)
  add_library(fiso_flac INTERFACE)
  target_link_libraries(fiso_flac INTERFACE PkgConfig::FLAC)
else()
  # libFLAC (BSD-3-Clause) is built from source and linked statically, so the
  # `fiso` tool has no runtime dependency on Homebrew or system libraries.
  include(FetchContent)
  set(BUILD_CXXLIBS OFF CACHE BOOL "" FORCE)
  set(BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
  set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
  set(BUILD_DOCS OFF CACHE BOOL "" FORCE)
  set(INSTALL_MANPAGES OFF CACHE BOOL "" FORCE)
  set(INSTALL_CMAKE_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
  set(INSTALL_PKGCONFIG_MODULES OFF CACHE BOOL "" FORCE)
  set(WITH_OGG OFF CACHE BOOL "" FORCE)
  set(WITH_FORTIFY_SOURCE OFF CACHE BOOL "" FORCE)
  set(WITH_STACK_PROTECTOR OFF CACHE BOOL "" FORCE)
  set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
  # Only the libFLAC target is built (libFLAC's benchmarks and tools are skipped).
  set(FISO_FLAC_EXCLUDE "")
  if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.28)
    set(FISO_FLAC_EXCLUDE EXCLUDE_FROM_ALL)
  endif()
  FetchContent_Declare(flac
    GIT_REPOSITORY https://github.com/xiph/flac.git
    GIT_TAG 1.4.3
    GIT_SHALLOW TRUE
    ${FISO_FLAC_EXCLUDE})
  FetchContent_MakeAvailable(flac)
  add_library(fiso_flac INTERFACE)
  target_link_libraries(fiso_flac INTERFACE FLAC::FLAC)
endif()

add_subdirectory(libfiso)
add_subdirectory(fiso-cli)
if(APPLE)
  add_subdirectory(fiso-explorer/native/thumbnail)
endif()
if(FISO_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
