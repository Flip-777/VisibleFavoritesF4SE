cmake_minimum_required(VERSION 3.20)

# ---- Options ----

option(COPY_BUILD "Copy the build output to the Fallout 4 directory." OFF)

# ---- Cache build vars ----

macro(set_from_environment VARIABLE)
  if (NOT DEFINED ${VARIABLE} AND DEFINED ENV{${VARIABLE}})
    set(${VARIABLE} $ENV{${VARIABLE}})
  endif ()
endmacro()

set_from_environment(Fallout4Path)
set_from_environment(VCPKG_INSTALLATION_ROOT)

if (DEFINED VCPKG_INSTALLATION_ROOT)
  set(CMAKE_TOOLCHAIN_FILE "${VCPKG_INSTALLATION_ROOT}/scripts/buildsystems/vcpkg.cmake" CACHE STRING "")
  set(VCPKG_TARGET_TRIPLET "x64-windows-static-md" CACHE STRING "")
else ()
  message(
    WARNING
    "Variable VCPKG_INSTALLATION_ROOT is not set. Continuing without vcpkg."
  )
endif ()

# overlay triplet anchored to the source tree so a reconfigure from the build dir keeps the toolset
if(NOT DEFINED VCPKG_OVERLAY_TRIPLETS)
  set(VCPKG_OVERLAY_TRIPLETS "${CMAKE_CURRENT_LIST_DIR}/cmake/triplets")
endif()
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL" CACHE STRING "")

# ---- Project ----

project(
  VisibleFavorites
  VERSION 1.2.3
  LANGUAGES CXX
)

configure_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/cmake/Version.h.in
  ${CMAKE_CURRENT_BINARY_DIR}/include/Version.h
  @ONLY
)

configure_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/cmake/version.rc.in
  ${CMAKE_CURRENT_BINARY_DIR}/version.rc
  @ONLY
)

# ---- Include guards ----

if (PROJECT_SOURCE_DIR STREQUAL PROJECT_BINARY_DIR)
  message(
    FATAL_ERROR
    "In-source builds not allowed. Please make a new directory (called a build directory) and run CMake from there."
  )
endif ()

# ---- Globals ----

if (MSVC)
  if (NOT ${CMAKE_GENERATOR} STREQUAL "Ninja")
    add_compile_options(
      /MP	# Build with Multiple Processes
    )
  endif ()
endif ()

set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_DEBUG OFF)

# ---- Dependencies ----

# Requires the Flip-777/CommonLibF4 fork, branch rel3gen.
set(CommonLibF4Path "${CMAKE_CURRENT_SOURCE_DIR}/../../CommonLibF4-NG/CommonLibF4" CACHE PATH "Path to CommonLibF4 - requires github.com/Flip-777/CommonLibF4 branch rel3gen (stock alandtse lacks 1.11.x support)")

if (NOT TARGET CommonLibF4)
  add_subdirectory("${CommonLibF4Path}" CommonLibF4)
endif ()

find_package(fmt REQUIRED CONFIG)
find_package(spdlog REQUIRED CONFIG)
find_package(imgui REQUIRED CONFIG)
find_package(minhook REQUIRED CONFIG)

# ---- Add source files ----

file(GLOB_RECURSE SOURCES
  src/*.cpp
  src/*.h
  include/*.h
)

source_group(
  TREE ${CMAKE_CURRENT_SOURCE_DIR}
  FILES ${SOURCES}
)

source_group(
  TREE ${CMAKE_CURRENT_BINARY_DIR}
  FILES ${CMAKE_CURRENT_BINARY_DIR}/include/Version.h
)

# ---- Create DLL ----

add_library(
  ${PROJECT_NAME}
  SHARED
  ${SOURCES}
  ${CMAKE_CURRENT_BINARY_DIR}/version.rc
  .clang-format
)

target_compile_definitions(
  ${PROJECT_NAME}
  PRIVATE
    _UNICODE
)

target_compile_features(
  ${PROJECT_NAME}
  PRIVATE
    cxx_std_23
)

target_include_directories(
  ${PROJECT_NAME}
  PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(
  ${PROJECT_NAME}
  PRIVATE
    CommonLibF4::CommonLibF4
    fmt::fmt
    imgui::imgui
    minhook::minhook
    d3d11
    spdlog::spdlog
)

target_precompile_headers(
  ${PROJECT_NAME}
  PRIVATE
    include/PCH.h
)

if (MSVC)
  target_compile_options(
    ${PROJECT_NAME}
    PRIVATE
      /sdl	# Enable Additional Security Checks
      /utf-8	# Set Source and Executable character sets to UTF-8
      /Zi	# Debug Information Format

      /permissive-	# Standards conformance
      /Zc:preprocessor	# Enable preprocessor conformance mode

      /W4	# Warning level
      /WX	# Warning level (warnings are errors)

      "$<$<CONFIG:DEBUG>:>"
      "$<$<CONFIG:RELEASE>:/Zc:inline;/JMC-;/Ob3>"
  )

  target_link_options(
    ${PROJECT_NAME}
    PRIVATE
      /WX	# Treat Linker Warnings as Errors

      "$<$<CONFIG:DEBUG>:/INCREMENTAL;/OPT:NOREF;/OPT:NOICF>"
      "$<$<CONFIG:RELEASE>:/INCREMENTAL:NO;/OPT:REF;/OPT:ICF;/DEBUG:FULL>"
  )
endif ()

# ---- File copying ----

if (COPY_BUILD)
  if (DEFINED Fallout4Path)
    add_custom_command(
      TARGET ${PROJECT_NAME}
      POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:${PROJECT_NAME}> ${Fallout4Path}/Data/F4SE/Plugins/
      COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_PDB_FILE:${PROJECT_NAME}> ${Fallout4Path}/Data/F4SE/Plugins/
    )
  else ()
    message(
      WARNING
      "Variable Fallout4Path is not defined. Skipping post-build copy command."
    )
  endif ()
endif ()

