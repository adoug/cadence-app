/* cd_mcp_viewport_tools.c - Cadence Engine MCP viewport tool handlers
 *
 * Implements:
 *   - viewport.capture    : Capture the current renderer output as a PNG image
 *                           encoded in base64. When the renderer does not
 *                           support pixel capture (e.g. null renderer in
 *                           headless mode), falls back to returning a JSON
 *                           scene description so AI agents can understand the
 *                           scene without a visual screenshot.
 *   - viewport.setCamera  : Set which camera node is active for rendering.
 *
 * All handlers follow the cd_mcp_tool_handler_t signature.
 *
 * When the renderer HAL supports pixel capture (e.g. OpenGL), the handler
 * reads pixels via hal->capture(), encodes them as PNG with stb_image_write,
 * base64-encodes the result, and returns it in a JSON response.
 *
 * When the renderer does not support capture (e.g. null renderer), the handler
 * falls back to a JSON scene description (Task 4.11).
 */

#include "cadence/cd_mcp_tools.h"
#include "cadence/cd_mcp.h"
#include "cadence/cd_kernel.h"
#include "cadence/cd_kernel_api.h"
#include "cadence/cd_scene.h"
#include "cadence/cd_commands.h"
#include "cadence/cd_render_hal.h"
#include "cadence/cd_type_registry.h"
#include "cadence/cd_memory.h"
#include "cadence/cd_renderer_types.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* stb_image_write — for PNG encoding via callback.
 * The implementation lives in third_party/stb/stb_impl.c;
 * we only include the header here (no IMPLEMENTATION define). */
#include "stb_image_write.h"

/* ============================================================================
 * Base64 encoder
 *
 * Standard base64 alphabet: A-Z, a-z, 0-9, +, / with = padding.
 * ============================================================================ */

static const char b64_table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/**
 * Encode binary data to a base64 string.
 *
 * @param data       Input bytes.
 * @param input_len  Number of input bytes.
 * @param output_len Receives the length of the base64 string (excluding NUL).
 *                   May be NULL.
 * @return Heap-allocated base64 string (caller frees), or NULL on failure.
 */
char* cd_base64_encode(const uint8_t* data, size_t input_len,
                       size_t* output_len)
{
    if (data == NULL && input_len > 0) return NULL;

    size_t out_len = 4 * ((input_len + 2) / 3);
    char* out = (char*)malloc(out_len + 1);
    if (out == NULL) return NULL;

    size_t i = 0;
    size_t j = 0;

    while (i + 2 < input_len) {
        uint32_t triple = ((uint32_t)data[i] << 16)
                        | ((uint32_t)data[i + 1] << 8)
                        |  (uint32_t)data[i + 2];
        out[j++] = b64_table[(triple >> 18) & 0x3F];
        out[j++] = b64_table[(triple >> 12) & 0x3F];
        out[j++] = b64_table[(triple >>  6) & 0x3F];
        out[j++] = b64_table[ triple        & 0x3F];
        i += 3;
    }

    if (i < input_len) {
        uint32_t a = data[i];
        uint32_t b = (i + 1 < input_len) ? data[i + 1] : 0;

        out[j++] = b64_table[(a >> 2) & 0x3F];
        out[j++] = b64_table[((a & 0x03) << 4) | ((b >> 4) & 0x0F)];

        if (i + 1 < input_len) {
            out[j++] = b64_table[(b & 0x0F) << 2];
        } else {
            out[j++] = '=';
        }
        out[j++] = '=';
    }

    out[j] = '\0';
    if (output_len != NULL) *output_len = j;
    return out;
}

/* ============================================================================
 * PNG-to-memory callback for stb_image_write
 *
 * stbi_write_png_to_func calls this callback with chunks of PNG data.
 * We accumulate them into a growing buffer.
 * ============================================================================ */

typedef struct {
    uint8_t* data;
    size_t   size;
    size_t   capacity;
    bool     failed;
} cd_png_buffer_t;

