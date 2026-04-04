# main/CMakeLists.txt - Cadence Engine executable

add_executable(cadence-engine
    main.c
    # BL-88: Kernel accessor API compiled directly into the exe (not the kernel
    # static lib) so that CD_API symbols are exported only from the exe, not
    # from plugin DLLs that also link cadence-kernel.
    ${CADENCE_KERNEL_DIR}/kernel/src/cd_kernel_api.c
)

target_include_directories(cadence-engine
    PRIVATE
        ${_KERNEL_INCLUDE_DIR}
)

# BL-83: cadence-mcp links static core libraries; no plugin DLL links needed.
target_link_libraries(cadence-engine
    PRIVATE
        cadence-kernel
        cadence-mcp
        ${CMAKE_DL_LIBS}
)
# glad for gladLoadGLLoader in headed mode (editor needs GL function pointers)
if(TARGET glad)
    target_link_libraries(cadence-engine PRIVATE glad)
endif()
if(UNIX)
    target_link_libraries(cadence-engine PRIVATE pthread)
endif()

# Link editor plugin for headed mode (ImGui overlay)
if(CD_BUILD_EDITOR AND NOT CD_BUILD_HEADLESS AND TARGET cadence-editor)
    target_link_libraries(cadence-engine PRIVATE cadence-editor)
endif()

# Increase stack size to 8MB (renderer has large stack allocations)
if(MSVC)
    target_link_options(cadence-engine PRIVATE /STACK:8388608)
endif()

# Set C11 standard for main executable
# ENABLE_EXPORTS: produce an import library (.lib on Windows) so plugin DLLs
# can link against symbols exported from the exe via CD_API (BL-88).
set_target_properties(cadence-engine PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF
    ENABLE_EXPORTS ON
)

# CD_BUILDING_ENGINE: tells CD_API macro to use dllexport (not dllimport)
target_compile_definitions(cadence-engine PRIVATE CD_BUILDING_ENGINE)
