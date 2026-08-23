cmake_minimum_required(VERSION 3.16)
project(crossover-server LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# SLikeNet v0.1.3 is pinned deliberately: it is the exact version the mod's
# prebuilt SLikeNet_LibStatic_Release_Win32.lib was built from (RAKNET_VERSION
# 4.082, RAKNET_PROTOCOL_VERSION 6). A different protocol version on either side
# makes every client bounce with ID_INCOMPATIBLE_PROTOCOL_VERSION.
set(SLIKENET_TAG "v.0.1.3" CACHE STRING "SLikeNet git tag to build against")
set(SLIKENET_DIR "" CACHE PATH "Pre-cloned SLikeNet source tree (skips the download)")

set(SLIKENET_ENABLE_DLL                  OFF CACHE BOOL "" FORCE)
set(SLIKENET_ENABLE_STATIC               ON  CACHE BOOL "" FORCE)
set(SLIKENET_ENABLE_SAMPLES              OFF CACHE BOOL "" FORCE)
set(SLIKENET_ENABLE_DEPENDENT_EXTENSIONS OFF CACHE BOOL "" FORCE)

if(SLIKENET_DIR)
  add_subdirectory(${SLIKENET_DIR} ${CMAKE_BINARY_DIR}/slikenet EXCLUDE_FROM_ALL)
  set(SLIKENET_INCLUDE_DIR ${SLIKENET_DIR}/Source/include)
else()
  include(FetchContent)
  FetchContent_Declare(slikenet
    GIT_REPOSITORY https://github.com/SLikeSoft/SLikeNet.git
    GIT_TAG        ${SLIKENET_TAG}
    GIT_SHALLOW    TRUE)
  FetchContent_MakeAvailable(slikenet)
  set(SLIKENET_INCLUDE_DIR ${slikenet_SOURCE_DIR}/Source/include)
endif()

# The static-library target was renamed between the v.0.1.3 tag (RakNetLibStatic)
# and upstream HEAD (SLikeNetLibStatic). Accept either so a local checkout at
# either revision builds.
if(TARGET RakNetLibStatic)
  set(SLIKENET_TARGET RakNetLibStatic)
elseif(TARGET SLikeNetLibStatic)
  set(SLIKENET_TARGET SLikeNetLibStatic)
else()
  message(FATAL_ERROR "SLikeNet static library target not found in ${SLIKENET_DIR}")
endif()
message(STATUS "SLikeNet static target: ${SLIKENET_TARGET}")

find_package(Threads REQUIRED)

add_library(crossover-core STATIC
  src/Config.cpp
  src/RelayServer.cpp
  src/StatusServer.cpp
  src/Tunnel.cpp)
target_include_directories(crossover-core PUBLIC include src ${SLIKENET_INCLUDE_DIR})
target_link_libraries(crossover-core PUBLIC ${SLIKENET_TARGET} Threads::Threads)

add_executable(crossover-server src/main.cpp)
target_link_libraries(crossover-server PRIVATE crossover-core)

# Headless stand-in for Fable + the mod, used by test/run-integration-test.sh.
add_executable(crossover-testclient test/TestClient.cpp)
target_include_directories(crossover-testclient PRIVATE include ${SLIKENET_INCLUDE_DIR})
target_link_libraries(crossover-testclient PRIVATE ${SLIKENET_TARGET} Threads::Threads)

# Player-side UDP-over-TCP bridge. Also cross-compiled for Windows by
# tunnel-client/build-windows.sh; this target is the native build.
add_executable(crossover-tunnel tunnel-client/crossover-tunnel.c)

install(TARGETS crossover-server crossover-tunnel crossover-testclient
        RUNTIME DESTINATION bin)
