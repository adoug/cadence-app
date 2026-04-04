# mcp/CMakeLists.txt - Cadence Engine MCP server static library

add_library(cadence-mcp STATIC
    src/cd_mcp_server.c
    src/cd_mcp_system_tools.c
    src/cd_mcp_scene_tools.c
    src/cd_mcp_node_tools.c
    src/cd_mcp_prop_tools.c
    src/cd_mcp_tag_tools.c
    src/cd_mcp_txn_tools.c
    src/cd_mcp_viewport_tools.c
    src/cd_mcp_asset_tools.c
    src/cd_mcp_script_tools.c
    src/cd_mcp_observe_tools.c
    src/cd_mcp_input_tools.c
    src/cd_mcp_play_tools.c
    src/cd_mcp_scene_save_tools.c
    src/cd_mcp_file_tools.c
    src/cd_mcp_prefab_tools.c
    src/cd_mcp_gamespec_tools.c
    src/cd_mcp_compose_tools.c
    src/cd_mcp_gamespec_plan_tools.c
    src/cd_mcp_build_tools.c
    src/cd_pak_writer.c
    src/cd_mcp_project_tools.c
    src/cd_mcp_asset_gen_tools.c
    src/cd_mcp_tcp.c
    src/cd_mcp_ws.c
    src/cd_mcp_socket.c
    src/cd_mcp_agent.c
    src/cd_mcp_log.c
    src/cd_mcp_log_tools.c
    src/cd_mcp_net_tools.c
    src/cd_mcp_ui_tools.c
    src/cd_mcp_sprite_anim_tools.c
    src/cd_mcp_anim_tools.c
    src/cd_mcp_camera_tools.c
    src/cd_mcp_debug_tools.c
    src/cd_mcp_terrain_tools.c
    src/cd_mcp_lock_tools.c
    src/cd_mcp_ai_tools.c
    src/cd_mcp_asset_pipeline_tools.c
    src/cd_mcp_audio_bus_tools.c
    src/cd_mcp_profiler_tools.c
    src/cd_mcp_scene_stream_tools.c
    src/cd_mcp_edit_tools.c
    src/cd_mcp_nav_tools.c
    src/cd_mcp_dialogue_tools.c
    src/cd_mcp_inventory_tools.c
    src/cd_mcp_mesh_tools.c
    src/cd_mcp_scene_validate_tools.c
    src/cd_mcp_material_tools.c
    src/cd_mcp_cook_tools.c
    src/cd_mcp_usd_tools.c
    src/cd_mcp_tool_registry.c
    src/cd_mcp_tool_state.c
    src/cd_mcp_fuzzy_match.c
    src/cd_mcp_error.c
    src/cd_mcp_async.c
)

target_include_directories(cadence-mcp
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE
        ${_KERNEL_INCLUDE_DIR}
)

# BL-83: Link static core libraries instead of plugin SHARED DLLs.
# cadence-mcp never links plugin DLLs — all symbols come from static libs.
# Plugin-provided libraries are linked conditionally (only present when
# plugins are cloned into the tree).
target_link_libraries(cadence-mcp
    PUBLIC
        cadence-kernel
        cadence-anim-core
        cadence-particles-core
        cadence-2d-core
        cadence-renderer-core
        cadence-physics-core
        cjson
        stb
    PRIVATE
        ${CD_LUA_TARGET}
        cadence-input-common
)

# Plugin-provided libraries — conditional on plugin availability
foreach(_mcp_dep cadence-ui-core cadence-net-core cadence-scene-io-core
                 cadence-script-core cadence-composer material_asset
                 scene_streaming)
    if(TARGET ${_mcp_dep})
        target_link_libraries(cadence-mcp PRIVATE ${_mcp_dep})
    endif()
endforeach()

# Link Winsock on Windows (needed by cd_mcp_tcp.c)
if(WIN32)
    target_link_libraries(cadence-mcp PRIVATE ws2_32)
endif()

# Apply strict compiler warnings to MCP
if(EXISTS "${PROJECT_SOURCE_DIR}/cmake/CompilerWarnings.cmake")
    include("${PROJECT_SOURCE_DIR}/cmake/CompilerWarnings.cmake")
endif()

# Set C11 standard for MCP
set_target_properties(cadence-mcp PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF
)
