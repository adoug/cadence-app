/* cd_mcp_tools.h - Cadence Engine MCP tool registration
 *
 * Declares registration functions for each group of MCP tools.
 * Each group registers its handlers with a cd_mcp_server_t instance.
 */
#ifndef CD_MCP_TOOLS_H
#define CD_MCP_TOOLS_H

#include "cadence/cd_mcp.h"

/* ============================================================================
 * Tool module registration table
 *
 * Each tool module has a name and a registration function. The built-in
 * modules are defined in cd_mcp_tool_registry.c. Plugins can add their
 * own modules at runtime via cd_mcp_register_tool_module().
 * ============================================================================ */

/**
 * A tool module entry: maps a category name to its registration function.
 */
typedef struct cd_mcp_tool_module_t {
    const char* name;
    cd_result_t (*register_fn)(cd_mcp_server_t* server);
} cd_mcp_tool_module_t;

/**
 * Register all built-in tool modules with the MCP server.
 *
 * Iterates the static table of built-in modules and any dynamically
 * registered plugin modules, calling each registration function.
 *
 * @param server  Pointer to an initialized cd_mcp_server_t.
 * @return CD_OK on success, or the first error from a module.
 */
cd_result_t cd_mcp_register_all_tools(cd_mcp_server_t* server);

/**
 * Register an additional tool module (for plugin use).
 *
 * Plugins can call this before cd_mcp_register_all_tools() to add
 * their own tool categories. The module struct is copied.
 *
 * @param module  Pointer to a tool module descriptor (copied).
 * @return CD_OK on success, CD_ERR_NULL if NULL, CD_ERR_FULL if at capacity.
 */
cd_result_t cd_mcp_register_tool_module(const cd_mcp_tool_module_t* module);

/**
 * Get the total number of registered tool modules (built-in + dynamic).
 *
 * @return Number of tool modules.
 */
uint32_t cd_mcp_get_tool_module_count(void);

/* ============================================================================
 * System tools: system.ping, system.health, system.capabilities,
 *               system.plugin_status, system.plugin_restart
 * ============================================================================ */

/**
 * Register the system tool handlers with the MCP server.
 *
 * Registers:
 *   - system.ping           : Health check, returns uptime
 *   - system.health         : Detailed engine status
 *   - system.capabilities   : Engine capability discovery
 *   - system.plugin_status  : List all plugins with health state
 *   - system.plugin_restart : Unload and reload a plugin (crash recovery)
 *
 * @param server  Pointer to an initialized cd_mcp_server_t.
 * @return CD_OK on success, CD_ERR_NULL if server is NULL.
 */
cd_result_t cd_mcp_register_system_tools(cd_mcp_server_t* server);

/* ============================================================================
 * Scene tools: scene.create3d
 * ============================================================================ */

/**
 * Register the scene tool handlers with the MCP server.
 *
 * Registers:
 *   - scene.create3d : Create a new 3D scene with a root node
 *
 * @param server  Pointer to an initialized cd_mcp_server_t.
 * @return CD_OK on success, CD_ERR_NULL if server is NULL.
 */
cd_result_t cd_mcp_register_scene_tools(cd_mcp_server_t* server);

/* ============================================================================
 * Node tools: node.create, node.delete, node.get, node.find,
 *             node.setTransform, node.setParent
 * ============================================================================ */

/**
 * Register the node tool handlers with the MCP server.
 *
 * Registers:
 *   - node.create       : Create a new node in the scene
 *   - node.delete       : Delete a node and its children
 *   - node.get          : Get full node properties
 *   - node.find         : Find nodes by name glob and/or tags
 *   - node.setTransform : Set local/world transform with partial updates
 *   - node.setParent    : Reparent a node under a new parent
 *
 * @param server  Pointer to an initialized cd_mcp_server_t.
 * @return CD_OK on success, CD_ERR_NULL if server is NULL.
 */
cd_result_t cd_mcp_register_node_tools(cd_mcp_server_t* server);

/* ============================================================================
 * Property tools: prop.set, prop.get, prop.batchSet
 * ============================================================================ */

/**
 * Register the property tool handlers with the MCP server.
 *
 * Registers:
 *   - prop.set      : Set a component property by dot-path
 *   - prop.get      : Get a component property by dot-path
 *   - prop.batchSet : Set multiple properties atomically
 *
 * @param server  Pointer to an initialized cd_mcp_server_t.
 * @return CD_OK on success, CD_ERR_NULL if server is NULL.
 */
