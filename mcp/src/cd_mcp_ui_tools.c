/* cd_mcp_ui_tools.c - Cadence Engine MCP UI tool handlers (S1.2)
 *
 * Implements:
 *   - ui.create_screen   : Create a named UI screen
 *   - ui.create_element  : Create a UI element with all properties
 *   - ui.update_element  : Modify element properties
 *   - ui.remove_element  : Remove a UI element
 *   - ui.query           : Get all screens/elements and their state
 *   - ui.show_screen     : Show a screen
 *   - ui.hide_screen     : Hide a screen
 *
 * All handlers follow the cd_mcp_tool_handler_t signature.
 */

#include "cadence/cd_mcp_tools.h"
#include "cadence/cd_mcp.h"
#include "cadence/cd_mcp_error.h"
#include "cadence/cd_kernel.h"
#include "cadence/cd_kernel_api.h"
#include "cadence/cd_ui.h"
#include "cJSON.h"

#include <string.h>
#include <stdio.h>

/* ============================================================================
 * Helper: get UI system from kernel
 * ============================================================================ */

static cd_ui_system_t* get_ui(struct cd_kernel_t* kernel) {
    if (!kernel) return NULL;
    return cd_kernel_get_ui(kernel);
}

/* ============================================================================
 * Helper: parse anchor name
 * ============================================================================ */

static cd_ui_anchor_t mcp_parse_anchor(const char* name) {
    if (!name) return CD_UI_ANCHOR_TOP_LEFT;
    if (strcmp(name, "top_left") == 0)      return CD_UI_ANCHOR_TOP_LEFT;
    if (strcmp(name, "top_center") == 0)    return CD_UI_ANCHOR_TOP_CENTER;
    if (strcmp(name, "top_right") == 0)     return CD_UI_ANCHOR_TOP_RIGHT;
    if (strcmp(name, "center_left") == 0)   return CD_UI_ANCHOR_CENTER_LEFT;
    if (strcmp(name, "center") == 0)        return CD_UI_ANCHOR_CENTER;
    if (strcmp(name, "center_right") == 0)  return CD_UI_ANCHOR_CENTER_RIGHT;
    if (strcmp(name, "bottom_left") == 0)   return CD_UI_ANCHOR_BOTTOM_LEFT;
    if (strcmp(name, "bottom_center") == 0) return CD_UI_ANCHOR_BOTTOM_CENTER;
    if (strcmp(name, "bottom_right") == 0)  return CD_UI_ANCHOR_BOTTOM_RIGHT;
    return CD_UI_ANCHOR_TOP_LEFT;
}

static const char* anchor_to_string(cd_ui_anchor_t a) {
    switch (a) {
        case CD_UI_ANCHOR_TOP_LEFT:      return "top_left";
        case CD_UI_ANCHOR_TOP_CENTER:    return "top_center";
        case CD_UI_ANCHOR_TOP_RIGHT:     return "top_right";
        case CD_UI_ANCHOR_CENTER_LEFT:   return "center_left";
        case CD_UI_ANCHOR_CENTER:        return "center";
        case CD_UI_ANCHOR_CENTER_RIGHT:  return "center_right";
        case CD_UI_ANCHOR_BOTTOM_LEFT:   return "bottom_left";
        case CD_UI_ANCHOR_BOTTOM_CENTER: return "bottom_center";
        case CD_UI_ANCHOR_BOTTOM_RIGHT:  return "bottom_right";
    }
    return "top_left";
}

static const char* type_to_string(cd_ui_type_t t) {
    switch (t) {
        case CD_UI_LABEL:    return "label";
        case CD_UI_PANEL:    return "panel";
        case CD_UI_IMAGE:    return "image";
        case CD_UI_PROGRESS: return "progress";
        case CD_UI_BUTTON:   return "button";
    }
    return "unknown";
}

/* ============================================================================
 * ui.create_screen
 * ============================================================================ */