static void cd_png_write_callback(void* context, void* data, int size) {
    cd_png_buffer_t* buf = (cd_png_buffer_t*)context;
    if (buf->failed || size <= 0) return;

    size_t needed = buf->size + (size_t)size;
    if (needed > buf->capacity) {
        size_t new_cap = buf->capacity * 2;
        if (new_cap < needed) new_cap = needed;
        if (new_cap < 4096) new_cap = 4096;

        uint8_t* new_data = (uint8_t*)realloc(buf->data, new_cap);
        if (new_data == NULL) {
            buf->failed = true;
            return;
        }
        buf->data = new_data;
        buf->capacity = new_cap;
    }

    memcpy(buf->data + buf->size, data, (size_t)size);
    buf->size += (size_t)size;
}

/* ============================================================================
 * Internal: Encode RGBA8 pixels to PNG in memory
 *
 * Returns a heap-allocated buffer of PNG bytes, or NULL on failure.
 * Caller is responsible for freeing the returned buffer.
 * ============================================================================ */

uint8_t* cd_encode_png(const void* rgba_pixels, uint32_t width,
                       uint32_t height, size_t* out_png_size)
{
    if (rgba_pixels == NULL || width == 0 || height == 0) return NULL;

    cd_png_buffer_t buf;
    memset(&buf, 0, sizeof(buf));

    int stride = (int)(width * 4);  /* RGBA8 = 4 bytes per pixel */

    int ok = stbi_write_png_to_func(cd_png_write_callback, &buf,
                                     (int)width, (int)height,
                                     4 /* RGBA */, rgba_pixels, stride);

    if (!ok || buf.failed || buf.data == NULL) {
        free(buf.data);
        return NULL;
    }

    if (out_png_size != NULL) *out_png_size = buf.size;
    return buf.data;
}

/* ============================================================================
 * Internal: Parse "index:generation" string to cd_id_t
 *
 * Returns CD_ID_INVALID on parse failure.
 * ============================================================================ */

static cd_id_t cd_id_parse(const char* str) {
    if (str == NULL || str[0] == '\0') {
        return CD_ID_INVALID;
    }
    unsigned int index = 0;
    unsigned int gen   = 0;
    int scanned = sscanf(str, "%u:%u", &index, &gen);
    if (scanned != 2) {
        return CD_ID_INVALID;
    }
    return cd_id_make((uint32_t)gen, (uint32_t)index);
}

/* ============================================================================
 * Internal: Format a cd_id_t as "index:generation" string
 *
 * Writes into the provided buffer and returns a pointer to it.
 * Buffer must be at least 24 bytes (two uint32 decimals + colon + NUL).
 * ============================================================================ */

#define CD_ID_STR_BUF_SIZE 24

static const char* cd_id_format(cd_id_t id, char* buf, size_t buf_size) {
    uint32_t index = cd_id_index(id);
    uint32_t gen   = cd_id_generation(id);
    snprintf(buf, buf_size, "%u:%u", index, gen);
    return buf;
}

/* ============================================================================
 * Internal: Build a JSON node object for a single scene node
 *
 * Creates a cJSON object with id, name, parent_id, children, transform,
 * components, and tags for the given node.
 * ============================================================================ */