cd_result_t cd_mcp_register_prop_tools(cd_mcp_server_t* server);

/* ============================================================================
 * Tag tools: tag.add, tag.remove
 * ============================================================================ */

/**
 * Register the tag tool handlers with the MCP server.
 *
 * Registers:
 *   - tag.add    : Add one or more tags to a node
 *   - tag.remove : Remove one or more tags from a node
 *
 * @param server  Pointer to an initialized cd_mcp_server_t.
 * @return CD_OK on success, CD_ERR_NULL if server is NULL.
 */
cd_result_t cd_mcp_register_tag_tools(cd_mcp_server_t* server);

/* ============================================================================
 * Transaction tools: txn.begin, txn.commit, txn.rollback
 * ============================================================================ */

/**
 * Register the transaction tool handlers with the MCP server.
 *
 * Registers:
 *   - txn.begin    : Begin a new transaction
 *   - txn.commit   : Commit (finalize) an active transaction
 *   - txn.rollback : Roll back an active transaction
 *
 * @param server  Pointer to an initialized cd_mcp_server_t.
 * @return CD_OK on success, CD_ERR_NULL if server is NULL.
 */
cd_result_t cd_mcp_register_txn_tools(cd_mcp_server_t* server);

/**
 * Reset the global MCP transaction state.
 *
 * Call between tests to clear the active transaction.
 * Not intended for production use.
 */
void cd_mcp_txn_reset_state(void);

/* ============================================================================
 * Viewport tools: viewport.capture, viewport.setCamera
 * ============================================================================ */

/**
 * Register the viewport tool handlers with the MCP server.
 *
 * Registers:
 *   - viewport.capture   : Capture the current viewport as a PNG image (base64)
 *   - viewport.setCamera : Set the active camera node for rendering
 *
 * @param server  Pointer to an initialized cd_mcp_server_t.
 * @return CD_OK on success, CD_ERR_NULL if server is NULL.
 */
cd_result_t cd_mcp_register_viewport_tools(cd_mcp_server_t* server);

/* ============================================================================
 * Asset tools: asset.import, asset.list
 * ============================================================================ */

/**
 * Register the asset tool handlers with the MCP server.
 *
 * Registers:
 *   - asset.import : Import a file into the project and register in asset DB
 *   - asset.list   : List registered assets with optional kind/prefix filter
 *
 * @param server  Pointer to an initialized cd_mcp_server_t.
 * @return CD_OK on success, CD_ERR_NULL if server is NULL.
 */
cd_result_t cd_mcp_register_asset_tools(cd_mcp_server_t* server);

/* ============================================================================
 * Script tools: script.create, script.read, script.attach, script.detach
 * ============================================================================ */

/**
 * Register the script tool handlers with the MCP server.
 *
 * Registers:
 *   - script.create : Create a script file from content string
 *   - script.read   : Read a script file back
 *   - script.attach : Attach a Lua script to a scene node
 *   - script.detach : Detach a script from a node
 *
 * @param server  Pointer to an initialized cd_mcp_server_t.
 * @return CD_OK on success, CD_ERR_NULL if server is NULL.
 */
cd_result_t cd_mcp_register_script_tools(cd_mcp_server_t* server);

/**
 * Set the script manager for MCP script tools.
 *
 * Must be called before script.attach/script.detach can be used.
 * Typically called during plugin initialization or test setup.
 *
 * The pointer is stored as void* to avoid a hard dependency on the
 * scripting_lua plugin headers.  Internally cast to cd_script_mgr_t*.
 *
 * @param mgr  Pointer to the script manager (not owned).
 */
void cd_mcp_script_tools_set_mgr(void* mgr);

/**
 * Set the reload watcher for MCP script tools.
 *
 * If set, attached scripts will be tracked for hot-reload.
 *
 * The pointer is stored as void* to avoid a hard dependency on the
 * scripting_lua plugin headers.  Internally cast to cd_script_reload_t*.
 *
 * @param reload  Pointer to the reload watcher (not owned).
 */
void cd_mcp_script_tools_set_reload(void* reload);

/**
 * Get the current script manager pointer.
 *
 * @return Pointer to the script manager (as void*), or NULL if not set.
 */
void* cd_mcp_script_tools_get_mgr(void);

/* ============================================================================
 * Observe tools: observe.state, observe.perf
 * ============================================================================ */