static cJSON* cd_mcp_handle_ui_create_screen(
    struct cd_kernel_t* kernel, const cJSON* params,
    int* error_code, const char** error_msg)
{
    cd_ui_system_t* ui = get_ui(kernel);
    if (!ui) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg = cd_mcp_error_fmt(
            "UI system not initialized",
            "The ui_runtime plugin is not loaded or failed to initialize.",
            "Ensure the ui_runtime plugin is listed in project.toml and loaded before calling UI tools.");
        return NULL;
    }

    const cJSON* name_item = cJSON_GetObjectItemCaseSensitive(params, "name");
    if (!name_item || !cJSON_IsString(name_item)) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg = cd_mcp_error_fmt(
            "Missing required parameter: name",
            "ui.create_screen requires a 'name' string to identify the screen.",
            "Example: {\"name\": \"hud\"} or {\"name\": \"pause_menu\"}");
        return NULL;
    }

    int32_t idx = cd_ui_screen_get_or_create(ui, name_item->valuestring);
    if (idx < 0) {
        char detail[256];
        snprintf(detail, sizeof(detail),
                 "Cannot create screen '%s': all %d screen slots are in use.",
                 name_item->valuestring, CD_UI_MAX_SCREENS);
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg = cd_mcp_error_fmt(
            "Max UI screens reached",
            detail,
            "Remove unused screens with ui.remove_element or increase CD_UI_MAX_SCREENS.");
        return NULL;
    }

    cJSON* result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "name", name_item->valuestring);
    cJSON_AddNumberToObject(result, "index", (double)idx);
    return result;
}

/* ============================================================================
 * ui.create_element
 * ============================================================================ */

static cJSON* cd_mcp_handle_ui_create_element(
    struct cd_kernel_t* kernel, const cJSON* params,
    int* error_code, const char** error_msg)
{
    cd_ui_system_t* ui = get_ui(kernel);
    if (!ui) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg = cd_mcp_error_fmt(
            "UI system not initialized",
            "The ui_runtime plugin is not loaded or failed to initialize.",
            "Ensure the ui_runtime plugin is listed in project.toml and loaded before calling UI tools.");
        return NULL;
    }

    const cJSON* type_item = cJSON_GetObjectItemCaseSensitive(params, "type");
    const cJSON* screen_item = cJSON_GetObjectItemCaseSensitive(params, "screen");
    if (!type_item || !cJSON_IsString(type_item)) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg = cd_mcp_error_fmt(
            "Missing required parameter: type",
            "ui.create_element requires a 'type' string specifying the element kind.",
            "Valid types: label, panel, button, progress. Example: {\"type\": \"label\", \"text\": \"Hello\"}");
        return NULL;
    }

    const char* screen = screen_item && cJSON_IsString(screen_item) ?
                         screen_item->valuestring : "hud";
    const char* type = type_item->valuestring;

    /* Extract common properties */
    const cJSON* text_item = cJSON_GetObjectItemCaseSensitive(params, "text");
    const cJSON* x_item = cJSON_GetObjectItemCaseSensitive(params, "x");
    const cJSON* y_item = cJSON_GetObjectItemCaseSensitive(params, "y");
    const cJSON* w_item = cJSON_GetObjectItemCaseSensitive(params, "width");
    const cJSON* h_item = cJSON_GetObjectItemCaseSensitive(params, "height");
    const cJSON* fs_item = cJSON_GetObjectItemCaseSensitive(params, "font_size");

    float x = x_item && cJSON_IsNumber(x_item) ? (float)x_item->valuedouble : 0.0f;
    float y = y_item && cJSON_IsNumber(y_item) ? (float)y_item->valuedouble : 0.0f;
    float w = w_item && cJSON_IsNumber(w_item) ? (float)w_item->valuedouble : 100.0f;
    float h = h_item && cJSON_IsNumber(h_item) ? (float)h_item->valuedouble : 30.0f;
    float fs = fs_item && cJSON_IsNumber(fs_item) ? (float)fs_item->valuedouble : 16.0f;
    const char* text = text_item && cJSON_IsString(text_item) ?
                       text_item->valuestring : "";

    int32_t id = -1;
    if (strcmp(type, "label") == 0) {
        id = cd_ui_create_label(ui, screen, text, x, y, fs);
    } else if (strcmp(type, "panel") == 0) {
        cd_ui_color_t color = {0.2f, 0.2f, 0.2f, 0.8f};
        id = cd_ui_create_panel(ui, screen, x, y, w, h, color);
    } else if (strcmp(type, "button") == 0) {
        id = cd_ui_create_button(ui, screen, text, x, y, w, h);
    } else if (strcmp(type, "progress") == 0) {
        const cJSON* val_item = cJSON_GetObjectItemCaseSensitive(params, "value");
        float val = val_item && cJSON_IsNumber(val_item) ? (float)val_item->valuedouble : 0.0f;
        cd_ui_color_t fill = {0.0f, 0.8f, 0.2f, 1.0f};
        cd_ui_color_t bg = {0.3f, 0.3f, 0.3f, 0.8f};
        id = cd_ui_create_progress(ui, screen, x, y, w, h, val, fill, bg);
    } else {
        char detail[256];
        snprintf(detail, sizeof(detail),
                 "Received type='%s', which is not a recognized UI element type.",
                 type);
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg = cd_mcp_error_fmt(
            "Invalid element type",
            detail,
            "Valid types: label, panel, button, progress.");
        return NULL;
    }

    if (id < 0) {
        char detail[256];
        snprintf(detail, sizeof(detail),
                 "Cannot create element on screen '%s': all %d element slots are in use.",
                 screen, CD_UI_MAX_ELEMENTS);
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg = cd_mcp_error_fmt(
            "Max UI elements reached",
            detail,
            "Remove unused elements with ui.remove_element or use ui.query to inspect current elements.");
        return NULL;
    }

    /* Apply optional properties */
    const cJSON* anchor_item = cJSON_GetObjectItemCaseSensitive(params, "anchor");
    if (anchor_item && cJSON_IsString(anchor_item)) {
        cd_ui_set_anchor(ui, id, mcp_parse_anchor(anchor_item->valuestring));
    }

    const cJSON* visible_item = cJSON_GetObjectItemCaseSensitive(params, "visible");
    if (visible_item && cJSON_IsBool(visible_item)) {
        cd_ui_set_visible(ui, id, cJSON_IsTrue(visible_item));
    }

    cJSON* result = cJSON_CreateObject();
    cJSON_AddNumberToObject(result, "id", (double)id);
    cJSON_AddStringToObject(result, "type", type);
    return result;
}