static cJSON* cd_mcp_build_node_json(cd_scene_t* scene,
                                      cd_type_registry_t* types,
                                      cd_node_t* node) {
    (void)scene; /* Reserved for future use (e.g., resolving child names) */

    cJSON* node_obj = cJSON_CreateObject();
    if (node_obj == NULL) return NULL;

    /* id */
    char id_buf[CD_ID_STR_BUF_SIZE];
    cd_id_format(node->id, id_buf, sizeof(id_buf));
    cJSON_AddStringToObject(node_obj, "id", id_buf);

    /* name */
    cJSON_AddStringToObject(node_obj, "name", node->name.buf);

    /* parent_id */
    if (cd_id_is_valid(node->parent)) {
        char parent_buf[CD_ID_STR_BUF_SIZE];
        cd_id_format(node->parent, parent_buf, sizeof(parent_buf));
        cJSON_AddStringToObject(node_obj, "parent_id", parent_buf);
    } else {
        cJSON_AddNullToObject(node_obj, "parent_id");
    }

    /* children */
    cJSON* children_arr = cJSON_CreateArray();
    if (children_arr != NULL) {
        for (uint32_t i = 0; i < node->child_count; i++) {
            char child_buf[CD_ID_STR_BUF_SIZE];
            cd_id_format(node->children[i], child_buf, sizeof(child_buf));
            cJSON_AddItemToArray(children_arr, cJSON_CreateString(child_buf));
        }
    }
    cJSON_AddItemToObject(node_obj, "children", children_arr);

    /* transform */
    cJSON* transform = cJSON_CreateObject();
    if (transform != NULL) {
        cJSON* pos = cJSON_CreateObject();
        if (pos != NULL) {
            cJSON_AddNumberToObject(pos, "x",
                (double)node->local_transform.position.x);
            cJSON_AddNumberToObject(pos, "y",
                (double)node->local_transform.position.y);
            cJSON_AddNumberToObject(pos, "z",
                (double)node->local_transform.position.z);
        }
        cJSON_AddItemToObject(transform, "position", pos);

        cJSON* rot = cJSON_CreateObject();
        if (rot != NULL) {
            cJSON_AddNumberToObject(rot, "x",
                (double)node->local_transform.rotation.x);
            cJSON_AddNumberToObject(rot, "y",
                (double)node->local_transform.rotation.y);
            cJSON_AddNumberToObject(rot, "z",
                (double)node->local_transform.rotation.z);
            cJSON_AddNumberToObject(rot, "w",
                (double)node->local_transform.rotation.w);
        }
        cJSON_AddItemToObject(transform, "rotation", rot);

        cJSON* scl = cJSON_CreateObject();
        if (scl != NULL) {
            cJSON_AddNumberToObject(scl, "x",
                (double)node->local_transform.scale.x);
            cJSON_AddNumberToObject(scl, "y",
                (double)node->local_transform.scale.y);
            cJSON_AddNumberToObject(scl, "z",
                (double)node->local_transform.scale.z);
        }
        cJSON_AddItemToObject(transform, "scale", scl);
    }
    cJSON_AddItemToObject(node_obj, "transform", transform);

    /* components */
    cJSON* comps_arr = cJSON_CreateArray();
    if (comps_arr != NULL) {
        for (uint32_t c = 0; c < node->component_count; c++) {
            cJSON* comp_obj = cJSON_CreateObject();
            if (comp_obj == NULL) continue;

            cd_component_entry_t* entry = &node->components[c];

            /* Look up type name from the type registry */
            const char* type_name = "Unknown";
            if (types != NULL) {
                cd_type_info_t* info = cd_type_get(types, entry->type_id);
                if (info != NULL) {
                    type_name = info->name.buf;
                }
            }
            cJSON_AddStringToObject(comp_obj, "type", type_name);

            /* type_id for reference */
            char type_id_buf[CD_ID_STR_BUF_SIZE];
            cd_id_format(entry->type_id, type_id_buf, sizeof(type_id_buf));
            cJSON_AddStringToObject(comp_obj, "type_id", type_id_buf);

            cJSON_AddNumberToObject(comp_obj, "size", (double)entry->size);

            cJSON_AddItemToArray(comps_arr, comp_obj);
        }
    }
    cJSON_AddItemToObject(node_obj, "components", comps_arr);

    /* tags */
    cJSON* tags_arr = cJSON_CreateArray();
    if (tags_arr != NULL) {
        for (uint32_t t = 0; t < node->tag_count; t++) {
            cJSON_AddItemToArray(tags_arr,
                cJSON_CreateString(node->tag_names[t].buf));
        }
    }
    cJSON_AddItemToObject(node_obj, "tags", tags_arr);

    /* enabled */
    cJSON_AddBoolToObject(node_obj, "enabled", node->enabled);

    return node_obj;
}

/* ============================================================================
 * Internal: Build a JSON scene description for use when the renderer does
 * not support pixel capture (null/headless mode).
 *
 * Returns a cJSON result object describing the scene, or NULL on error
 * (with error_code and error_msg set).
 * ============================================================================ */

