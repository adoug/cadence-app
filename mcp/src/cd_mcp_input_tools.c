/* cd_mcp_input_tools.c - Cadence Engine MCP input tool handlers
 *
 * Implements:
 *   - input.simulate : Inject raw input events for headless play-testing
 *   - input.action   : Trigger a high-level input action by name
 *
 * All handlers follow the cd_mcp_tool_handler_t signature.
 * These tools call cd_input_sim_*() functions which update state arrays
 * and emit events on the kernel event bus. Safe to call from MCP thread.
 *
 * Task 7.5.
 */

#include "cadence/cd_mcp_tools.h"
#include "cadence/cd_mcp.h"
#include "cadence/cd_kernel.h"
#include "cadence/cd_kernel_api.h"
#include "cadence/cd_input_sim_ext_api.h"
#include "cadence/cd_input_api.h"
#include "cJSON.h"

#include <string.h>
#include <ctype.h>

/* ============================================================================
 * GLFW key code constants
 *
 * Defined locally to avoid a build-time dependency on GLFW headers (which are
 * fetched via FetchContent and may not be available at include time for the
 * MCP library). Values match GLFW 3.x definitions.
 * ============================================================================ */

/* Printable keys */
#define CD_KEY_SPACE         32
#define CD_KEY_APOSTROPHE    39
#define CD_KEY_COMMA         44
#define CD_KEY_MINUS         45
#define CD_KEY_PERIOD        46
#define CD_KEY_SLASH         47
#define CD_KEY_0             48
#define CD_KEY_1             49
#define CD_KEY_2             50
#define CD_KEY_3             51
#define CD_KEY_4             52
#define CD_KEY_5             53
#define CD_KEY_6             54
#define CD_KEY_7             55
#define CD_KEY_8             56
#define CD_KEY_9             57
#define CD_KEY_SEMICOLON     59
#define CD_KEY_EQUAL         61
#define CD_KEY_A             65
#define CD_KEY_B             66
#define CD_KEY_C             67
#define CD_KEY_D             68
#define CD_KEY_E             69
#define CD_KEY_F             70
#define CD_KEY_G             71
#define CD_KEY_H             72
#define CD_KEY_I             73
#define CD_KEY_J             74
#define CD_KEY_K             75
#define CD_KEY_L             76
#define CD_KEY_M             77
#define CD_KEY_N             78
#define CD_KEY_O             79
#define CD_KEY_P             80
#define CD_KEY_Q             81
#define CD_KEY_R             82
#define CD_KEY_S             83
#define CD_KEY_T             84
#define CD_KEY_U             85
#define CD_KEY_V             86
#define CD_KEY_W             87
#define CD_KEY_X             88
#define CD_KEY_Y             89
#define CD_KEY_Z             90
#define CD_KEY_LEFT_BRACKET  91
#define CD_KEY_BACKSLASH     92
#define CD_KEY_RIGHT_BRACKET 93
#define CD_KEY_GRAVE_ACCENT  96

/* Function keys */
#define CD_KEY_ESCAPE        256
#define CD_KEY_ENTER         257
#define CD_KEY_TAB           258
#define CD_KEY_BACKSPACE     259
#define CD_KEY_INSERT        260
#define CD_KEY_DELETE        261
#define CD_KEY_RIGHT         262
#define CD_KEY_LEFT          263
#define CD_KEY_DOWN          264
#define CD_KEY_UP            265
#define CD_KEY_PAGE_UP       266
#define CD_KEY_PAGE_DOWN     267
#define CD_KEY_HOME          268
#define CD_KEY_END           269
#define CD_KEY_CAPS_LOCK     280
#define CD_KEY_SCROLL_LOCK   281
#define CD_KEY_NUM_LOCK      282
#define CD_KEY_PRINT_SCREEN  283
#define CD_KEY_PAUSE         284
#define CD_KEY_F1            290
#define CD_KEY_F2            291
#define CD_KEY_F3            292
#define CD_KEY_F4            293
#define CD_KEY_F5            294
#define CD_KEY_F6            295
#define CD_KEY_F7            296
#define CD_KEY_F8            297
#define CD_KEY_F9            298
#define CD_KEY_F10           299
#define CD_KEY_F11           300
#define CD_KEY_F12           301