/* ============================================================================
 * ui.update_element
 * ============================================================================ */

static cJSON* cd_mcp_handle_ui_update_element(
    struct cd_kernel_t* kernel, const cJSON* params,
    int* error_code, const char** error_msg)
{
    cd_ui_system_t* ui = get_ui(kernel);
    if (!ui) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg = cd_mcp_error_fmt(
            "UI system not initialized",
            "The ui_runtime plugin is not loaded or failed to initialize.",
            "Ensure the ui_runtime plugin is listed in project.toml and loaded before calling UI tools.");
        return NULL;
    }

    const cJSON* id_item = cJSON_GetObjectItemCaseSensitive(params, "id");
    if (!id_item || !cJSON_IsNumber(id_item)) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg = cd_mcp_error_fmt(
            "Missing required parameter: id",
            "ui.update_element requires an integer 'id' of the element to update.",
            "Use ui.query to list all elements and their IDs.");
        return NULL;
    }

    int32_t id = (int32_t)id_item->valuedouble;
    cd_ui_element_t* e = cd_ui_get_element(ui, id);
    if (!e) {
        char detail[256];
        snprintf(detail, sizeof(detail),
                 "No active UI element with id=%d. It may have been removed or never created.",
                 id);
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg = cd_mcp_error_fmt(
            "Element not found",
            detail,
            "Use ui.query to list all active elements and their IDs.");
        return NULL;
    }

    const cJSON* text_item = cJSON_GetObjectItemCaseSensitive(params, "text");
    if (text_item && cJSON_IsString(text_item))
        cd_ui_set_text(ui, id, text_item->valuestring);

    const cJSON* x_item = cJSON_GetObjectItemCaseSensitive(params, "x");
    const cJSON* y_item = cJSON_GetObjectItemCaseSensitive(params, "y");
    if (x_item && cJSON_IsNumber(x_item) && y_item && cJSON_IsNumber(y_item))
        cd_ui_set_position(ui, id, (float)x_item->valuedouble, (float)y_item->valuedouble);

    const cJSON* w_item = cJSON_GetObjectItemCaseSensitive(params, "width");
    const cJSON* h_item = cJSON_GetObjectItemCaseSensitive(params, "height");
    if (w_item && cJSON_IsNumber(w_item) && h_item && cJSON_IsNumber(h_item))
        cd_ui_set_size(ui, id, (float)w_item->valuedouble, (float)h_item->valuedouble);

    const cJSON* vis_item = cJSON_GetObjectItemCaseSensitive(params, "visible");
    if (vis_item && cJSON_IsBool(vis_item))
        cd_ui_set_visible(ui, id, cJSON_IsTrue(vis_item));

    const cJSON* anchor_item = cJSON_GetObjectItemCaseSensitive(params, "anchor");
    if (anchor_item && cJSON_IsString(anchor_item))
        cd_ui_set_anchor(ui, id, mcp_parse_anchor(anchor_item->valuestring));

    const cJSON* fs_item = cJSON_GetObjectItemCaseSensitive(params, "font_size");
    if (fs_item && cJSON_IsNumber(fs_item))
        e->font_size = (float)fs_item->valuedouble;

    const cJSON* prog_item = cJSON_GetObjectItemCaseSensitive(params, "value");
    if (prog_item && cJSON_IsNumber(prog_item))
        cd_ui_set_progress(ui, id, (float)prog_item->valuedouble);

    cJSON* result = cJSON_CreateObject();
    cJSON_AddNumberToObject(result, "id", (double)id);
    cJSON_AddBoolToObject(result, "updated", 1);
    return result;
}

