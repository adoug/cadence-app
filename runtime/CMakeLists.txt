# runtime/CMakeLists.txt - Cadence Runtime (stripped game-only binary)
#
# Task 11.1: cadence-runtime — no MCP server, no editor, no hot-reload.
# Links only cadence-kernel (not cadence-mcp).

add_executable(cadence-runtime
    main.c
    # BL-88: Kernel accessor API compiled directly into the runtime exe
    ${CADENCE_KERNEL_DIR}/kernel/src/cd_kernel_api.c
)

target_include_directories(cadence-runtime
    PRIVATE
        ${_KERNEL_INCLUDE_DIR}
)

# Runtime links ONLY the kernel — no MCP, no editor dependencies
target_link_libraries(cadence-runtime
    PRIVATE
        cadence-kernel
        ${CMAKE_DL_LIBS}
)
if(TARGET scene_streaming)
    target_link_libraries(cadence-runtime PRIVATE scene_streaming)
endif()
if(UNIX)
    target_link_libraries(cadence-runtime PRIVATE pthread)
endif()

# PA-1: Runtime no longer links GLFW directly -- window management is
# provided through the cd_window_api_t vtable from the renderer plugin.

# Define CD_RUNTIME to allow compile-time stripping of dev features
target_compile_definitions(cadence-runtime PRIVATE CD_RUNTIME=1)

# Set C11 standard
# ENABLE_EXPORTS: produce an import library so plugin DLLs can link against
# CD_API symbols exported from the runtime exe (BL-88).
set_target_properties(cadence-runtime PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF
    ENABLE_EXPORTS ON
)

# CD_BUILDING_ENGINE: tells CD_API macro to use dllexport (not dllimport)
target_compile_definitions(cadence-runtime PRIVATE CD_BUILDING_ENGINE)

# Install to bin directory
install(TARGETS cadence-runtime
    RUNTIME DESTINATION bin
)
