/* cd_mcp_audio_bus_tools.c - Cadence Engine MCP audio bus tools (BL-8)
 *
 * Implements:
 *   - audio.bus.list       : List all buses with volumes/states
 *   - audio.bus.set_volume : Set bus volume by name
 *   - audio.bus.mute       : Mute/unmute a bus
 *   - audio.bus.solo       : Solo/unsolo a bus
 */

#include "cadence/cd_mcp_tools.h"
#include "cadence/cd_mcp.h"
#include "cadence/cd_mcp_error.h"
#include "cadence/cd_kernel.h"
#include "cadence/cd_kernel_api.h"
#include "cadence/cd_audio_api.h"
#include "cJSON.h"

#include <string.h>
#include <stdio.h>

/* ============================================================================
 * audio.bus.list
 * ============================================================================ */

static cJSON* handle_audio_bus_list(cd_kernel_t* kernel,
                                     const cJSON* params,
                                     int* error_code,
                                     const char** error_msg) {
    (void)params;
    if (!kernel) {
        *error_code = -32603;
        *error_msg = cd_mcp_error_fmt("Kernel not available",
            "Audio bus tools require the engine to be initialized.", NULL);
        return NULL;
    }

    cd_audio_mixer_t* mixer = cd_kernel_get_audio_mixer(kernel);
    cJSON* result = cJSON_CreateObject();
    cJSON* buses = cJSON_AddArrayToObject(result, "buses");

    for (int i = 0; i < CD_AUDIO_MAX_BUSES; i++) {
        if (!mixer->buses[i].active) continue;
        cJSON* bus = cJSON_CreateObject();
        cJSON_AddNumberToObject(bus, "index", i);
        cJSON_AddStringToObject(bus, "name", mixer->buses[i].name);
        cJSON_AddNumberToObject(bus, "volume", (double)mixer->buses[i].volume);
        cJSON_AddBoolToObject(bus, "mute", mixer->buses[i].mute);
        cJSON_AddBoolToObject(bus, "solo", mixer->buses[i].solo);
        cJSON_AddNumberToObject(bus, "parent", mixer->buses[i].parent);
        cJSON_AddNumberToObject(bus, "effective_volume",
            (double)cd_audio_mixer_effective_volume(mixer, i));
        cJSON_AddNumberToObject(bus, "lowpass_cutoff",
            (double)mixer->buses[i].effects.lowpass_cutoff);
        cJSON_AddBoolToObject(bus, "fade_active",
            mixer->buses[i].effects.fade_active);
        cJSON_AddItemToArray(buses, bus);
    }

    cJSON_AddNumberToObject(result, "bus_count", (double)mixer->bus_count);
    cJSON_AddBoolToObject(result, "any_solo", mixer->any_solo);

    return result;
}

/* ============================================================================
 * audio.bus.set_volume
 * ============================================================================ */

static cJSON* handle_audio_bus_set_volume(cd_kernel_t* kernel,
                                           const cJSON* params,
                                           int* error_code,
                                           const char** error_msg) {
    if (!kernel) {
        *error_code = -32603;
        *error_msg = cd_mcp_error_fmt("Kernel not available",
            "Audio bus tools require the engine to be initialized.", NULL);
        return NULL;
    }

    const cJSON* j_name = cJSON_GetObjectItem(params, "bus");
    const cJSON* j_vol  = cJSON_GetObjectItem(params, "volume");

    if (!j_name || !cJSON_IsString(j_name)) {
        *error_code = -32602;
        *error_msg = cd_mcp_error_fmt("Missing required parameter: bus",
            "audio.bus.set_volume requires a 'bus' string parameter.",
            "Example: {\"bus\": \"master\", \"volume\": 0.8}");
        return NULL;
    }
    if (!j_vol || !cJSON_IsNumber(j_vol)) {
        *error_code = -32602;
        *error_msg = cd_mcp_error_fmt("Missing required parameter: volume",
            "audio.bus.set_volume requires a 'volume' number (0.0 to 1.0).",
            "Example: {\"bus\": \"master\", \"volume\": 0.8}");
        return NULL;
    }

    int idx = cd_audio_mixer_find_bus(cd_kernel_get_audio_mixer(kernel), j_name->valuestring);
    if (idx < 0) {
        char detail[256];
        snprintf(detail, sizeof(detail),
            "No bus named '%s' exists.", j_name->valuestring);
        *error_code = -32602;
        *error_msg = cd_mcp_error_fmt("Bus not found", detail,
            "Use audio.bus.list to see available bus names.");
        return NULL;
    }

    cd_audio_mixer_set_volume(cd_kernel_get_audio_mixer(kernel), idx, (float)j_vol->valuedouble);

    cJSON* result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "bus", j_name->valuestring);
    cJSON_AddNumberToObject(result, "volume",
        (double)cd_audio_mixer_get_volume(cd_kernel_get_audio_mixer(kernel), idx));
    return result;
}

/* ============================================================================
 * audio.bus.mute
 * ============================================================================ */