/**
 * Register the observe tool handlers with the MCP server.
 *
 * Registers:
 *   - observe.state : Read runtime node state (Lua self.* fields, future
 *                     component properties) for one or more nodes.
 *   - observe.perf  : Return engine performance counters (stub for Task 8.4).
 *
 * @param server  Pointer to an initialized cd_mcp_server_t.
 * @return CD_OK on success, CD_ERR_NULL if server is NULL.
 */
cd_result_t cd_mcp_register_observe_tools(cd_mcp_server_t* server);

/**
 * Set the script manager used by the observe tools.
 *
 * Must be called before observe.state can return Lua self.* values.
 * Accepts a void* to avoid a hard compile-time dependency on the
 * scripting_lua plugin headers.  Internally cast to cd_script_mgr_t*.
 *
 * @param mgr  Pointer to the script manager (not owned), or NULL to clear.
 */
void cd_mcp_observe_tools_set_mgr(void* mgr);

/**
 * Get the current script manager pointer used by the observe tools.
 *
 * @return Pointer to the script manager (as void*), or NULL if not set.
 */
void* cd_mcp_observe_tools_get_mgr(void);

/* ============================================================================
 * Input tools: input.simulate, input.action
 * ============================================================================ */

/**
 * Register the input tool handlers with the MCP server.
 *
 * Registers:
 *   - input.simulate : Inject raw input events for headless play-testing
 *   - input.action   : Trigger a high-level input action by name
 *
 * Requires the input_sim plugin to be loaded (Task 7.4).
 *
 * @param server  Pointer to an initialized cd_mcp_server_t.
 * @return CD_OK on success, CD_ERR_NULL if server is NULL.
 */
cd_result_t cd_mcp_register_input_tools(cd_mcp_server_t* server);

/* ============================================================================
 * Play tools: play.start, play.stop, play.pause, play.resume, play.step
 * ============================================================================ */

/**
 * Register the play mode tool handlers with the MCP server.
 *
 * Registers:
 *   - play.start  : Enter play mode (snapshot scene)
 *   - play.stop   : Stop play and restore scene
 *   - play.pause  : Pause play
 *   - play.resume : Resume from pause
 *   - play.step   : Advance N fixed-update frames while paused
 *
 * @param server  Pointer to an initialized cd_mcp_server_t.
 * @return CD_OK on success, CD_ERR_NULL if server is NULL.
 */
cd_result_t cd_mcp_register_play_tools(cd_mcp_server_t* server);

/* ============================================================================
 * Scene save/open tools: scene.save, scene.open
 * ============================================================================ */

/**
 * Register the scene save/open tool handlers with the MCP server.
 *
 * Registers:
 *   - scene.save : Save the active scene to a TOML file
 *   - scene.open : Load a scene from a TOML file (replacing active scene)
 *
 * @param server  Pointer to an initialized cd_mcp_server_t.
 * @return CD_OK on success, CD_ERR_NULL if server is NULL.
 */
cd_result_t cd_mcp_register_scene_save_tools(cd_mcp_server_t* server);

/**
 * Reset the global MCP play tools state.
 *
 * Call between tests to clear the session counter and active session.
 * Not intended for production use.
 */
void cd_mcp_play_tools_reset_state(void);

/* ============================================================================
 * File tools: file.read, file.write, file.list, file.delete
 * ============================================================================ */

/**
 * Register the file tool handlers with the MCP server.
 *
 * Registers:
 *   - file.read   : Read a file by res:// URI
 *   - file.write  : Write content to a file (creates dirs as needed)
 *   - file.list   : List files/directories with optional glob filter
 *   - file.delete : Delete a file by res:// URI
 *
 * All paths are sandboxed to the project directory (cd_kernel_get_config(kernel)->project_path).
 * Path traversal with ".." that escapes the project root is rejected.
 *
 * @param server  Pointer to an initialized cd_mcp_server_t.
 * @return CD_OK on success, CD_ERR_NULL if server is NULL.
 */
cd_result_t cd_mcp_register_file_tools(cd_mcp_server_t* server);

/* ============================================================================
 * Prefab tools: prefab.create, prefab.instantiate
 * ============================================================================ */

/**
 * Register the prefab tool handlers with the MCP server.
 *
 * Registers:
 *   - prefab.create      : Save a node subtree as a .prefab.toml file
 *   - prefab.instantiate : Instantiate a prefab from file under a parent node
 *
 * @param server  Pointer to an initialized cd_mcp_server_t.
 * @return CD_OK on success, CD_ERR_NULL if server is NULL.
 */