/* ============================================================================
 * ui.remove_element
 * ============================================================================ */

static cJSON* cd_mcp_handle_ui_remove_element(
    struct cd_kernel_t* kernel, const cJSON* params,
    int* error_code, const char** error_msg)
{
    cd_ui_system_t* ui = get_ui(kernel);
    if (!ui) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg = cd_mcp_error_fmt(
            "UI system not initialized",
            "The ui_runtime plugin is not loaded or failed to initialize.",
            "Ensure the ui_runtime plugin is listed in project.toml and loaded before calling UI tools.");
        return NULL;
    }

    const cJSON* id_item = cJSON_GetObjectItemCaseSensitive(params, "id");
    if (!id_item || !cJSON_IsNumber(id_item)) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg = cd_mcp_error_fmt(
            "Missing required parameter: id",
            "ui.remove_element requires an integer 'id' of the element to remove.",
            "Use ui.query to list all elements and their IDs.");
        return NULL;
    }

    int32_t id = (int32_t)id_item->valuedouble;
    cd_ui_remove_element(ui, id);

    cJSON* result = cJSON_CreateObject();
    cJSON_AddNumberToObject(result, "id", (double)id);
    cJSON_AddBoolToObject(result, "removed", 1);
    return result;
}

/* ============================================================================
 * ui.query
 * ============================================================================ */