static cJSON* cd_mcp_build_scene_description(
    struct cd_kernel_t* kernel,
    int*                error_code,
    const char**        error_msg)
{
    if (kernel == NULL) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Kernel not available";
        return NULL;
    }

    cJSON* result = cJSON_CreateObject();
    if (result == NULL) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Failed to allocate JSON response";
        return NULL;
    }

    cJSON_AddStringToObject(result, "type", "scene_description");
    cJSON_AddStringToObject(result, "format", "json");

    /* Get the renderer backend name if available */
    const cd_render_hal_t* hal = cd_render_hal_get();
    if (hal != NULL && hal->get_backend_name != NULL) {
        cJSON_AddStringToObject(result, "renderer",
            hal->get_backend_name());
    } else {
        cJSON_AddStringToObject(result, "renderer", "None");
    }

    /* Build scene object */
    cJSON* scene_obj = cJSON_CreateObject();
    if (scene_obj == NULL) {
        cJSON_Delete(result);
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Failed to allocate JSON response";
        return NULL;
    }

    cd_scene_t* scene = cd_kernel_get_scene(kernel);
    if (scene == NULL) {
        /* Empty scene -- valid but with 0 nodes */
        cJSON_AddNumberToObject(scene_obj, "node_count", 0);
        cJSON_AddItemToObject(scene_obj, "nodes", cJSON_CreateArray());
        cJSON_AddItemToObject(result, "scene", scene_obj);
        return result;
    }

    /* Count live nodes and collect them */
    cJSON* nodes_arr = cJSON_CreateArray();
    uint32_t node_count = 0;

    for (uint32_t i = 0; i < scene->nodes.capacity; i++) {
        uint32_t gen = scene->nodes.generations[i];
        if (gen == 0) {
            continue; /* Never used */
        }

        cd_node_t* node = (cd_node_t*)((uint8_t*)scene->nodes.data +
                          (i * scene->nodes.element_size));

        /* The node is live only if its stored id generation matches
         * the slot's current generation */
        if (cd_id_generation(node->id) != gen) {
            continue; /* Deleted slot */
        }

        node_count++;

        if (nodes_arr != NULL) {
            cJSON* node_json = cd_mcp_build_node_json(
                scene, cd_kernel_get_types(kernel), node);
            if (node_json != NULL) {
                cJSON_AddItemToArray(nodes_arr, node_json);
            }
        }
    }

    cJSON_AddNumberToObject(scene_obj, "node_count", (double)node_count);
    cJSON_AddItemToObject(scene_obj, "nodes",
        nodes_arr != NULL ? nodes_arr : cJSON_CreateArray());

    cJSON_AddItemToObject(result, "scene", scene_obj);

    return result;
}

/* ============================================================================
 * viewport.capture handler
 *
 * Input:  { "width": 800, "height": 600, "format": "png" }
 * Output (with capture support):
 *   { "type": "image", "format": "png", "width": 800, "height": 600,
 *     "data": "<base64 encoded PNG>" }
 *
 * Output (without capture support -- null renderer fallback):
 *   { "type": "scene_description", "format": "json",
 *     "renderer": "Null",
 *     "scene": { "node_count": N, "nodes": [...] } }
 *
 * Error cases:
 *   -32602  Unsupported format (only "png" supported)
 *   -32602  Zero width or height
 *   -32603  Capture failed
 *   -32603  PNG encoding failed
 * ============================================================================ */