/* Modifier keys */
#define CD_KEY_LEFT_SHIFT    340
#define CD_KEY_LEFT_CONTROL  341
#define CD_KEY_LEFT_ALT      342
#define CD_KEY_LEFT_SUPER    343
#define CD_KEY_RIGHT_SHIFT   344
#define CD_KEY_RIGHT_CONTROL 345
#define CD_KEY_RIGHT_ALT     346
#define CD_KEY_RIGHT_SUPER   347
#define CD_KEY_MENU          348

/* Mouse button indices (GLFW-compatible) */
#define CD_MOUSE_BUTTON_LEFT   0
#define CD_MOUSE_BUTTON_RIGHT  1
#define CD_MOUSE_BUTTON_MIDDLE 2

/* ============================================================================
 * Key name to key code lookup table
 * ============================================================================ */

typedef struct {
    const char* name;
    int         code;
} cd_key_mapping_t;

static const cd_key_mapping_t g_key_mappings[] = {
    /* Letters (uppercase names, matched case-insensitively) */
    { "A", CD_KEY_A }, { "B", CD_KEY_B }, { "C", CD_KEY_C },
    { "D", CD_KEY_D }, { "E", CD_KEY_E }, { "F", CD_KEY_F },
    { "G", CD_KEY_G }, { "H", CD_KEY_H }, { "I", CD_KEY_I },
    { "J", CD_KEY_J }, { "K", CD_KEY_K }, { "L", CD_KEY_L },
    { "M", CD_KEY_M }, { "N", CD_KEY_N }, { "O", CD_KEY_O },
    { "P", CD_KEY_P }, { "Q", CD_KEY_Q }, { "R", CD_KEY_R },
    { "S", CD_KEY_S }, { "T", CD_KEY_T }, { "U", CD_KEY_U },
    { "V", CD_KEY_V }, { "W", CD_KEY_W }, { "X", CD_KEY_X },
    { "Y", CD_KEY_Y }, { "Z", CD_KEY_Z },

    /* Digits */
    { "0", CD_KEY_0 }, { "1", CD_KEY_1 }, { "2", CD_KEY_2 },
    { "3", CD_KEY_3 }, { "4", CD_KEY_4 }, { "5", CD_KEY_5 },
    { "6", CD_KEY_6 }, { "7", CD_KEY_7 }, { "8", CD_KEY_8 },
    { "9", CD_KEY_9 },

    /* Special keys */
    { "Space",       CD_KEY_SPACE },
    { "Enter",       CD_KEY_ENTER },
    { "Return",      CD_KEY_ENTER },
    { "Escape",      CD_KEY_ESCAPE },
    { "Esc",         CD_KEY_ESCAPE },
    { "Tab",         CD_KEY_TAB },
    { "Backspace",   CD_KEY_BACKSPACE },
    { "Insert",      CD_KEY_INSERT },
    { "Delete",      CD_KEY_DELETE },
    { "Home",        CD_KEY_HOME },
    { "End",         CD_KEY_END },
    { "PageUp",      CD_KEY_PAGE_UP },
    { "PageDown",    CD_KEY_PAGE_DOWN },
    { "CapsLock",    CD_KEY_CAPS_LOCK },
    { "ScrollLock",  CD_KEY_SCROLL_LOCK },
    { "NumLock",     CD_KEY_NUM_LOCK },
    { "PrintScreen", CD_KEY_PRINT_SCREEN },
    { "Pause",       CD_KEY_PAUSE },
    { "Menu",        CD_KEY_MENU },

    /* Arrow keys */
    { "Left",  CD_KEY_LEFT },
    { "Right", CD_KEY_RIGHT },
    { "Up",    CD_KEY_UP },
    { "Down",  CD_KEY_DOWN },

    /* Modifier keys */
    { "LeftShift",     CD_KEY_LEFT_SHIFT },
    { "RightShift",    CD_KEY_RIGHT_SHIFT },
    { "LeftControl",   CD_KEY_LEFT_CONTROL },
    { "LeftCtrl",      CD_KEY_LEFT_CONTROL },
    { "RightControl",  CD_KEY_RIGHT_CONTROL },
    { "RightCtrl",     CD_KEY_RIGHT_CONTROL },
    { "LeftAlt",       CD_KEY_LEFT_ALT },
    { "RightAlt",      CD_KEY_RIGHT_ALT },
    { "LeftSuper",     CD_KEY_LEFT_SUPER },
    { "RightSuper",    CD_KEY_RIGHT_SUPER },

    /* Function keys */
    { "F1",  CD_KEY_F1 },  { "F2",  CD_KEY_F2 },
    { "F3",  CD_KEY_F3 },  { "F4",  CD_KEY_F4 },
    { "F5",  CD_KEY_F5 },  { "F6",  CD_KEY_F6 },
    { "F7",  CD_KEY_F7 },  { "F8",  CD_KEY_F8 },
    { "F9",  CD_KEY_F9 },  { "F10", CD_KEY_F10 },
    { "F11", CD_KEY_F11 }, { "F12", CD_KEY_F12 },

    /* Punctuation */
    { "Apostrophe",   CD_KEY_APOSTROPHE },
    { "Comma",        CD_KEY_COMMA },
    { "Minus",        CD_KEY_MINUS },
    { "Period",       CD_KEY_PERIOD },
    { "Slash",        CD_KEY_SLASH },
    { "Semicolon",    CD_KEY_SEMICOLON },
    { "Equal",        CD_KEY_EQUAL },
    { "LeftBracket",  CD_KEY_LEFT_BRACKET },
    { "Backslash",    CD_KEY_BACKSLASH },
    { "RightBracket", CD_KEY_RIGHT_BRACKET },
    { "GraveAccent",  CD_KEY_GRAVE_ACCENT },

    { NULL, 0 } /* sentinel */
};