static cJSON* cd_mcp_handle_ui_query(
    struct cd_kernel_t* kernel, const cJSON* params,
    int* error_code, const char** error_msg)
{
    (void)params;
    cd_ui_system_t* ui = get_ui(kernel);
    if (!ui) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg = cd_mcp_error_fmt(
            "UI system not initialized",
            "The ui_runtime plugin is not loaded or failed to initialize.",
            "Ensure the ui_runtime plugin is listed in project.toml and loaded before calling UI tools.");
        return NULL;
    }

    cJSON* result = cJSON_CreateObject();

    /* Screens array */
    cJSON* screens_arr = cJSON_AddArrayToObject(result, "screens");
    for (uint32_t i = 0; i < ui->screen_count; i++) {
        if (!ui->screens[i].active) continue;
        cJSON* s = cJSON_CreateObject();
        cJSON_AddStringToObject(s, "name", ui->screens[i].name);
        cJSON_AddBoolToObject(s, "visible", ui->screens[i].visible);
        cJSON_AddItemToArray(screens_arr, s);
    }

    /* Elements array */
    cJSON* elems_arr = cJSON_AddArrayToObject(result, "elements");
    for (uint32_t i = 0; i < ui->element_count; i++) {
        cd_ui_element_t* e = &ui->elements[i];
        if (!e->active) continue;

        cJSON* el = cJSON_CreateObject();
        cJSON_AddNumberToObject(el, "id", (double)i);
        cJSON_AddStringToObject(el, "type", type_to_string(e->type));
        cJSON_AddBoolToObject(el, "visible", e->visible);
        cJSON_AddStringToObject(el, "anchor", anchor_to_string(e->anchor));
        cJSON_AddNumberToObject(el, "x", (double)e->offset_x);
        cJSON_AddNumberToObject(el, "y", (double)e->offset_y);
        cJSON_AddNumberToObject(el, "width", (double)e->width);
        cJSON_AddNumberToObject(el, "height", (double)e->height);
        cJSON_AddNumberToObject(el, "font_size", (double)e->font_size);

        if (e->text[0] != '\0')
            cJSON_AddStringToObject(el, "text", e->text);

        if (e->type == CD_UI_PROGRESS)
            cJSON_AddNumberToObject(el, "progress", (double)e->progress);

        /* Screen name */
        if (e->screen_index < ui->screen_count && ui->screens[e->screen_index].active)
            cJSON_AddStringToObject(el, "screen", ui->screens[e->screen_index].name);

        /* Interactive state */
        cJSON_AddBoolToObject(el, "hovered", e->hovered);
        cJSON_AddBoolToObject(el, "pressed", e->pressed);
        cJSON_AddBoolToObject(el, "clicked", e->clicked);

        cJSON_AddItemToArray(elems_arr, el);
    }

    return result;
}

/* ============================================================================
 * ui.show_screen / ui.hide_screen
 * ============================================================================ */

static cJSON* cd_mcp_handle_ui_show_screen(
    struct cd_kernel_t* kernel, const cJSON* params,
    int* error_code, const char** error_msg)
{
    cd_ui_system_t* ui = get_ui(kernel);
    if (!ui) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg = cd_mcp_error_fmt(
            "UI system not initialized",
            "The ui_runtime plugin is not loaded or failed to initialize.",
            "Ensure the ui_runtime plugin is listed in project.toml and loaded before calling UI tools.");
        return NULL;
    }

    const cJSON* name_item = cJSON_GetObjectItemCaseSensitive(params, "name");
    if (!name_item || !cJSON_IsString(name_item)) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg = cd_mcp_error_fmt(
            "Missing required parameter: name",
            "ui.show_screen requires a 'name' string identifying the screen to show.",
            "Use ui.query to list available screens, or ui.create_screen to create one first.");
        return NULL;
    }

    cd_ui_screen_show(ui, name_item->valuestring);

    cJSON* result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "screen", name_item->valuestring);
    cJSON_AddBoolToObject(result, "visible", 1);
    return result;
}

static cJSON* cd_mcp_handle_ui_hide_screen(
    struct cd_kernel_t* kernel, const cJSON* params,
    int* error_code, const char** error_msg)
{
    cd_ui_system_t* ui = get_ui(kernel);
    if (!ui) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg = cd_mcp_error_fmt(
            "UI system not initialized",
            "The ui_runtime plugin is not loaded or failed to initialize.",
            "Ensure the ui_runtime plugin is listed in project.toml and loaded before calling UI tools.");
        return NULL;
    }

    const cJSON* name_item = cJSON_GetObjectItemCaseSensitive(params, "name");
    if (!name_item || !cJSON_IsString(name_item)) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg = cd_mcp_error_fmt(
            "Missing required parameter: name",
            "ui.hide_screen requires a 'name' string identifying the screen to hide.",
            "Use ui.query to list available screens.");
        return NULL;
    }

    cd_ui_screen_hide(ui, name_item->valuestring);

    cJSON* result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "screen", name_item->valuestring);
    cJSON_AddBoolToObject(result, "visible", 0);
    return result;
}

/* ============================================================================
 * Registration
 * ============================================================================ */