static cJSON* cd_mcp_handle_viewport_capture(
    struct cd_kernel_t* kernel,
    const cJSON*        params,
    int*                error_code,
    const char**        error_msg)
{
    /* Validate kernel */
    if (kernel == NULL) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Kernel not available";
        return NULL;
    }

    /* --- Parse parameters ------------------------------------------------ */

    uint32_t width  = 800;  /* default */
    uint32_t height = 600;  /* default */

    if (params != NULL) {
        const cJSON* width_item = cJSON_GetObjectItemCaseSensitive(
            params, "width");
        if (width_item != NULL && cJSON_IsNumber(width_item)) {
            int w = (int)width_item->valuedouble;
            if (w > 0) width = (uint32_t)w;
        }

        const cJSON* height_item = cJSON_GetObjectItemCaseSensitive(
            params, "height");
        if (height_item != NULL && cJSON_IsNumber(height_item)) {
            int h = (int)height_item->valuedouble;
            if (h > 0) height = (uint32_t)h;
        }

        /* Check format (only "png" is supported) */
        const cJSON* format_item = cJSON_GetObjectItemCaseSensitive(
            params, "format");
        if (format_item != NULL && cJSON_IsString(format_item) &&
            format_item->valuestring != NULL) {
            if (strcmp(format_item->valuestring, "png") != 0) {
                *error_code = CD_JSONRPC_INVALID_PARAMS;
                *error_msg  = "Unsupported format, only 'png' is supported";
                return NULL;
            }
        }
    }

    /* Reject zero dimensions */
    if (width == 0 || height == 0) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = "Width and height must be greater than zero";
        return NULL;
    }

    /* --- Get the renderer HAL -------------------------------------------- */

    const cd_render_hal_t* hal = cd_render_hal_get();

    /* --- Check capture support; fall back to scene description ----------- */

    if (hal == NULL || hal->supports_capture == NULL ||
        !hal->supports_capture()) {
        /* No renderer or renderer does not support pixel capture.
         * Fall back to returning a JSON scene description (Task 4.11). */
        return cd_mcp_build_scene_description(kernel, error_code, error_msg);
    }

    /* --- Capture pixels -------------------------------------------------- */

    void*    pixels   = NULL;
    uint32_t pix_size = 0;

    if (hal->capture == NULL) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Capture function not implemented";
        return NULL;
    }

    cd_result_t res = hal->capture(width, height, &pixels, &pix_size);
    if (res != CD_OK || pixels == NULL) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Capture failed";
        return NULL;
    }

    /* --- Encode to PNG --------------------------------------------------- */

    size_t   png_size = 0;
    uint8_t* png_data = cd_encode_png(pixels, width, height, &png_size);
    cd_mem_free_tagged(pixels);

    if (png_data == NULL || png_size == 0) {
        free(png_data);
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "PNG encoding failed";
        return NULL;
    }

    /* --- Base64 encode --------------------------------------------------- */

    size_t b64_len = 0;
    char*  b64_str = cd_base64_encode(png_data, png_size, &b64_len);
    free(png_data);

    if (b64_str == NULL) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Base64 encoding failed";
        return NULL;
    }

    /* --- Build JSON response --------------------------------------------- */

    cJSON* result = cJSON_CreateObject();
    if (result == NULL) {
        free(b64_str);
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Failed to allocate JSON response";
        return NULL;
    }

    cJSON_AddStringToObject(result, "type", "image");
    cJSON_AddStringToObject(result, "format", "png");
    cJSON_AddNumberToObject(result, "width", (double)width);
    cJSON_AddNumberToObject(result, "height", (double)height);
    cJSON_AddStringToObject(result, "data", b64_str);

    free(b64_str);
    return result;
}

/* ============================================================================
 * viewport.setCamera handler
 *
 * Input:  { "node_id": "1:1",
 *           "position": { "x": 0, "y": 5, "z": 10 },    (optional)
 *           "rotation": { "x": 0, "y": 0, "z": 0, "w": 1 },  (optional)
 *           "fov": 60,    (optional)
 *           "near": 0.1,  (optional)
 *           "far": 1000   (optional)
 *         }
 *
 * Output: { "status": "ok",
 *           "camera_id": "1:1",
 *           "fov_degrees": 60.0,
 *           "near_clip": 0.1,
 *           "far_clip": 1000.0,
 *           "active": true }
 *
 * Error cases:
 *   -32602  Missing/invalid node_id, node not found, no Camera3D component
 *   -32603  Scene not available, kernel null
 * ============================================================================ */