/**
 * Case-insensitive string comparison.
 */
static int cd_strcasecmp(const char* a, const char* b) {
    while (*a && *b) {
        int ca = tolower((unsigned char)*a);
        int cb = tolower((unsigned char)*b);
        if (ca != cb) return ca - cb;
        a++;
        b++;
    }
    return tolower((unsigned char)*a) - tolower((unsigned char)*b);
}

/**
 * Look up a GLFW key code from a string name.
 *
 * @param name  Key name string (case-insensitive).
 * @return Key code on success, -1 if not found.
 */
static int cd_key_from_name(const char* name) {
    if (name == NULL || name[0] == '\0') return -1;

    for (const cd_key_mapping_t* m = g_key_mappings; m->name != NULL; m++) {
        if (cd_strcasecmp(m->name, name) == 0) {
            return m->code;
        }
    }

    return -1;
}

/**
 * Look up a mouse button index from a string name.
 *
 * @param name  Button name string ("left", "right", "middle").
 * @return Button index on success, -1 if not found.
 */
static int cd_mouse_button_from_name(const char* name) {
    if (name == NULL || name[0] == '\0') return -1;

    if (cd_strcasecmp(name, "left") == 0)   return CD_MOUSE_BUTTON_LEFT;
    if (cd_strcasecmp(name, "right") == 0)  return CD_MOUSE_BUTTON_RIGHT;
    if (cd_strcasecmp(name, "middle") == 0) return CD_MOUSE_BUTTON_MIDDLE;

    return -1;
}

/**
 * Look up a gamepad axis index from a string name.
 *
 * @param name  Axis name string.
 * @return Axis index on success, -1 if not found.
 */
static int cd_gamepad_axis_from_name(const char* name) {
    if (name == NULL || name[0] == '\0') return -1;

    if (cd_strcasecmp(name, "left_x") == 0)        return 0;
    if (cd_strcasecmp(name, "left_y") == 0)         return 1;
    if (cd_strcasecmp(name, "right_x") == 0)        return 2;
    if (cd_strcasecmp(name, "right_y") == 0)        return 3;
    if (cd_strcasecmp(name, "left_trigger") == 0)   return 4;
    if (cd_strcasecmp(name, "right_trigger") == 0)  return 5;

    return -1;
}

/* ============================================================================
 * input.simulate handler
 *
 * Input:
 *   { "actions": [
 *       { "type": "key_press", "key": "W" },
 *       { "type": "key_release", "key": "W" },
 *       { "type": "mouse_move", "dx": 100, "dy": -50 },
 *       { "type": "mouse_move", "x": 400, "y": 300 },
 *       { "type": "mouse_click", "button": "left" },
 *       { "type": "mouse_press", "button": "left" },
 *       { "type": "mouse_release", "button": "left" },
 *       { "type": "mouse_scroll", "dx": 0, "dy": 1.0 },
 *       { "type": "gamepad_button_press", "gamepad": 0, "button": 0 },
 *       { "type": "gamepad_button_release", "gamepad": 0, "button": 0 },
 *       { "type": "gamepad_axis", "gamepad": 0, "axis": "left_x", "value": 0.8 }
 *   ]}
 *
 * Output:
 *   { "processed": 5 }
 *
 * Error cases:
 *   -32602 INVALID_PARAMS: missing actions array, invalid action type
 *   -32603 INTERNAL_ERROR: simulation function failure
 * ============================================================================ */