cd_result_t cd_mcp_register_prefab_tools(cd_mcp_server_t* server);

/* ============================================================================
 * Gamespec tools: gamespec.validate
 * ============================================================================ */

/**
 * Register the gamespec tool handlers with the MCP server.
 *
 * Registers:
 *   - gamespec.validate : Validate a .gamespec.toml, reporting errors/warnings
 *
 * @param server  Pointer to an initialized cd_mcp_server_t.
 * @return CD_OK on success, CD_ERR_NULL if server is NULL.
 */
cd_result_t cd_mcp_register_gamespec_tools(cd_mcp_server_t* server);

/* ============================================================================
 * Gamespec plan/diff tools: gamespec.plan, gamespec.diff
 * ============================================================================ */

/**
 * Register the gamespec plan/diff tool handlers with the MCP server.
 *
 * Registers:
 *   - gamespec.plan : Show what files would be generated without writing
 *   - gamespec.diff : Show what changed between spec and on-disk files
 *
 * @param server  Pointer to an initialized cd_mcp_server_t.
 * @return CD_OK on success, CD_ERR_NULL if server is NULL.
 */
cd_result_t cd_mcp_register_gamespec_plan_tools(cd_mcp_server_t* server);

/* ============================================================================
 * Compose tools: gamespec.compose
 * ============================================================================ */

/**
 * Register the compose tool handlers with the MCP server.
 *
 * Registers:
 *   - gamespec.compose : Generate game code from a Game Spec using all 9
 *                        composer generators. Supports full and incremental modes.
 *
 * @param server  Pointer to an initialized cd_mcp_server_t.
 * @return CD_OK on success, CD_ERR_NULL if server is NULL.
 */
cd_result_t cd_mcp_register_compose_tools(cd_mcp_server_t* server);

/* ============================================================================
 * Build tools: build.config, build.export
 * ============================================================================ */

/**
 * Register the build tool handlers with the MCP server.
 *
 * Registers:
 *   - build.config : Return project build configuration
 *   - build.export : Pack project assets into PAK files
 *
 * @param server  Pointer to an initialized cd_mcp_server_t.
 * @return CD_OK on success, CD_ERR_NULL if server is NULL.
 */
cd_result_t cd_mcp_register_build_tools(cd_mcp_server_t* server);

/* ============================================================================
 * Project tools: project.create, project.open, project.info
 * ============================================================================ */

/**
 * Register the project tool handlers with the MCP server.
 *
 * Registers:
 *   - project.create : Create a new project directory with scaffolding
 *   - project.open   : Set the kernel's active project path
 *   - project.info   : Return info about the current project
 *
 * @param server  Pointer to an initialized cd_mcp_server_t.
 * @return CD_OK on success, CD_ERR_NULL if server is NULL.
 */
cd_result_t cd_mcp_register_project_tools(cd_mcp_server_t* server);

/**
 * Register procedural asset generation tools:
 *   - asset_gen.listTemplates  : List available templates and their parameters
 *   - asset_gen.generate       : Generate a single procedural mesh
 *   - asset_gen.batchGenerate  : Generate multiple procedural meshes
 *
 * @param server  Pointer to an initialized cd_mcp_server_t.
 * @return CD_OK on success, CD_ERR_NULL if server is NULL.
 */
cd_result_t cd_mcp_register_asset_gen_tools(cd_mcp_server_t* server);

/* ============================================================================
 * Log tools: log.stream, log.query (Task 19.3)
 * ============================================================================ */

/**
 * Register the log tool handlers with the MCP server.
 *
 * Registers:
 *   - log.stream : Poll for new log entries since last call (per-agent cursor)
 *   - log.query  : Query log entries with time/level/module filters
 *
 * @param server  Pointer to an initialized cd_mcp_server_t.
 * @return CD_OK on success, CD_ERR_NULL if server is NULL.
 */
cd_result_t cd_mcp_register_log_tools(cd_mcp_server_t* server);

/**
 * Reset log tools state (stream cursors).
 * Call between tests.
 */
void cd_mcp_log_tools_reset(void);

/* ============================================================================
 * Net tools: net.host, net.connect, net.disconnect, net.status
 * ============================================================================ */