static cJSON* cd_mcp_handle_viewport_set_camera(
    struct cd_kernel_t* kernel,
    const cJSON*        params,
    int*                error_code,
    const char**        error_msg)
{
    /* Validate kernel */
    if (kernel == NULL) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Kernel not available";
        return NULL;
    }

    /* Validate scene */
    if (cd_kernel_get_scene(kernel) == NULL) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Scene not available";
        return NULL;
    }

    /* --- Parse node_id (required) ---------------------------------------- */

    if (params == NULL) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = "Missing required parameter: node_id";
        return NULL;
    }

    const cJSON* node_id_item = cJSON_GetObjectItemCaseSensitive(
        params, "node_id");
    if (node_id_item == NULL || !cJSON_IsString(node_id_item) ||
        node_id_item->valuestring == NULL ||
        node_id_item->valuestring[0] == '\0') {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = "Missing or empty required parameter: node_id";
        return NULL;
    }

    cd_id_t node_id = cd_id_parse(node_id_item->valuestring);
    if (!cd_id_is_valid(node_id)) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = "Invalid node_id format (expected \"index:generation\")";
        return NULL;
    }

    /* --- Validate node exists -------------------------------------------- */

    cd_node_t* node = cd_node_get(cd_kernel_get_scene(kernel), node_id);
    if (node == NULL) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = "Node not found";
        return NULL;
    }

    /* --- Validate Camera3D component exists ------------------------------ */

    cd_id_t cam_type = cd_type_find(cd_kernel_get_types(kernel), "Camera3D");
    if (!cd_id_is_valid(cam_type)) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Camera3D type not registered";
        return NULL;
    }

    cd_camera3d_t* camera = (cd_camera3d_t*)cd_node_get_component(
        cd_kernel_get_scene(kernel), node_id, cam_type);
    if (camera == NULL) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = "Node does not have a Camera3D component";
        return NULL;
    }

    /* --- Deactivate all other cameras ------------------------------------ */

    cd_id_t camera_nodes[CD_SCENE_MAX_QUERY_RESULTS];
    uint32_t camera_count = 0;
    cd_scene_find_by_component(cd_kernel_get_scene(kernel), cam_type,
                                camera_nodes, &camera_count,
                                CD_SCENE_MAX_QUERY_RESULTS);

    for (uint32_t i = 0; i < camera_count; i++) {
        if (camera_nodes[i] != node_id) {
            cd_camera3d_t* other = (cd_camera3d_t*)cd_node_get_component(
                cd_kernel_get_scene(kernel), camera_nodes[i], cam_type);
            if (other != NULL) {
                other->active = false;
            }
        }
    }

    /* --- Activate the target camera -------------------------------------- */

    camera->active = true;

    /* --- Apply optional overrides ---------------------------------------- */

    /* Position override */
    const cJSON* pos_item = cJSON_GetObjectItemCaseSensitive(params, "position");
    if (pos_item != NULL && cJSON_IsObject(pos_item)) {
        cd_transform_t transform = node->local_transform;
        const cJSON* px = cJSON_GetObjectItemCaseSensitive(pos_item, "x");
        const cJSON* py = cJSON_GetObjectItemCaseSensitive(pos_item, "y");
        const cJSON* pz = cJSON_GetObjectItemCaseSensitive(pos_item, "z");
        if (px != NULL && cJSON_IsNumber(px))
            transform.position.x = (float)px->valuedouble;
        if (py != NULL && cJSON_IsNumber(py))
            transform.position.y = (float)py->valuedouble;
        if (pz != NULL && cJSON_IsNumber(pz))
            transform.position.z = (float)pz->valuedouble;
        cd_command_t pos_cmd;
        memset(&pos_cmd, 0, sizeof(pos_cmd));
        pos_cmd.type = CD_CMD_SET_TRANSFORM;
        pos_cmd.target = node_id;
        pos_cmd.payload.set_transform.transform = transform;
        cd_command_execute_sync(
            cd_kernel_get_commands(kernel), &pos_cmd, cd_kernel_get_scene(kernel), cd_kernel_get_events(kernel));
        /* Re-fetch node after command execution */
        node = cd_node_get(cd_kernel_get_scene(kernel), node_id);
    }

    /* Rotation override */
    const cJSON* rot_item = cJSON_GetObjectItemCaseSensitive(params, "rotation");
    if (rot_item != NULL && cJSON_IsObject(rot_item)) {
        cd_transform_t transform = node->local_transform;
        const cJSON* rx = cJSON_GetObjectItemCaseSensitive(rot_item, "x");
        const cJSON* ry = cJSON_GetObjectItemCaseSensitive(rot_item, "y");
        const cJSON* rz = cJSON_GetObjectItemCaseSensitive(rot_item, "z");
        const cJSON* rw = cJSON_GetObjectItemCaseSensitive(rot_item, "w");
        if (rx != NULL && cJSON_IsNumber(rx))
            transform.rotation.x = (float)rx->valuedouble;
        if (ry != NULL && cJSON_IsNumber(ry))
            transform.rotation.y = (float)ry->valuedouble;
        if (rz != NULL && cJSON_IsNumber(rz))
            transform.rotation.z = (float)rz->valuedouble;
        if (rw != NULL && cJSON_IsNumber(rw))
            transform.rotation.w = (float)rw->valuedouble;
        cd_command_t rot_cmd;
        memset(&rot_cmd, 0, sizeof(rot_cmd));
        rot_cmd.type = CD_CMD_SET_TRANSFORM;
        rot_cmd.target = node_id;
        rot_cmd.payload.set_transform.transform = transform;
        cd_command_execute_sync(
            cd_kernel_get_commands(kernel), &rot_cmd, cd_kernel_get_scene(kernel), cd_kernel_get_events(kernel));
        node = cd_node_get(cd_kernel_get_scene(kernel), node_id);
    }

    /* FOV override */
    const cJSON* fov_item = cJSON_GetObjectItemCaseSensitive(params, "fov");
    if (fov_item != NULL && cJSON_IsNumber(fov_item)) {
        camera->fov_degrees = (float)fov_item->valuedouble;
    }

    /* Near clip override */
    const cJSON* near_item = cJSON_GetObjectItemCaseSensitive(params, "near");
    if (near_item != NULL && cJSON_IsNumber(near_item)) {
        camera->near_clip = (float)near_item->valuedouble;
    }

    /* Far clip override */
    const cJSON* far_item = cJSON_GetObjectItemCaseSensitive(params, "far");
    if (far_item != NULL && cJSON_IsNumber(far_item)) {
        camera->far_clip = (float)far_item->valuedouble;
    }

    /* --- Build JSON response --------------------------------------------- */

    cJSON* result = cJSON_CreateObject();
    if (result == NULL) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Failed to allocate JSON response";
        return NULL;
    }

    char id_buf[CD_ID_STR_BUF_SIZE];
    cd_id_format(node_id, id_buf, sizeof(id_buf));

    cJSON_AddStringToObject(result, "status", "ok");
    cJSON_AddStringToObject(result, "camera_id", id_buf);
    cJSON_AddNumberToObject(result, "fov_degrees", (double)camera->fov_degrees);
    cJSON_AddNumberToObject(result, "near_clip", (double)camera->near_clip);
    cJSON_AddNumberToObject(result, "far_clip", (double)camera->far_clip);
    cJSON_AddBoolToObject(result, "active", camera->active);

    return result;
}