static cJSON* cd_mcp_handle_input_simulate(
    struct cd_kernel_t* kernel,
    const cJSON*        params,
    int*                error_code,
    const char**        error_msg)
{
    /* BL-83: Access input simulation through kernel vtable */
    const cd_input_sim_ext_api_t* isim = cd_kernel_get_input_sim_ext_api(kernel);
    if (!isim) {
        *error_code = -32603;
        *error_msg  = "Input simulation plugin not loaded";
        return NULL;
    }

    if (params == NULL) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = "Missing parameters";
        return NULL;
    }

    const cJSON* actions = cJSON_GetObjectItemCaseSensitive(params, "actions");
    if (actions == NULL || !cJSON_IsArray(actions)) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = "Missing or invalid 'actions' array";
        return NULL;
    }

    int processed = 0;
    const cJSON* action = NULL;

    cJSON_ArrayForEach(action, actions) {
        if (!cJSON_IsObject(action)) continue;

        const cJSON* type_item = cJSON_GetObjectItemCaseSensitive(action, "type");
        if (type_item == NULL || !cJSON_IsString(type_item) ||
            type_item->valuestring == NULL) {
            continue;
        }

        const char* type = type_item->valuestring;
        cd_result_t res = CD_OK;

        if (cd_strcasecmp(type, "key_press") == 0) {
            /* Key press: { "type": "key_press", "key": "W" } */
            const cJSON* key_item = cJSON_GetObjectItemCaseSensitive(action, "key");
            if (key_item == NULL || !cJSON_IsString(key_item)) continue;

            int key_code = cd_key_from_name(key_item->valuestring);
            if (key_code < 0) continue;

            res = isim->key(key_code, true, isim->userdata);

        } else if (cd_strcasecmp(type, "key_release") == 0) {
            /* Key release: { "type": "key_release", "key": "W" } */
            const cJSON* key_item = cJSON_GetObjectItemCaseSensitive(action, "key");
            if (key_item == NULL || !cJSON_IsString(key_item)) continue;

            int key_code = cd_key_from_name(key_item->valuestring);
            if (key_code < 0) continue;

            res = isim->key(key_code, false, isim->userdata);

        } else if (cd_strcasecmp(type, "mouse_move") == 0) {
            /* Mouse move: supports absolute (x, y) or relative (dx, dy).
             * For relative, we treat dx/dy as the new absolute position since
             * cd_input_sim_mouse_move takes absolute coords. The dx/dy
             * delta is computed internally by the sim plugin. */
            const cJSON* x_item  = cJSON_GetObjectItemCaseSensitive(action, "x");
            const cJSON* y_item  = cJSON_GetObjectItemCaseSensitive(action, "y");
            const cJSON* dx_item = cJSON_GetObjectItemCaseSensitive(action, "dx");
            const cJSON* dy_item = cJSON_GetObjectItemCaseSensitive(action, "dy");

            double x = 0.0, y = 0.0;

            if (x_item != NULL && cJSON_IsNumber(x_item) &&
                y_item != NULL && cJSON_IsNumber(y_item)) {
                /* Absolute position */
                x = x_item->valuedouble;
                y = y_item->valuedouble;
            } else if (dx_item != NULL && cJSON_IsNumber(dx_item) &&
                       dy_item != NULL && cJSON_IsNumber(dy_item)) {
                /* Relative movement — read current position and offset.
                 * cd_input_sim_mouse_move() takes absolute coords, so we
                 * must add the delta to the current cursor position. */
                const cd_input_api_t* api = isim->get_api(isim->userdata);
                double cur_x = 0.0, cur_y = 0.0;
                if (api && api->get_mouse_pos) {
                    api->get_mouse_pos(&cur_x, &cur_y, api->userdata);
                }
                x = cur_x + dx_item->valuedouble;
                y = cur_y + dy_item->valuedouble;
            } else {
                continue;
            }

            res = isim->mouse_move(x, y, isim->userdata);

        } else if (cd_strcasecmp(type, "mouse_click") == 0) {
            /* Mouse click: press then auto-release */
            const cJSON* btn_item = cJSON_GetObjectItemCaseSensitive(action, "button");
            if (btn_item == NULL || !cJSON_IsString(btn_item)) continue;

            int btn = cd_mouse_button_from_name(btn_item->valuestring);
            if (btn < 0) continue;

            res = isim->mouse_button(btn, true, isim->userdata);
            if (res == CD_OK) {
                res = isim->mouse_button(btn, false, isim->userdata);
            }

        } else if (cd_strcasecmp(type, "mouse_press") == 0) {
            /* Mouse button press (no auto-release) */
            const cJSON* btn_item = cJSON_GetObjectItemCaseSensitive(action, "button");
            if (btn_item == NULL || !cJSON_IsString(btn_item)) continue;

            int btn = cd_mouse_button_from_name(btn_item->valuestring);
            if (btn < 0) continue;

            res = isim->mouse_button(btn, true, isim->userdata);

        } else if (cd_strcasecmp(type, "mouse_release") == 0) {
            /* Mouse button release */
            const cJSON* btn_item = cJSON_GetObjectItemCaseSensitive(action, "button");
            if (btn_item == NULL || !cJSON_IsString(btn_item)) continue;

            int btn = cd_mouse_button_from_name(btn_item->valuestring);
            if (btn < 0) continue;

            res = isim->mouse_button(btn, false, isim->userdata);

        } else if (cd_strcasecmp(type, "mouse_scroll") == 0) {
            /* Mouse scroll: { "type": "mouse_scroll", "dx": 0, "dy": 1.0 } */
            const cJSON* dx_item = cJSON_GetObjectItemCaseSensitive(action, "dx");
            const cJSON* dy_item = cJSON_GetObjectItemCaseSensitive(action, "dy");

            double dx = 0.0, dy = 0.0;
            if (dx_item != NULL && cJSON_IsNumber(dx_item)) dx = dx_item->valuedouble;
            if (dy_item != NULL && cJSON_IsNumber(dy_item)) dy = dy_item->valuedouble;

            res = isim->mouse_scroll(dx, dy, isim->userdata);

        } else if (cd_strcasecmp(type, "gamepad_button_press") == 0) {
            /* Gamepad button press */
            const cJSON* gp_item  = cJSON_GetObjectItemCaseSensitive(action, "gamepad");
            const cJSON* btn_item = cJSON_GetObjectItemCaseSensitive(action, "button");

            int32_t gamepad = 0;
            if (gp_item != NULL && cJSON_IsNumber(gp_item)) {
                gamepad = (int32_t)gp_item->valuedouble;
            }

            int32_t button = 0;
            if (btn_item != NULL && cJSON_IsNumber(btn_item)) {
                button = (int32_t)btn_item->valuedouble;
            } else {
                continue;
            }

            res = isim->gamepad_button(gamepad, button, true, isim->userdata);

        } else if (cd_strcasecmp(type, "gamepad_button_release") == 0) {
            /* Gamepad button release */
            const cJSON* gp_item  = cJSON_GetObjectItemCaseSensitive(action, "gamepad");
            const cJSON* btn_item = cJSON_GetObjectItemCaseSensitive(action, "button");

            int32_t gamepad = 0;
            if (gp_item != NULL && cJSON_IsNumber(gp_item)) {
                gamepad = (int32_t)gp_item->valuedouble;
            }

            int32_t button = 0;
            if (btn_item != NULL && cJSON_IsNumber(btn_item)) {
                button = (int32_t)btn_item->valuedouble;
            } else {
                continue;
            }

            res = isim->gamepad_button(gamepad, button, false, isim->userdata);

        } else if (cd_strcasecmp(type, "gamepad_axis") == 0) {
            /* Gamepad axis: { "type": "gamepad_axis", "gamepad": 0,
             *                 "axis": "left_x", "value": 0.8 } */
            const cJSON* gp_item    = cJSON_GetObjectItemCaseSensitive(action, "gamepad");
            const cJSON* axis_item  = cJSON_GetObjectItemCaseSensitive(action, "axis");
            const cJSON* value_item = cJSON_GetObjectItemCaseSensitive(action, "value");

            int32_t gamepad = 0;
            if (gp_item != NULL && cJSON_IsNumber(gp_item)) {
                gamepad = (int32_t)gp_item->valuedouble;
            }

            int axis = -1;
            if (axis_item != NULL && cJSON_IsString(axis_item)) {
                axis = cd_gamepad_axis_from_name(axis_item->valuestring);
            } else if (axis_item != NULL && cJSON_IsNumber(axis_item)) {
                axis = (int)axis_item->valuedouble;
            }
            if (axis < 0) continue;

            float value = 0.0f;
            if (value_item != NULL && cJSON_IsNumber(value_item)) {
                value = (float)value_item->valuedouble;
            }

            res = isim->gamepad_axis(gamepad, axis, value, isim->userdata);

        } else {
            /* Unknown action type -- skip */
            continue;
        }

        if (res == CD_OK) {
            processed++;
        }
    }

    /* Build response */
    cJSON* result = cJSON_CreateObject();
    if (result == NULL) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Failed to allocate JSON response";
        return NULL;
    }

    cJSON_AddNumberToObject(result, "processed", (double)processed);
    return result;
}