/**
 * Register the networking tool handlers with the MCP server.
 *
 * Registers:
 *   - net.host       : Start hosting a multiplayer session
 *   - net.connect    : Connect to a hosted session
 *   - net.disconnect : Disconnect from current session
 *   - net.status     : Query connection state and peer list
 *
 * @param server  Pointer to an initialized cd_mcp_server_t.
 * @return CD_OK on success, CD_ERR_NULL if server is NULL.
 */
cd_result_t cd_mcp_register_net_tools(cd_mcp_server_t* server);

/* ============================================================================
 * UI tools: ui.create_screen, ui.create_element, ui.update_element,
 *           ui.remove_element, ui.query, ui.show_screen, ui.hide_screen
 * ============================================================================ */

/**
 * Register the UI tool handlers with the MCP server.
 *
 * Registers:
 *   - ui.create_screen  : Create a named UI screen
 *   - ui.create_element : Create a UI element (label/button/panel/progress)
 *   - ui.update_element : Modify element properties
 *   - ui.remove_element : Remove an element
 *   - ui.query          : Get all screens and elements with their state
 *   - ui.show_screen    : Show a screen
 *   - ui.hide_screen    : Hide a screen
 *
 * @param server  Pointer to an initialized cd_mcp_server_t.
 * @return CD_OK on success, CD_ERR_NULL if server is NULL.
 */
cd_result_t cd_mcp_register_ui_tools(cd_mcp_server_t* server);

/* ============================================================================
 * Sprite animation tools: sprite_anim.setup, sprite_anim.add_clip,
 *                         sprite_anim.play, sprite_anim.query (S1.5)
 * ============================================================================ */

/**
 * Register the sprite animation tool handlers with the MCP server.
 *
 * Registers:
 *   - sprite_anim.setup    : Set frames grid on a node
 *   - sprite_anim.add_clip : Add animation clip
 *   - sprite_anim.play     : Play a clip
 *   - sprite_anim.query    : Get current animation state
 *
 * @param server  Pointer to an initialized cd_mcp_server_t.
 * @return CD_OK on success, CD_ERR_NULL if server is NULL.
 */
cd_result_t cd_mcp_register_sprite_anim_tools(cd_mcp_server_t* server);

/* ============================================================================
 * Skeletal animation tools: anim.play, anim.blend_to, anim.stop,
 *                           anim.query, anim.add_state, anim.add_transition
 * ============================================================================ */

/**
 * Register the skeletal animation tool handlers with the MCP server.
 *
 * Registers:
 *   - anim.play           : Play a skeletal animation clip
 *   - anim.blend_to       : Crossfade to a new clip
 *   - anim.stop           : Stop animation playback
 *   - anim.query          : Get current animation state
 *   - anim.add_state      : Add state machine state
 *   - anim.add_transition : Add state machine transition
 *
 * @param server  Pointer to an initialized cd_mcp_server_t.
 * @return CD_OK on success, CD_ERR_NULL if server is NULL.
 */
cd_result_t cd_mcp_register_anim_tools(cd_mcp_server_t* server);

/* ============================================================================
 * Camera tools: camera.set_mode, camera.configure, camera.shake, camera.query
 * ============================================================================ */

/**
 * Register the camera tool handlers with the MCP server.
 *
 * Registers:
 *   - camera.set_mode  : Set camera mode and optional target
 *   - camera.configure : Set mode-specific parameters
 *   - camera.shake     : Trigger shake effect
 *   - camera.query     : Get current camera state
 *
 * @param server  Pointer to an initialized cd_mcp_server_t.
 * @return CD_OK on success, CD_ERR_NULL if server is NULL.
 */
cd_result_t cd_mcp_register_camera_tools(cd_mcp_server_t* server);

/* ============================================================================
 * Lock tools: node.lock, node.unlock (P1-6)
 * ============================================================================ */

/**
 * Register the node lock tool handlers with the MCP server.
 *
 * Registers:
 *   - node.lock   : Explicitly lock a node for a transaction
 *   - node.unlock : Explicitly unlock a node
 *
 * @param server  Pointer to an initialized cd_mcp_server_t.
 * @return CD_OK on success, CD_ERR_NULL if server is NULL.
 */
cd_result_t cd_mcp_register_lock_tools(cd_mcp_server_t* server);

/* ============================================================================
 * Debug tools: debug.setBreakpoint, debug.removeBreakpoint,
 *              debug.listBreakpoints, debug.continue, debug.step,
 *              debug.stepOver, debug.stepOut, debug.eval,
 *              debug.stackTrace, debug.locals
 * ============================================================================ */