/* ============================================================================
 * Registration
 * ============================================================================ */

cd_result_t cd_mcp_register_viewport_tools(cd_mcp_server_t* server) {
    if (server == NULL) {
        return CD_ERR_NULL;
    }

    cd_result_t res = cd_mcp_register_tool_ex(server, "viewport.capture",
        cd_mcp_handle_viewport_capture,
        "Capture the current viewport as a base64-encoded PNG image.",
        "{\"type\":\"object\",\"properties\":{"
        "\"width\":{\"type\":\"integer\",\"minimum\":1,\"description\":\"Capture width in pixels (default 800)\"},"
        "\"height\":{\"type\":\"integer\",\"minimum\":1,\"description\":\"Capture height in pixels (default 600)\"},"
        "\"format\":{\"type\":\"string\",\"enum\":[\"png\"],\"description\":\"Image format (only png supported)\"}"
        "}}");
    if (res != CD_OK) return res;

    res = cd_mcp_register_tool_ex(server, "viewport.setCamera",
        cd_mcp_handle_viewport_set_camera,
        "Set the active camera node for rendering with optional transform and lens overrides.",
        "{\"type\":\"object\",\"properties\":{"
        "\"node_id\":{\"type\":\"string\",\"description\":\"Camera node ID as index:generation\"},"
        "\"position\":{\"type\":\"object\",\"properties\":{\"x\":{\"type\":\"number\"},\"y\":{\"type\":\"number\"},\"z\":{\"type\":\"number\"}}},"
        "\"rotation\":{\"type\":\"object\",\"properties\":{\"x\":{\"type\":\"number\"},\"y\":{\"type\":\"number\"},\"z\":{\"type\":\"number\"},\"w\":{\"type\":\"number\"}}},"
        "\"fov\":{\"type\":\"number\",\"description\":\"Field of view in degrees\"},"
        "\"near\":{\"type\":\"number\",\"description\":\"Near clip plane distance\"},"
        "\"far\":{\"type\":\"number\",\"description\":\"Far clip plane distance\"}"
        "},\"required\":[\"node_id\"]}");
    if (res != CD_OK) return res;

    return CD_OK;
}