/* ============================================================================
 * input.action handler
 *
 * Input:
 *   { "action": "move_forward", "value": 1.0, "duration_ms": 2000 }
 *
 * Output:
 *   { "action": "move_forward", "value": 1.0 }
 *
 * Error cases:
 *   -32602 INVALID_PARAMS: missing action name
 *   -32603 INTERNAL_ERROR: sim function failure
 * ============================================================================ */

static cJSON* cd_mcp_handle_input_action(
    struct cd_kernel_t* kernel,
    const cJSON*        params,
    int*                error_code,
    const char**        error_msg)
{
    const cd_input_sim_ext_api_t* isim = cd_kernel_get_input_sim_ext_api(kernel);
    if (!isim) {
        *error_code = -32603;
        *error_msg  = "Input simulation plugin not loaded";
        return NULL;
    }

    if (params == NULL) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = "Missing parameters";
        return NULL;
    }

    /* Extract "action" (required) */
    const cJSON* action_item = cJSON_GetObjectItemCaseSensitive(params, "action");
    if (action_item == NULL || !cJSON_IsString(action_item) ||
        action_item->valuestring == NULL || action_item->valuestring[0] == '\0') {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = "Missing or empty required parameter: action";
        return NULL;
    }
    const char* action_name = action_item->valuestring;

    /* Extract "value" (optional, default 1.0) */
    float value = 1.0f;
    const cJSON* value_item = cJSON_GetObjectItemCaseSensitive(params, "value");
    if (value_item != NULL && cJSON_IsNumber(value_item)) {
        value = (float)value_item->valuedouble;
    }

    /* duration_ms is noted for future use but not actively waited on */

    /* Trigger the action */
    cd_result_t res = isim->action(action_name, value > 0.5f, isim->userdata);
    if (res != CD_OK) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Failed to trigger input action";
        return NULL;
    }

    /* Build response */
    cJSON* result = cJSON_CreateObject();
    if (result == NULL) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Failed to allocate JSON response";
        return NULL;
    }

    cJSON_AddStringToObject(result, "action", action_name);
    cJSON_AddNumberToObject(result, "value", (double)value);
    return result;
}