static cJSON* handle_audio_bus_mute(cd_kernel_t* kernel,
                                     const cJSON* params,
                                     int* error_code,
                                     const char** error_msg) {
    if (!kernel) {
        *error_code = -32603;
        *error_msg = cd_mcp_error_fmt("Kernel not available",
            "Audio bus tools require the engine to be initialized.", NULL);
        return NULL;
    }

    const cJSON* j_name  = cJSON_GetObjectItem(params, "bus");
    const cJSON* j_muted = cJSON_GetObjectItem(params, "muted");

    if (!j_name || !cJSON_IsString(j_name)) {
        *error_code = -32602;
        *error_msg = cd_mcp_error_fmt("Missing required parameter: bus",
            "audio.bus.mute requires a 'bus' string parameter.",
            "Example: {\"bus\": \"music\", \"muted\": true}");
        return NULL;
    }
    if (!j_muted) {
        *error_code = -32602;
        *error_msg = cd_mcp_error_fmt("Missing required parameter: muted",
            "audio.bus.mute requires a 'muted' boolean parameter.",
            "Example: {\"bus\": \"music\", \"muted\": true}");
        return NULL;
    }

    int idx = cd_audio_mixer_find_bus(cd_kernel_get_audio_mixer(kernel), j_name->valuestring);
    if (idx < 0) {
        char detail[256];
        snprintf(detail, sizeof(detail),
            "No bus named '%s' exists.", j_name->valuestring);
        *error_code = -32602;
        *error_msg = cd_mcp_error_fmt("Bus not found", detail,
            "Use audio.bus.list to see available bus names.");
        return NULL;
    }

    cd_audio_mixer_set_muted(cd_kernel_get_audio_mixer(kernel), idx,
                              cJSON_IsTrue(j_muted) ? true : false);

    cJSON* result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "bus", j_name->valuestring);
    cJSON_AddBoolToObject(result, "muted", cd_kernel_get_audio_mixer(kernel)->buses[idx].mute);
    return result;
}

/* ============================================================================
 * audio.bus.solo
 * ============================================================================ */

static cJSON* handle_audio_bus_solo(cd_kernel_t* kernel,
                                     const cJSON* params,
                                     int* error_code,
                                     const char** error_msg) {
    if (!kernel) {
        *error_code = -32603;
        *error_msg = cd_mcp_error_fmt("Kernel not available",
            "Audio bus tools require the engine to be initialized.", NULL);
        return NULL;
    }

    const cJSON* j_name = cJSON_GetObjectItem(params, "bus");
    const cJSON* j_solo = cJSON_GetObjectItem(params, "solo");

    if (!j_name || !cJSON_IsString(j_name)) {
        *error_code = -32602;
        *error_msg = cd_mcp_error_fmt("Missing required parameter: bus",
            "audio.bus.solo requires a 'bus' string parameter.",
            "Example: {\"bus\": \"sfx\", \"solo\": true}");
        return NULL;
    }
    if (!j_solo) {
        *error_code = -32602;
        *error_msg = cd_mcp_error_fmt("Missing required parameter: solo",
            "audio.bus.solo requires a 'solo' boolean parameter.",
            "Example: {\"bus\": \"sfx\", \"solo\": true}");
        return NULL;
    }

    int idx = cd_audio_mixer_find_bus(cd_kernel_get_audio_mixer(kernel), j_name->valuestring);
    if (idx < 0) {
        char detail[256];
        snprintf(detail, sizeof(detail),
            "No bus named '%s' exists.", j_name->valuestring);
        *error_code = -32602;
        *error_msg = cd_mcp_error_fmt("Bus not found", detail,
            "Use audio.bus.list to see available bus names.");
        return NULL;
    }

    cd_audio_mixer_set_solo(cd_kernel_get_audio_mixer(kernel), idx,
                             cJSON_IsTrue(j_solo) ? true : false);

    cJSON* result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "bus", j_name->valuestring);
    cJSON_AddBoolToObject(result, "solo", cd_kernel_get_audio_mixer(kernel)->buses[idx].solo);
    cJSON_AddBoolToObject(result, "any_solo", cd_kernel_get_audio_mixer(kernel)->any_solo);
    return result;
}

/* ============================================================================
 * Registration
 * ============================================================================ */

cd_result_t cd_mcp_register_audio_bus_tools(cd_mcp_server_t* server) {
    if (!server) return CD_ERR_NULL;

    cd_mcp_register_tool_ex(server, "audio.bus.list", handle_audio_bus_list,
        "List all audio buses with volumes, mute, and solo states.",
        "{\"type\":\"object\",\"properties\":{}}");
    cd_mcp_register_tool_ex(server, "audio.bus.set_volume", handle_audio_bus_set_volume,
        "Set the volume of an audio bus by name.",
        "{\"type\":\"object\",\"properties\":{"
        "\"bus\":{\"type\":\"string\",\"description\":\"Bus name\"},"
        "\"volume\":{\"type\":\"number\",\"description\":\"Volume 0.0 to 1.0\"}"
        "},\"required\":[\"bus\",\"volume\"]}");
    cd_mcp_register_tool_ex(server, "audio.bus.mute", handle_audio_bus_mute,
        "Mute or unmute an audio bus by name.",
        "{\"type\":\"object\",\"properties\":{"
        "\"bus\":{\"type\":\"string\",\"description\":\"Bus name\"},"
        "\"muted\":{\"type\":\"boolean\"}"
        "},\"required\":[\"bus\",\"muted\"]}");
    cd_mcp_register_tool_ex(server, "audio.bus.solo", handle_audio_bus_solo,
        "Solo or unsolo an audio bus by name.",
        "{\"type\":\"object\",\"properties\":{"
        "\"bus\":{\"type\":\"string\",\"description\":\"Bus name\"},"
        "\"solo\":{\"type\":\"boolean\"}"
        "},\"required\":[\"bus\",\"solo\"]}");

    return CD_OK;
}