/**
 * Register the Lua debug tool handlers with the MCP server.
 *
 * Registers:
 *   - debug.setBreakpoint    : Set a breakpoint at file:line
 *   - debug.removeBreakpoint : Remove a breakpoint by ID
 *   - debug.listBreakpoints  : List all active breakpoints
 *   - debug.continue         : Resume execution from pause
 *   - debug.step             : Step into (next line, any depth)
 *   - debug.stepOver         : Step over (next line, same depth)
 *   - debug.stepOut          : Step out (return from current function)
 *   - debug.eval             : Evaluate expression in paused context
 *   - debug.stackTrace       : Get stack trace at pause point
 *   - debug.locals           : Get local variables at current frame
 *
 * Requires the scripting_lua plugin to be loaded and cd_lua_debug_init()
 * to have been called (which sets the global debug singleton).
 *
 * @param server  Pointer to an initialized cd_mcp_server_t.
 * @return CD_OK on success, CD_ERR_NULL if server is NULL.
 */
cd_result_t cd_mcp_register_debug_tools(cd_mcp_server_t* server);

/* ============================================================================
 * Terrain Tools
 *
 * Implements:
 *   - terrain.create        : Create terrain with dimensions
 *   - terrain.loadHeightmap : Load heightmap file
 *   - terrain.paint         : Brush-based height modification
 *   - terrain.setSplatTexture : Set layer texture
 *   - terrain.getHeight     : Query height at position
 *
 * Requires the renderer_opengl plugin to be loaded.
 *
 * @param server  Pointer to an initialized cd_mcp_server_t.
 * @return CD_OK on success, CD_ERR_NULL if server is NULL.
 */
cd_result_t cd_mcp_register_terrain_tools(cd_mcp_server_t* server);

/* ============================================================================
 * AI Tools: ai.listAgents, ai.getAgentState, ai.setAgentState,
 *           ai.getBlackboard, ai.setBlackboard, ai.getSensoryMemory,
 *           ai.emitSound
 * ============================================================================ */

/**
 * Register the AI tool handlers with the MCP server.
 *
 * Registers:
 *   - ai.listAgents      : List all AI agents with summary
 *   - ai.getAgentState   : Get detailed agent state (goals, BB, memory)
 *   - ai.setAgentState   : Force agent state for testing
 *   - ai.getBlackboard   : Get all BB key-value pairs
 *   - ai.setBlackboard   : Set a BB entry
 *   - ai.getSensoryMemory: Get agent's sensory memory records
 *   - ai.emitSound       : Inject a sound event
 *
 * @param server  Pointer to an initialized cd_mcp_server_t.
 * @return CD_OK on success, CD_ERR_NULL if server is NULL.
 */
cd_result_t cd_mcp_register_ai_tools(cd_mcp_server_t* server);

/**
 * Set the agent system function pointers (called by scripting_lua at init).
 */
void cd_mcp_ai_tools_set_agent_fns(void* get_count, void* get_agent, void* get_all);

/**
 * Set the blackboard pool pointer (called by scripting_lua at init).
 */
void cd_mcp_ai_tools_set_bb_pool(void* pool);

/**
 * Set the sound emitter function (called by scripting_lua at init).
 */
void cd_mcp_ai_tools_set_sound_fn(void* fn);

/* ============================================================================
 * Asset Pipeline Tools: asset.compress_texture, asset.optimize_mesh,
 *                       asset.pipeline.info (Sprint 7.3)
 * ============================================================================ */

/**
 * Register the asset pipeline tool handlers with the MCP server.
 *
 * Registers:
 *   - asset.compress_texture : Compress a texture to BC1/BC3 DDS format
 *   - asset.optimize_mesh   : Optimize mesh vertex cache ordering
 *   - asset.pipeline.info   : Report pipeline capabilities
 *
 * @param server  Pointer to an initialized cd_mcp_server_t.
 * @return CD_OK on success, CD_ERR_NULL if server is NULL.
 */
cd_result_t cd_mcp_register_asset_pipeline_tools(cd_mcp_server_t* server);

/* ============================================================================
 * Material Tools: material.create, material.get, material.set, material.list
 * ============================================================================ */

/**
 * Register the material tool handlers with the MCP server.
 *
 * Registers:
 *   - material.create : Create a new .cdmat material file
 *   - material.get    : Read material properties from a .cdmat file
 *   - material.set    : Modify material properties in a .cdmat file
 *   - material.list   : List all .cdmat materials in the project
 *
 * @param server  Pointer to an initialized cd_mcp_server_t.
 * @return CD_OK on success, CD_ERR_NULL if server is NULL.
 */