/* ============================================================================
 * Registration
 * ============================================================================ */

cd_result_t cd_mcp_register_input_tools(cd_mcp_server_t* server) {
    if (server == NULL) {
        return CD_ERR_NULL;
    }

    cd_result_t res;

    res = cd_mcp_register_tool_ex(server, "input.simulate", cd_mcp_handle_input_simulate,
        "Inject raw input events (key, mouse, gamepad) for headless play-testing",
        "{\"type\":\"object\",\"properties\":{"
        "\"actions\":{\"type\":\"array\",\"items\":{\"type\":\"object\"},\"description\":\"Array of input actions with type and parameters\"}"
        "},\"required\":[\"actions\"]}");
    if (res != CD_OK) return res;

    res = cd_mcp_register_tool_ex(server, "input.action", cd_mcp_handle_input_action,
        "Trigger a high-level input action by name",
        "{\"type\":\"object\",\"properties\":{"
        "\"action\":{\"type\":\"string\",\"description\":\"Name of the input action to trigger\"},"
        "\"value\":{\"type\":\"number\",\"description\":\"Action intensity value (default 1.0)\"},"
        "\"duration_ms\":{\"type\":\"number\",\"description\":\"Duration in milliseconds (reserved for future use)\"}"
        "},\"required\":[\"action\"]}");
    if (res != CD_OK) return res;

    return CD_OK;
}