cd_result_t cd_mcp_register_ui_tools(cd_mcp_server_t* server) {
    if (!server) return CD_ERR_NULL;

    cd_result_t res;

    res = cd_mcp_register_tool_ex(server, "ui.create_screen",
        cd_mcp_handle_ui_create_screen,
        "Create a named UI screen",
        "{\"type\":\"object\",\"properties\":{"
        "\"name\":{\"type\":\"string\",\"description\":\"Screen name identifier\"}"
        "},\"required\":[\"name\"]}");
    if (res != CD_OK) return res;

    res = cd_mcp_register_tool_ex(server, "ui.create_element",
        cd_mcp_handle_ui_create_element,
        "Create a UI element on a screen with type and layout properties",
        "{\"type\":\"object\",\"properties\":{"
        "\"type\":{\"type\":\"string\",\"description\":\"Element type: label, panel, button, or progress\"},"
        "\"screen\":{\"type\":\"string\",\"description\":\"Target screen name (default: hud)\"},"
        "\"text\":{\"type\":\"string\",\"description\":\"Text content for label/button\"},"
        "\"x\":{\"type\":\"number\",\"description\":\"X position\"},"
        "\"y\":{\"type\":\"number\",\"description\":\"Y position\"},"
        "\"width\":{\"type\":\"number\",\"description\":\"Element width\"},"
        "\"height\":{\"type\":\"number\",\"description\":\"Element height\"},"
        "\"font_size\":{\"type\":\"number\",\"description\":\"Font size for text\"},"
        "\"value\":{\"type\":\"number\",\"description\":\"Progress value (0-1) for progress type\"},"
        "\"anchor\":{\"type\":\"string\",\"description\":\"Anchor point (e.g. top_left, center)\"},"
        "\"visible\":{\"type\":\"boolean\",\"description\":\"Initial visibility\"}"
        "},\"required\":[\"type\"]}");
    if (res != CD_OK) return res;

    res = cd_mcp_register_tool_ex(server, "ui.update_element",
        cd_mcp_handle_ui_update_element,
        "Modify properties of an existing UI element",
        "{\"type\":\"object\",\"properties\":{"
        "\"id\":{\"type\":\"number\",\"description\":\"Element ID to update\"},"
        "\"text\":{\"type\":\"string\",\"description\":\"New text content\"},"
        "\"x\":{\"type\":\"number\",\"description\":\"New X position\"},"
        "\"y\":{\"type\":\"number\",\"description\":\"New Y position\"},"
        "\"width\":{\"type\":\"number\",\"description\":\"New width\"},"
        "\"height\":{\"type\":\"number\",\"description\":\"New height\"},"
        "\"visible\":{\"type\":\"boolean\",\"description\":\"Visibility state\"},"
        "\"anchor\":{\"type\":\"string\",\"description\":\"Anchor point\"},"
        "\"font_size\":{\"type\":\"number\",\"description\":\"Font size\"},"
        "\"value\":{\"type\":\"number\",\"description\":\"Progress value (0-1)\"}"
        "},\"required\":[\"id\"]}");
    if (res != CD_OK) return res;

    res = cd_mcp_register_tool_ex(server, "ui.remove_element",
        cd_mcp_handle_ui_remove_element,
        "Remove a UI element by ID",
        "{\"type\":\"object\",\"properties\":{"
        "\"id\":{\"type\":\"number\",\"description\":\"Element ID to remove\"}"
        "},\"required\":[\"id\"]}");
    if (res != CD_OK) return res;

    res = cd_mcp_register_tool_ex(server, "ui.query",
        cd_mcp_handle_ui_query,
        "Get all UI screens and elements with their current state",
        "{\"type\":\"object\",\"properties\":{}}");
    if (res != CD_OK) return res;

    res = cd_mcp_register_tool_ex(server, "ui.show_screen",
        cd_mcp_handle_ui_show_screen,
        "Make a UI screen visible",
        "{\"type\":\"object\",\"properties\":{"
        "\"name\":{\"type\":\"string\",\"description\":\"Screen name to show\"}"
        "},\"required\":[\"name\"]}");
    if (res != CD_OK) return res;

    res = cd_mcp_register_tool_ex(server, "ui.hide_screen",
        cd_mcp_handle_ui_hide_screen,
        "Hide a UI screen",
        "{\"type\":\"object\",\"properties\":{"
        "\"name\":{\"type\":\"string\",\"description\":\"Screen name to hide\"}"
        "},\"required\":[\"name\"]}");
    if (res != CD_OK) return res;

    return CD_OK;
}