cd_result_t cd_mcp_register_material_tools(cd_mcp_server_t* server);

/* ============================================================================
 * Cook Tools: asset.cook, asset.cook_all, asset.cook_status,
 *             asset.import_settings.get, asset.import_settings.set
 * ============================================================================ */

/**
 * Register the asset cooking tool handlers with the MCP server.
 *
 * Registers:
 *   - asset.cook              : Cook a single asset
 *   - asset.cook_all          : Cook all project assets
 *   - asset.cook_status       : Get stale/fresh counts
 *   - asset.import_settings.get : Read .import sidecar
 *   - asset.import_settings.set : Write .import sidecar
 *
 * @param server  Pointer to an initialized cd_mcp_server_t.
 * @return CD_OK on success, CD_ERR_NULL if server is NULL.
 */
cd_result_t cd_mcp_register_cook_tools(cd_mcp_server_t* server);

/* ============================================================================
 * USD/MaterialX Import Tools: asset.import_usd, asset.import_mtlx
 * ============================================================================ */

/**
 * Register the USD and MaterialX import tool handlers with the MCP server.
 *
 * Registers:
 *   - asset.import_usd  : Import an OpenUSD scene
 *   - asset.import_mtlx : Import MaterialX materials
 *
 * @param server  Pointer to an initialized cd_mcp_server_t.
 * @return CD_OK on success, CD_ERR_NULL if server is NULL.
 */
cd_result_t cd_mcp_register_usd_tools(cd_mcp_server_t* server);

/* ============================================================================
 * Audio Bus Tools: audio.bus.list, audio.bus.set_volume,
 *                  audio.bus.mute, audio.bus.solo (BL-8)
 * ============================================================================ */

/**
 * Register the audio bus tool handlers with the MCP server.
 *
 * Registers:
 *   - audio.bus.list       : List all buses with current volumes/states
 *   - audio.bus.set_volume : Set bus volume by name
 *   - audio.bus.mute       : Mute/unmute a bus
 *   - audio.bus.solo       : Solo/unsolo a bus
 *
 * @param server  Pointer to an initialized cd_mcp_server_t.
 * @return CD_OK on success, CD_ERR_NULL if server is NULL.
 */
cd_result_t cd_mcp_register_audio_bus_tools(cd_mcp_server_t* server);

/* ============================================================================
 * Profiler tools: perf.profile_frame, perf.memory_report,
 *                 perf.profiler_enable (Sprint 8.3)
 * ============================================================================ */

/**
 * Register the profiler tool handlers with the MCP server.
 *
 * Registers:
 *   - perf.profile_frame   : Hierarchical timing data for the last frame
 *   - perf.memory_report   : Memory usage across all subsystems
 *   - perf.profiler_enable : Enable/disable the CPU profiler
 *
 * @param server  Pointer to an initialized cd_mcp_server_t.
 * @return CD_OK on success, CD_ERR_NULL if server is NULL.
 */
cd_result_t cd_mcp_register_profiler_tools(cd_mcp_server_t* server);

/* ============================================================================
 * Edit tools: edit.undo, edit.redo, edit.history (S2.2)
 * ============================================================================ */

/**
 * Register the edit (undo/redo/history) tool handlers with the MCP server.
 *
 * Registers:
 *   - edit.undo    : Undo the last command group(s)
 *   - edit.redo    : Redo the last undone command group(s)
 *   - edit.history : List recent undo stack entries
 *
 * @param server  Pointer to an initialized cd_mcp_server_t.
 * @return CD_OK on success, CD_ERR_NULL if server is NULL.
 */
cd_result_t cd_mcp_register_edit_tools(cd_mcp_server_t* server);

/* ============================================================================
 * Scene streaming tools: scene.stream.add_chunk, scene.stream.status,
 *                        scene.stream.load, scene.stream.unload (Sprint 8.4)
 * ============================================================================ */

/**
 * Register the scene streaming tool handlers with the MCP server.
 *
 * Registers:
 *   - scene.stream.add_chunk : Add a streaming chunk definition
 *   - scene.stream.status    : Report all chunks and their load status
 *   - scene.stream.load      : Force-load a specific chunk
 *   - scene.stream.unload    : Force-unload a specific chunk
 *
 * @param server  Pointer to an initialized cd_mcp_server_t.
 * @return CD_OK on success, CD_ERR_NULL if server is NULL.
 */
