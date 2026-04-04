# CMakeLists.txt - Cadence App (dev engine + runtime)
#
# Builds cadence-engine (MCP + editor) and cadence-runtime (game player)
# against the Cadence kernel SDK + plugins.
#
# Two modes:
#   1. Source tree: set CADENCE_KERNEL_DIR to the cadence-kernel checkout
#      cmake -B build -DCADENCE_KERNEL_DIR=../cadence-kernel
#
#   2. Installed SDK: set CMAKE_PREFIX_PATH to the SDK install prefix
#      cmake -B build -DCMAKE_PREFIX_PATH=../cadence-sdk-v0.1.1-win64

cmake_minimum_required(VERSION 3.20)

project(cadence-app
    VERSION 0.1.1
    DESCRIPTION "Cadence Engine dev application"
    LANGUAGES C CXX
)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/plugins)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# Build options
option(CD_BUILD_EDITOR "Build editor UI plugin" ON)
option(CD_BUILD_HEADLESS "Build headless (no windowing)" OFF)

set(CADENCE_KERNEL_DIR "" CACHE PATH "Path to cadence-kernel source tree (dev mode)")

# ============================================================================
# Find the kernel — either from source tree or installed SDK
# ============================================================================

if(CADENCE_KERNEL_DIR AND EXISTS "${CADENCE_KERNEL_DIR}/CMakeLists.txt")
    message(STATUS "Kernel: source tree at ${CADENCE_KERNEL_DIR}")

    # Build kernel as a subdirectory — gives us all targets directly
    set(CD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(CD_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
    add_subdirectory(${CADENCE_KERNEL_DIR} ${CMAKE_BINARY_DIR}/kernel)

    # Kernel targets are available directly (cadence-kernel, cadence-mcp, etc.)
    set(_KERNEL_INCLUDE_DIR "${CADENCE_KERNEL_DIR}/kernel/include")
    set(_KERNEL_SOURCE_MODE ON)
else()
    message(STATUS "Kernel: looking for installed CadenceSDK...")
    find_package(CadenceSDK REQUIRED)
    set(_KERNEL_INCLUDE_DIR "${CadenceSDK_INCLUDE_DIRS}")
    set(_KERNEL_SOURCE_MODE OFF)
endif()

# ============================================================================
# Plugins — auto-discover from plugins/ directory
# ============================================================================

set(_CD_PLUGIN_DIRS
    renderer-null renderer-opengl input scripting-lua physics-jolt
    input-sim audio audio-null scene-io ui-runtime animation particles
    renderer-2d renderer-software networking asset-pipeline portal
    spatial scene-streaming material-asset demo-launcher renderer-lua
    nav ai dialogue
)

set(_CD_PLUGIN_COUNT 0)
foreach(_plug IN LISTS _CD_PLUGIN_DIRS)
    string(REPLACE "-" "_" _plug_dir "${_plug}")
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/plugins/${_plug_dir}/CMakeLists.txt")
        if(CD_BUILD_HEADLESS AND (_plug_dir STREQUAL "renderer_opengl" OR _plug_dir STREQUAL "input"))
            continue()
        endif()
        add_subdirectory(plugins/${_plug_dir})
        math(EXPR _CD_PLUGIN_COUNT "${_CD_PLUGIN_COUNT} + 1")
    endif()
endforeach()

# Editor plugin
if(CD_BUILD_EDITOR AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/plugins/editor/CMakeLists.txt")
    add_subdirectory(plugins/editor)
endif()

list(LENGTH _CD_PLUGIN_DIRS _CD_PLUGIN_TOTAL)
message(STATUS "Plugins found: ${_CD_PLUGIN_COUNT} of ${_CD_PLUGIN_TOTAL}")

# ============================================================================
# MCP library
# ============================================================================

add_subdirectory(mcp)

# ============================================================================
# cadence-engine (dev binary with MCP + editor)
# ============================================================================

if(_CD_PLUGIN_COUNT GREATER 0)
    add_subdirectory(main)
    message(STATUS "cadence-engine: ENABLED")
else()
    message(STATUS "cadence-engine: SKIPPED (no plugins — clone into plugins/)")
endif()

# ============================================================================
# cadence-runtime (stripped game-only binary)
# ============================================================================

add_subdirectory(runtime)

# ============================================================================
# Engine release: cmake --install build --component engine
# ============================================================================

if(TARGET cadence-engine)
    install(TARGETS cadence-engine
        RUNTIME DESTINATION bin
        COMPONENT engine
    )
endif()

if(TARGET cadence-runtime)
    install(TARGETS cadence-runtime
        RUNTIME DESTINATION bin
        COMPONENT engine
    )
endif()

# Plugin DLLs
install(
    DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>/"
    DESTINATION bin
    COMPONENT engine
    FILES_MATCHING
        PATTERN "*.dll"
        PATTERN "*.so"
        PATTERN "*.exe"
        PATTERN "*.pdb" EXCLUDE
)

# Shader files
install(
    DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>/shaders/"
    DESTINATION bin/shaders
    COMPONENT engine
    FILES_MATCHING
        PATTERN "*.vert"
        PATTERN "*.frag"
        PATTERN "*.comp"
        PATTERN "*.glsl"
)

# Print config
message(STATUS "Cadence App v${PROJECT_VERSION}")
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "Kernel mode: ${_KERNEL_SOURCE_MODE}")