cd_result_t cd_mcp_register_scene_stream_tools(cd_mcp_server_t* server);

/* ============================================================================
 * Nav tools: nav.bake, nav.find_path, nav.debug_draw (S4.1)
 * ============================================================================ */

/**
 * Register the navigation/pathfinding tool handlers with the MCP server.
 *
 * Registers:
 *   - nav.bake       : Trigger navgrid obstacle bake
 *   - nav.find_path  : Find A* path between two world positions
 *   - nav.debug_draw : Enable/disable navgrid debug visualization
 *
 * Requires a NavGrid to exist as _G._nav_grid in the Lua state
 * (created by nav_controller.lua or user code via require("nav_grid")).
 *
 * @param server  Pointer to an initialized cd_mcp_server_t.
 * @return CD_OK on success, CD_ERR_NULL if server is NULL.
 */
cd_result_t cd_mcp_register_nav_tools(cd_mcp_server_t* server);

/**
 * Set the Lua eval function for nav tools (called by scripting_lua at init).
 */
void cd_mcp_nav_tools_set_lua_eval(void* fn);

/* ============================================================================
 * Dialogue / Quest tools: dialogue.start, dialogue.advance, dialogue.status,
 *                         quest.status, quest.update (S4.3)
 * ============================================================================ */

/**
 * Register the dialogue and quest tool handlers with the MCP server.
 *
 * Registers:
 *   - dialogue.start   : Start a dialogue by registered ID
 *   - dialogue.advance : Advance or choose in active dialogue
 *   - dialogue.status  : Get current dialogue state
 *   - quest.status     : Get quest tracker state
 *   - quest.update     : Update a quest objective
 *
 * Requires _G._dialogue_manager and/or _G._quest_tracker to exist
 * in the Lua state (created by user scripts via require("dialogue")/require("quest")).
 *
 * @param server  Pointer to an initialized cd_mcp_server_t.
 * @return CD_OK on success, CD_ERR_NULL if server is NULL.
 */
cd_result_t cd_mcp_register_dialogue_tools(cd_mcp_server_t* server);

/**
 * Set the Lua eval function for dialogue/quest tools (called by scripting_lua at init).
 */
void cd_mcp_dialogue_tools_set_lua_eval(void* fn);

/* ============================================================================
 * Inventory tools: inventory.list, inventory.add, inventory.remove,
 *                  inventory.inspect
 * ============================================================================ */

/* ============================================================================
 * Mesh tools: mesh.attach_primitive, mesh.set_material, mesh.remove
 * ============================================================================ */

/**
 * Register the mesh tool handlers with the MCP server.
 *
 * Registers:
 *   - mesh.attach_primitive : Attach a primitive mesh with material to a node
 *   - mesh.set_material     : Update material properties on a MeshRenderer
 *   - mesh.remove           : Remove MeshRenderer component from a node
 *
 * @param server  Pointer to an initialized cd_mcp_server_t.
 * @return CD_OK on success, CD_ERR_NULL if server is NULL.
 */
cd_result_t cd_mcp_register_mesh_tools(cd_mcp_server_t* server);

/**
 * Register inventory management tools.
 *
 * @param server  Pointer to an initialized cd_mcp_server_t.
 * @return CD_OK on success, CD_ERR_NULL if server is NULL.
 */
cd_result_t cd_mcp_register_inventory_tools(cd_mcp_server_t* server);

/* ============================================================================
 * Scene validate tools: scene.validate (Task 2.5)
 * ============================================================================ */

/**
 * Register the scene validation tool handlers with the MCP server.
 *
 * Registers:
 *   - scene.validate : Validate a TOML scene file against the expected schema.
 *                      Checks node ID uniqueness, parent references, component
 *                      type registration, asset existence, and warns about
 *                      missing transforms and orphan nodes.
 *
 * @param server  Pointer to an initialized cd_mcp_server_t.
 * @return CD_OK on success, CD_ERR_NULL if server is NULL.
 */
cd_result_t cd_mcp_register_scene_validate_tools(cd_mcp_server_t* server);

/**
 * Set the Lua eval function for inventory tools (called by scripting_lua at init).
 */
void cd_mcp_inventory_tools_set_lua_eval(void* fn);

#endif /* CD_MCP_TOOLS_H */
