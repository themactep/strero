/*
 * imp_control.c - IMP Control Module Implementation
 * Copyright (C) 2025 Paul Philippov, Thingino Project
 */

#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <json-c/json.h>
#include <sys/stat.h>

#include "imp_control.h"
#include "../../common.h"

#include "../http/http_router.h"
#include "../http/http_module.h"
#include "../../config.h"
#include "../../module_system.h"

/* IMP headers for T31X */
#include <imp/imp_system.h>
#include <imp/imp_isp.h>

#define TAG "IMP_CONTROL"

/* Static module settings - no need to configure these */
#define IMP_CONTROL_ENDPOINTS_ENABLED true
#define IMP_CONTROL_VALIDATE_RANGE true
#define IMP_CONTROL_LOG_CHANGES true

/* Module state */
static struct {
    bool initialized;
    bool running;
    imp_control_config_t config;  /* Contains only IMP parameters */
    imp_control_params_t current_params;
    pthread_mutex_t params_mutex;
} g_imp_control_state = {0};

/* Forward declarations - alphabetically ordered */
static int apply_imp_params(const imp_control_params_t* params);
static int get_current_imp_params(imp_control_params_t* params);
static int imp_control_config_parse(json_object* json, void* config);
static int save_params_to_config(const imp_control_params_t* params);
static void send_params_json_response(int client_socket, const imp_control_params_t* params, const char* message);

/* Apply IMP parameters to hardware */

/* Parse IMP control configuration from JSON */
static int imp_control_config_parse(json_object* json, void* config)
{
    if (!json || !config) {
        IMP_LOG_ERR(TAG, "Invalid config pointer");
        return -1;
    }

    imp_control_config_t* imp_config = (imp_control_config_t*)config;

    /* For modular config: JSON root is the imp_control config directly */
    /* For fallback config: Look for "imp_control" section in main config */
    json_object* imp_obj = json;

    /* Check if this is a fallback from main config (has "imp_control" section) */
    json_object* imp_section = NULL;
    if (json_object_object_get_ex(json, "imp_control", &imp_section)) {
        IMP_LOG_DBG(TAG, "Using imp_control section from main config (fallback mode)");
        imp_obj = imp_section;
    } else {
        IMP_LOG_DBG(TAG, "Using dedicated imp_control config file");
    }

    /* Set defaults first */
    imp_control_set_defaults(imp_config);

    /* Parse configuration fields */
    json_object* field = NULL;

    if (json_object_object_get_ex(imp_obj, "brightness", &field)) {
        imp_config->brightness = (unsigned char)json_object_get_int(field);
    }

    if (json_object_object_get_ex(imp_obj, "contrast", &field)) {
        imp_config->contrast = (unsigned char)json_object_get_int(field);
    }

    if (json_object_object_get_ex(imp_obj, "saturation", &field)) {
        imp_config->saturation = (unsigned char)json_object_get_int(field);
    }

    if (json_object_object_get_ex(imp_obj, "sharpness", &field)) {
        imp_config->sharpness = (unsigned char)json_object_get_int(field);
    }

    if (json_object_object_get_ex(imp_obj, "hue", &field)) {
        imp_config->hue = (unsigned char)json_object_get_int(field);
    }

    if (json_object_object_get_ex(imp_obj, "ae_compensation", &field)) {
        imp_config->ae_compensation = (unsigned char)json_object_get_int(field);
    }

    if (json_object_object_get_ex(imp_obj, "noise_reduction_2d", &field)) {
        imp_config->noise_reduction_2d = (unsigned char)json_object_get_int(field);
    }

    if (json_object_object_get_ex(imp_obj, "noise_reduction_3d", &field)) {
        imp_config->noise_reduction_3d = (unsigned char)json_object_get_int(field);
    }

    if (json_object_object_get_ex(imp_obj, "flip_horizontal", &field)) {
        imp_config->flip_horizontal = json_object_get_boolean(field);
    }

    if (json_object_object_get_ex(imp_obj, "flip_vertical", &field)) {
        imp_config->flip_vertical = json_object_get_boolean(field);
    }

    if (json_object_object_get_ex(imp_obj, "day_night_mode", &field)) {
        const char* mode_str = json_object_get_string(field);
        imp_config->day_night_mode = imp_control_string_to_day_night_mode(mode_str);
    }

    /* Parse Priority 2 extensions */
    if (json_object_object_get_ex(imp_obj, "anti_flicker", &field)) {
        const char* mode_str = json_object_get_string(field);
        imp_config->anti_flicker = imp_control_string_to_antiflicker_mode(mode_str);
    }

    if (json_object_object_get_ex(imp_obj, "backlight_compensation", &field)) {
        imp_config->backlight_compensation = (unsigned char)json_object_get_int(field);
    }

    if (json_object_object_get_ex(imp_obj, "highlight_suppression", &field)) {
        imp_config->highlight_suppression = (unsigned char)json_object_get_int(field);
    }

    if (json_object_object_get_ex(imp_obj, "white_balance_mode", &field)) {
        const char* mode_str = json_object_get_string(field);
        imp_config->white_balance_mode = imp_control_string_to_wb_mode(mode_str);
    }

    if (json_object_object_get_ex(imp_obj, "white_balance_r_gain", &field)) {
        imp_config->white_balance_r_gain = (unsigned short)json_object_get_int(field);
    }

    if (json_object_object_get_ex(imp_obj, "white_balance_b_gain", &field)) {
        imp_config->white_balance_b_gain = (unsigned short)json_object_get_int(field);
    }

    if (json_object_object_get_ex(imp_obj, "drc_strength", &field)) {
        imp_config->drc_strength = (unsigned char)json_object_get_int(field);
    }

    if (json_object_object_get_ex(imp_obj, "defog_strength", &field)) {
        imp_config->defog_strength = (unsigned char)json_object_get_int(field);
    }

    return 0;
}

/* Apply IMP parameters to hardware */
static int apply_imp_params(const imp_control_params_t* params)
{
    int ret;

    IMP_LOG_INFO(TAG, "Applying IMP parameters: brightness=%d, contrast=%d, saturation=%d, sharpness=%d, hue=%d, ae_comp=%d, nr2d=%d, nr3d=%d, hflip=%d, vflip=%d, mode=%d, antiflicker=%d, backlight=%d, highlight=%d, wb_mode=%d, wb_r=%d, wb_b=%d, drc=%d, defog=%d",
                 params->brightness, params->contrast, params->saturation, params->sharpness,
                 params->hue, params->ae_compensation, params->noise_reduction_2d, params->noise_reduction_3d,
                 params->flip_horizontal, params->flip_vertical, params->day_night_mode,
                 params->anti_flicker, params->backlight_compensation, params->highlight_suppression,
                 params->white_balance_mode, params->white_balance_r_gain, params->white_balance_b_gain,
                 params->drc_strength, params->defog_strength);

    /* Set basic image quality parameters */
    ret = IMP_ISP_Tuning_SetBrightness(params->brightness);
    if (ret < 0) {
        IMP_LOG_ERR(TAG, "Failed to set brightness to %d: %d", params->brightness, ret);
        return -1;
    }

    ret = IMP_ISP_Tuning_SetContrast(params->contrast);
    if (ret < 0) {
        IMP_LOG_ERR(TAG, "Failed to set contrast to %d: %d", params->contrast, ret);
        return -1;
    }

    ret = IMP_ISP_Tuning_SetSaturation(params->saturation);
    if (ret < 0) {
        IMP_LOG_ERR(TAG, "Failed to set saturation to %d: %d", params->saturation, ret);
        return -1;
    }

    ret = IMP_ISP_Tuning_SetSharpness(params->sharpness);
    if (ret < 0) {
        IMP_LOG_ERR(TAG, "Failed to set sharpness to %d: %d", params->sharpness, ret);
        return -1;
    }

    /* Set Priority 1 extensions */
#if defined(PLATFORM_T31)
    ret = IMP_ISP_Tuning_SetBcshHue(params->hue);
    if (ret < 0) {
        IMP_LOG_ERR(TAG, "Failed to set hue to %d: %d", params->hue, ret);
        return -1;
    }

    ret = IMP_ISP_Tuning_SetAeComp(params->ae_compensation);
    if (ret < 0) {
        IMP_LOG_ERR(TAG, "Failed to set AE compensation to %d: %d", params->ae_compensation, ret);
        return -1;
    }
#else
    /* Not available on non-T31 SDKs; retain cached value only */
    (void)ret;
#endif

    ret = IMP_ISP_Tuning_SetSinterStrength(params->noise_reduction_2d);
    if (ret < 0) {
        IMP_LOG_ERR(TAG, "Failed to set 2D noise reduction to %d: %d", params->noise_reduction_2d, ret);
        return -1;
    }

    ret = IMP_ISP_Tuning_SetTemperStrength(params->noise_reduction_3d);
    if (ret < 0) {
        IMP_LOG_ERR(TAG, "Failed to set 3D noise reduction to %d: %d", params->noise_reduction_3d, ret);
        return -1;
    }

    ret = IMP_ISP_Tuning_SetISPHflip(params->flip_horizontal ? IMPISP_TUNING_OPS_MODE_ENABLE : IMPISP_TUNING_OPS_MODE_DISABLE);
    if (ret < 0) {
        IMP_LOG_ERR(TAG, "Failed to set horizontal flip to %d: %d", params->flip_horizontal, ret);
        return -1;
    }

    ret = IMP_ISP_Tuning_SetISPVflip(params->flip_vertical ? IMPISP_TUNING_OPS_MODE_ENABLE : IMPISP_TUNING_OPS_MODE_DISABLE);
    if (ret < 0) {
        IMP_LOG_ERR(TAG, "Failed to set vertical flip to %d: %d", params->flip_vertical, ret);
        return -1;
    }

    /* Set day/night mode */
    IMPISPRunningMode isp_mode = (params->day_night_mode == IMP_CONTROL_MODE_NIGHT) ?
                                 IMPISP_RUNNING_MODE_NIGHT : IMPISP_RUNNING_MODE_DAY;
    ret = IMP_ISP_Tuning_SetISPRunningMode(isp_mode);
    if (ret < 0) {
        IMP_LOG_ERR(TAG, "Failed to set day/night mode to %d: %d", params->day_night_mode, ret);
        return -1;
    }

    /* Set Priority 2 extensions */

    /* Set anti-flicker */
    IMPISPAntiflickerAttr antiflicker_attr;
    if (params->anti_flicker == IMP_CONTROL_ANTIFLICKER_50HZ) {
        antiflicker_attr = IMPISP_ANTIFLICKER_50HZ;
    } else if (params->anti_flicker == IMP_CONTROL_ANTIFLICKER_60HZ) {
        antiflicker_attr = IMPISP_ANTIFLICKER_60HZ;
    } else {
        antiflicker_attr = IMPISP_ANTIFLICKER_DISABLE;
    }
    ret = IMP_ISP_Tuning_SetAntiFlickerAttr(antiflicker_attr);
    if (ret < 0) {
        IMP_LOG_ERR(TAG, "Failed to set anti-flicker to %d: %d", params->anti_flicker, ret);
        return -1;
    }

    /* Set backlight compensation */
#if defined(PLATFORM_T31)
    ret = IMP_ISP_Tuning_SetBacklightComp(params->backlight_compensation);
    if (ret < 0) {
        IMP_LOG_ERR(TAG, "Failed to set backlight compensation to %d: %d", params->backlight_compensation, ret);
        return -1;
    }
#endif

    /* Set highlight suppression */
    ret = IMP_ISP_Tuning_SetHiLightDepress(params->highlight_suppression);
    if (ret < 0) {
        IMP_LOG_ERR(TAG, "Failed to set highlight suppression to %d: %d", params->highlight_suppression, ret);
        return -1;
    }

    /* Set white balance */
    IMPISPWB wb_attr;
    if (params->white_balance_mode == IMP_CONTROL_WB_MANUAL) {
        wb_attr.mode = ISP_CORE_WB_MODE_MANUAL;
        wb_attr.rgain = params->white_balance_r_gain;
        wb_attr.bgain = params->white_balance_b_gain;
    } else {
        wb_attr.mode = ISP_CORE_WB_MODE_AUTO;
        wb_attr.rgain = 256;  /* Default gain values for auto mode */
        wb_attr.bgain = 256;
    }
    ret = IMP_ISP_Tuning_SetWB(&wb_attr);
    if (ret < 0) {
        IMP_LOG_ERR(TAG, "Failed to set white balance (mode=%d, r=%d, b=%d): %d",
                   params->white_balance_mode, wb_attr.rgain, wb_attr.bgain, ret);
        return -1;
    }

    /* Set DRC strength */
#if defined(PLATFORM_T31)
    ret = IMP_ISP_Tuning_SetDRC_Strength(params->drc_strength);
    if (ret < 0) {
        IMP_LOG_ERR(TAG, "Failed to set DRC strength to %d: %d", params->drc_strength, ret);
        return -1;
    }
#endif

    /* Set defog strength */
#if defined(PLATFORM_T31)
    unsigned char defog_val = params->defog_strength;
    ret = IMP_ISP_Tuning_SetDefog_Strength(&defog_val);
    if (ret < 0) {
        IMP_LOG_ERR(TAG, "Failed to set defog strength to %d: %d", params->defog_strength, ret);
        return -1;
    }
#endif

    IMP_LOG_INFO(TAG, "Successfully applied all IMP parameters");
    return 0;
}

/* Get current IMP parameters from hardware */
static int get_current_imp_params(imp_control_params_t* params)
{
    int ret;

    /* Get basic image quality parameters */
    ret = IMP_ISP_Tuning_GetBrightness(&params->brightness);
    if (ret < 0) {
        IMP_LOG_ERR(TAG, "Failed to get brightness: %d", ret);
        return -1;
    }

    ret = IMP_ISP_Tuning_GetContrast(&params->contrast);
    if (ret < 0) {
        IMP_LOG_ERR(TAG, "Failed to get contrast: %d", ret);
        return -1;
    }

    ret = IMP_ISP_Tuning_GetSaturation(&params->saturation);
    if (ret < 0) {
        IMP_LOG_ERR(TAG, "Failed to get saturation: %d", ret);
        return -1;
    }

    ret = IMP_ISP_Tuning_GetSharpness(&params->sharpness);
    if (ret < 0) {
        IMP_LOG_ERR(TAG, "Failed to get sharpness: %d", ret);
        return -1;
    }

    /* Get Priority 1 extensions */
#if defined(PLATFORM_T31)
    ret = IMP_ISP_Tuning_GetBcshHue(&params->hue);
    if (ret < 0) {
        IMP_LOG_ERR(TAG, "Failed to get hue: %d", ret);
        return -1;
    }

    int ae_comp;
    ret = IMP_ISP_Tuning_GetAeComp(&ae_comp);
    if (ret < 0) {
        IMP_LOG_ERR(TAG, "Failed to get AE compensation: %d", ret);
        return -1;
    }
    params->ae_compensation = (unsigned char)ae_comp;
#else
    params->hue = g_imp_control_state.current_params.hue;
    params->ae_compensation = g_imp_control_state.current_params.ae_compensation;
#endif

    /* Note: Noise reduction and flip parameters don't have get functions in T31X IMP API */
    /* We'll use cached values from our internal state */
    params->noise_reduction_2d = g_imp_control_state.current_params.noise_reduction_2d;
    params->noise_reduction_3d = g_imp_control_state.current_params.noise_reduction_3d;
    params->flip_horizontal = g_imp_control_state.current_params.flip_horizontal;
    params->flip_vertical = g_imp_control_state.current_params.flip_vertical;

    IMPISPRunningMode isp_mode;
    ret = IMP_ISP_Tuning_GetISPRunningMode(&isp_mode);
    if (ret < 0) {
        IMP_LOG_ERR(TAG, "Failed to get day/night mode: %d", ret);
        return -1;
    }
    params->day_night_mode = (isp_mode == IMPISP_RUNNING_MODE_NIGHT) ?
                             IMP_CONTROL_MODE_NIGHT : IMP_CONTROL_MODE_DAY;

    /* Get Priority 2 extensions */

    /* Get anti-flicker (use cached value as get function may not be reliable) */
    params->anti_flicker = g_imp_control_state.current_params.anti_flicker;

    /* Get backlight compensation */
#if defined(PLATFORM_T31)
    int backlight_comp;
    ret = IMP_ISP_Tuning_GetBacklightComp(&backlight_comp);
    if (ret < 0) {
        IMP_LOG_ERR(TAG, "Failed to get backlight compensation: %d", ret);
        params->backlight_compensation = g_imp_control_state.current_params.backlight_compensation;
    } else {
        params->backlight_compensation = (unsigned char)backlight_comp;
    }
#else
    params->backlight_compensation = g_imp_control_state.current_params.backlight_compensation;
#endif

    /* Get highlight suppression */
#if defined(PLATFORM_T31)
    int highlight_suppress;
    ret = IMP_ISP_Tuning_GetHiLightDepress(&highlight_suppress);
    if (ret < 0) {
        IMP_LOG_ERR(TAG, "Failed to get highlight suppression: %d", ret);
        params->highlight_suppression = g_imp_control_state.current_params.highlight_suppression;
    } else {
        params->highlight_suppression = (unsigned char)highlight_suppress;
    }
#else
    params->highlight_suppression = g_imp_control_state.current_params.highlight_suppression;
#endif

    /* Get white balance */
    IMPISPWB wb_attr;
    ret = IMP_ISP_Tuning_GetWB(&wb_attr);
    if (ret < 0) {
        IMP_LOG_ERR(TAG, "Failed to get white balance: %d", ret);
        params->white_balance_mode = g_imp_control_state.current_params.white_balance_mode;
        params->white_balance_r_gain = g_imp_control_state.current_params.white_balance_r_gain;
        params->white_balance_b_gain = g_imp_control_state.current_params.white_balance_b_gain;
    } else {
        params->white_balance_mode = (wb_attr.mode == ISP_CORE_WB_MODE_MANUAL) ?
                                    IMP_CONTROL_WB_MANUAL : IMP_CONTROL_WB_AUTO;
        params->white_balance_r_gain = wb_attr.rgain;
        params->white_balance_b_gain = wb_attr.bgain;
    }

    /* Get DRC and defog strength (use cached values as get functions may not be available) */
    params->drc_strength = g_imp_control_state.current_params.drc_strength;
    params->defog_strength = g_imp_control_state.current_params.defog_strength;

    return 0;
}

/* Save parameters to dedicated module configuration file */
static int save_params_to_config(const imp_control_params_t* params)
{
    if (!params) {
        IMP_LOG_ERR(TAG, "Invalid parameters pointer");
        return -1;
    }

    /* Create JSON object for the dedicated module configuration */
    json_object* root = json_object_new_object();
    if (!root) {
        IMP_LOG_ERR(TAG, "Failed to create JSON root object");
        return -1;
    }

    /* Add IMP parameters directly to root object (no wrapper) */
    json_object_object_add(root, "brightness", json_object_new_int(params->brightness));
    json_object_object_add(root, "contrast", json_object_new_int(params->contrast));
    json_object_object_add(root, "saturation", json_object_new_int(params->saturation));
    json_object_object_add(root, "sharpness", json_object_new_int(params->sharpness));

    /* Add Priority 1 extensions */
    json_object_object_add(root, "hue", json_object_new_int(params->hue));
    json_object_object_add(root, "ae_compensation", json_object_new_int(params->ae_compensation));
    json_object_object_add(root, "noise_reduction_2d", json_object_new_int(params->noise_reduction_2d));
    json_object_object_add(root, "noise_reduction_3d", json_object_new_int(params->noise_reduction_3d));
    json_object_object_add(root, "flip_horizontal", json_object_new_boolean(params->flip_horizontal));
    json_object_object_add(root, "flip_vertical", json_object_new_boolean(params->flip_vertical));
    json_object_object_add(root, "day_night_mode", json_object_new_string(imp_control_day_night_mode_to_string(params->day_night_mode)));

    /* Add Priority 2 extensions */
    json_object_object_add(root, "anti_flicker", json_object_new_string(imp_control_antiflicker_mode_to_string(params->anti_flicker)));
    json_object_object_add(root, "backlight_compensation", json_object_new_int(params->backlight_compensation));
    json_object_object_add(root, "highlight_suppression", json_object_new_int(params->highlight_suppression));
    json_object_object_add(root, "white_balance_mode", json_object_new_string(imp_control_wb_mode_to_string(params->white_balance_mode)));
    json_object_object_add(root, "white_balance_r_gain", json_object_new_int(params->white_balance_r_gain));
    json_object_object_add(root, "white_balance_b_gain", json_object_new_int(params->white_balance_b_gain));
    json_object_object_add(root, "drc_strength", json_object_new_int(params->drc_strength));
    json_object_object_add(root, "defog_strength", json_object_new_int(params->defog_strength));

    /* Save to dedicated module configuration file */
    const char* config_path = "/etc/streamer.d/imp_control.json";
    int ret = json_object_to_file_ext(config_path, root, JSON_C_TO_STRING_PRETTY);

    json_object_put(root);

    if (ret < 0) {
        IMP_LOG_ERR(TAG, "Failed to write module configuration to %s", config_path);
        return -1;
    }

    IMP_LOG_INFO(TAG, "IMP control parameters saved to module configuration: %s", config_path);
    return 0;
}



/* Send JSON response with IMP parameters */
static void send_params_json_response(int client_socket, const imp_control_params_t* params, const char* message)
{
    char json_response[2048];  /* Increased size for Priority 2 parameters */

    if (message) {
        /* Response with success message */
        snprintf(json_response, sizeof(json_response),
                 "{"
                 "\"success\":true,"
                 "\"message\":\"%s\","
                 "\"brightness\":%d,"
                 "\"contrast\":%d,"
                 "\"saturation\":%d,"
                 "\"sharpness\":%d,"
                 "\"hue\":%d,"
                 "\"ae_compensation\":%d,"
                 "\"noise_reduction_2d\":%d,"
                 "\"noise_reduction_3d\":%d,"
                 "\"flip_horizontal\":%s,"
                 "\"flip_vertical\":%s,"
                 "\"day_night_mode\":\"%s\","
                 "\"anti_flicker\":\"%s\","
                 "\"backlight_compensation\":%d,"
                 "\"highlight_suppression\":%d,"
                 "\"white_balance_mode\":\"%s\","
                 "\"white_balance_r_gain\":%d,"
                 "\"white_balance_b_gain\":%d,"
                 "\"drc_strength\":%d,"
                 "\"defog_strength\":%d,"
                 "\"timestamp\":%lld"
                 "}",
                 message,
                 params->brightness, params->contrast, params->saturation, params->sharpness,
                 params->hue, params->ae_compensation, params->noise_reduction_2d, params->noise_reduction_3d,
                 params->flip_horizontal ? "true" : "false",
                 params->flip_vertical ? "true" : "false",
                 imp_control_day_night_mode_to_string(params->day_night_mode),
                 imp_control_antiflicker_mode_to_string(params->anti_flicker),
                 params->backlight_compensation, params->highlight_suppression,
                 imp_control_wb_mode_to_string(params->white_balance_mode),
                 params->white_balance_r_gain, params->white_balance_b_gain,
                 params->drc_strength, params->defog_strength,
                 (long long)time(NULL));
    } else {
        /* Simple parameter response */
        snprintf(json_response, sizeof(json_response),
                 "{"
                 "\"brightness\":%d,"
                 "\"contrast\":%d,"
                 "\"saturation\":%d,"
                 "\"sharpness\":%d,"
                 "\"hue\":%d,"
                 "\"ae_compensation\":%d,"
                 "\"noise_reduction_2d\":%d,"
                 "\"noise_reduction_3d\":%d,"
                 "\"flip_horizontal\":%s,"
                 "\"flip_vertical\":%s,"
                 "\"day_night_mode\":\"%s\","
                 "\"anti_flicker\":\"%s\","
                 "\"backlight_compensation\":%d,"
                 "\"highlight_suppression\":%d,"
                 "\"white_balance_mode\":\"%s\","
                 "\"white_balance_r_gain\":%d,"
                 "\"white_balance_b_gain\":%d,"
                 "\"drc_strength\":%d,"
                 "\"defog_strength\":%d,"
                 "\"timestamp\":%lld"
                 "}",
                 params->brightness, params->contrast, params->saturation, params->sharpness,
                 params->hue, params->ae_compensation, params->noise_reduction_2d, params->noise_reduction_3d,
                 params->flip_horizontal ? "true" : "false",
                 params->flip_vertical ? "true" : "false",
                 imp_control_day_night_mode_to_string(params->day_night_mode),
                 imp_control_antiflicker_mode_to_string(params->anti_flicker),
                 params->backlight_compensation, params->highlight_suppression,
                 imp_control_wb_mode_to_string(params->white_balance_mode),
                 params->white_balance_r_gain, params->white_balance_b_gain,
                 params->drc_strength, params->defog_strength,
                 (long long)time(NULL));
    }

    http_send_json(client_socket, json_response);
}

/* Module lifecycle callbacks */
int imp_control_init(void* config)
{
    IMP_LOG_INFO(TAG, "Initializing IMP control module");

    if (g_imp_control_state.initialized) {
        IMP_LOG_WARN(TAG, "IMP control module already initialized");
        return 0;
    }

    /* Load configuration using dual path approach */
    if (config) {
        /* Configuration provided by module system */
        IMP_LOG_INFO(TAG, "Using configuration provided by module system");
        memcpy(&g_imp_control_state.config, config, sizeof(imp_control_config_t));
    } else {
        /* Load configuration from file using module system function */
        IMP_LOG_INFO(TAG, "Loading configuration from file");
        if (module_load_config_file("imp_control", &g_imp_control_state.config,
                                   sizeof(imp_control_config_t), imp_control_config_parse) != 0) {
            IMP_LOG_WARN(TAG, "Failed to load configuration, using defaults");
            imp_control_set_defaults(&g_imp_control_state.config);
        }
    }

    /* Initialize mutex */
    if (pthread_mutex_init(&g_imp_control_state.params_mutex, NULL) != 0) {
        IMP_LOG_ERR(TAG, "Failed to initialize parameters mutex");
        return -1;
    }

    /* Initialize current parameters with config values */
    g_imp_control_state.current_params.brightness = g_imp_control_state.config.brightness;
    g_imp_control_state.current_params.contrast = g_imp_control_state.config.contrast;
    g_imp_control_state.current_params.saturation = g_imp_control_state.config.saturation;
    g_imp_control_state.current_params.sharpness = g_imp_control_state.config.sharpness;
    g_imp_control_state.current_params.hue = g_imp_control_state.config.hue;
    g_imp_control_state.current_params.ae_compensation = g_imp_control_state.config.ae_compensation;
    g_imp_control_state.current_params.noise_reduction_2d = g_imp_control_state.config.noise_reduction_2d;
    g_imp_control_state.current_params.noise_reduction_3d = g_imp_control_state.config.noise_reduction_3d;
    g_imp_control_state.current_params.flip_horizontal = g_imp_control_state.config.flip_horizontal;
    g_imp_control_state.current_params.flip_vertical = g_imp_control_state.config.flip_vertical;
    g_imp_control_state.current_params.day_night_mode = g_imp_control_state.config.day_night_mode;
    g_imp_control_state.current_params.anti_flicker = g_imp_control_state.config.anti_flicker;
    g_imp_control_state.current_params.backlight_compensation = g_imp_control_state.config.backlight_compensation;
    g_imp_control_state.current_params.highlight_suppression = g_imp_control_state.config.highlight_suppression;
    g_imp_control_state.current_params.white_balance_mode = g_imp_control_state.config.white_balance_mode;
    g_imp_control_state.current_params.white_balance_r_gain = g_imp_control_state.config.white_balance_r_gain;
    g_imp_control_state.current_params.white_balance_b_gain = g_imp_control_state.config.white_balance_b_gain;
    g_imp_control_state.current_params.drc_strength = g_imp_control_state.config.drc_strength;
    g_imp_control_state.current_params.defog_strength = g_imp_control_state.config.defog_strength;

    g_imp_control_state.initialized = true;
    IMP_LOG_INFO(TAG, "IMP control module initialized successfully");
    return 0;
}

int imp_control_start(void)
{
    IMP_LOG_INFO(TAG, "Starting IMP control module");

    if (!g_imp_control_state.initialized) {
        IMP_LOG_ERR(TAG, "IMP control module not initialized");
        return -1;
    }

    if (g_imp_control_state.running) {
        IMP_LOG_WARN(TAG, "IMP control module already running");
        return 0;
    }

    /* Module is always enabled when compiled in */

    /* Apply default parameters to IMP */
    if (apply_imp_params(&g_imp_control_state.current_params) < 0) {
        IMP_LOG_ERR(TAG, "Failed to apply default IMP parameters");
        return -1;
    }

    g_imp_control_state.running = true;
    IMP_LOG_INFO(TAG, "IMP control module started successfully");
    return 0;
}

int imp_control_stop(void)
{
    IMP_LOG_INFO(TAG, "Stopping IMP control module");

    if (!g_imp_control_state.running) {
        IMP_LOG_WARN(TAG, "IMP control module not running");
        return 0;
    }

    g_imp_control_state.running = false;
    IMP_LOG_INFO(TAG, "IMP control module stopped successfully");
    return 0;
}

int imp_control_cleanup(void)
{
    IMP_LOG_INFO(TAG, "Cleaning up IMP control module");

    if (g_imp_control_state.running) {
        imp_control_stop();
    }

    if (!g_imp_control_state.initialized) {
        return 0;
    }

    /* Destroy mutex */
    pthread_mutex_destroy(&g_imp_control_state.params_mutex);

    /* Reset state */
    memset(&g_imp_control_state, 0, sizeof(g_imp_control_state));

    IMP_LOG_INFO(TAG, "IMP control module cleaned up successfully");
    return 0;
}

int imp_control_get_config_size(void)
{
    return sizeof(imp_control_config_t);
}

int imp_control_set_defaults(void* config)
{
    if (!config) {
        IMP_LOG_ERR(TAG, "Invalid config pointer");
        return -1;
    }

    imp_control_config_t* imp_config = (imp_control_config_t*)config;
    memset(imp_config, 0, sizeof(imp_control_config_t));

    /* Set default IMP parameters */
    imp_config->brightness = IMP_CONTROL_DEFAULT_VALUE;
    imp_config->contrast = IMP_CONTROL_DEFAULT_VALUE;
    imp_config->saturation = IMP_CONTROL_DEFAULT_VALUE;
    imp_config->sharpness = IMP_CONTROL_DEFAULT_VALUE;

    /* Set default Priority 1 extensions */
    imp_config->hue = IMP_CONTROL_DEFAULT_VALUE;
    imp_config->ae_compensation = IMP_CONTROL_DEFAULT_VALUE;
    imp_config->noise_reduction_2d = IMP_CONTROL_DEFAULT_VALUE;
    imp_config->noise_reduction_3d = IMP_CONTROL_DEFAULT_VALUE;
    imp_config->flip_horizontal = false;
    imp_config->flip_vertical = false;
    imp_config->day_night_mode = IMP_CONTROL_MODE_AUTO;

    /* Set default Priority 2 extensions */
    imp_config->anti_flicker = IMP_CONTROL_ANTIFLICKER_DISABLE;
    imp_config->backlight_compensation = 0;
    imp_config->highlight_suppression = 0;
    imp_config->white_balance_mode = IMP_CONTROL_WB_AUTO;
    imp_config->white_balance_r_gain = 256;  /* Default gain values */
    imp_config->white_balance_b_gain = 256;
    imp_config->drc_strength = IMP_CONTROL_DEFAULT_VALUE;
    imp_config->defog_strength = 0;  /* Defog disabled by default */

    return 0;
}

/* IMP parameter control functions */
int imp_control_get_params(imp_control_params_t* params)
{
    if (!params) {
        IMP_LOG_ERR(TAG, "Invalid parameters pointer");
        return -1;
    }

    if (!g_imp_control_state.initialized) {
        IMP_LOG_ERR(TAG, "IMP control module not initialized");
        return -1;
    }

    pthread_mutex_lock(&g_imp_control_state.params_mutex);

    /* Get current parameters from hardware */
    int ret = get_current_imp_params(&g_imp_control_state.current_params);
    if (ret == 0) {
        memcpy(params, &g_imp_control_state.current_params, sizeof(imp_control_params_t));
    }

    pthread_mutex_unlock(&g_imp_control_state.params_mutex);
    return ret;
}

int imp_control_set_params(const imp_control_params_t* params)
{
    if (!params) {
        IMP_LOG_ERR(TAG, "Invalid parameters pointer");
        return -1;
    }

    if (!g_imp_control_state.initialized) {
        IMP_LOG_ERR(TAG, "IMP control module not initialized");
        return -1;
    }

    /* Validate parameters */
    if (IMP_CONTROL_VALIDATE_RANGE) {
        if (!imp_control_validate_params(params)) {
            IMP_LOG_ERR(TAG, "Invalid parameter values provided");
            return -1;
        }
    }

    pthread_mutex_lock(&g_imp_control_state.params_mutex);

    /* Apply parameters to hardware */
    int ret = apply_imp_params(params);
    if (ret == 0) {
        /* Update cached values */
        memcpy(&g_imp_control_state.current_params, params, sizeof(imp_control_params_t));

        if (IMP_CONTROL_LOG_CHANGES) {
            IMP_LOG_INFO(TAG, "IMP parameters updated: brightness=%d, contrast=%d, saturation=%d, sharpness=%d",
                         params->brightness, params->contrast, params->saturation, params->sharpness);
        }
    }

    pthread_mutex_unlock(&g_imp_control_state.params_mutex);
    return ret;
}

int imp_control_set_brightness(unsigned char brightness)
{
    imp_control_params_t params;

    /* Get current parameters */
    if (imp_control_get_params(&params) < 0) {
        IMP_LOG_ERR(TAG, "Failed to get current IMP parameters");
        return -1;
    }

    /* Update brightness */
    params.brightness = brightness;

    /* Apply updated parameters */
    return imp_control_set_params(&params);
}

int imp_control_set_contrast(unsigned char contrast)
{
    imp_control_params_t params;

    /* Get current parameters */
    if (imp_control_get_params(&params) < 0) {
        IMP_LOG_ERR(TAG, "Failed to get current IMP parameters");
        return -1;
    }

    /* Update contrast */
    params.contrast = contrast;

    /* Apply updated parameters */
    return imp_control_set_params(&params);
}

int imp_control_set_saturation(unsigned char saturation)
{
    imp_control_params_t params;

    /* Get current parameters */
    if (imp_control_get_params(&params) < 0) {
        IMP_LOG_ERR(TAG, "Failed to get current IMP parameters");
        return -1;
    }

    /* Update saturation */
    params.saturation = saturation;

    /* Apply updated parameters */
    return imp_control_set_params(&params);
}

int imp_control_set_sharpness(unsigned char sharpness)
{
    imp_control_params_t params;

    /* Get current parameters */
    if (imp_control_get_params(&params) < 0) {
        IMP_LOG_ERR(TAG, "Failed to get current IMP parameters");
        return -1;
    }

    /* Update sharpness */
    params.sharpness = sharpness;

    /* Apply updated parameters */
    return imp_control_set_params(&params);
}

int imp_control_set_hue(unsigned char hue)
{
    imp_control_params_t params;

    /* Get current parameters */
    if (imp_control_get_params(&params) < 0) {
        IMP_LOG_ERR(TAG, "Failed to get current IMP parameters");
        return -1;
    }

    /* Update hue */
    params.hue = hue;

    /* Apply updated parameters */
    return imp_control_set_params(&params);
}

int imp_control_set_ae_compensation(unsigned char ae_comp)
{
    imp_control_params_t params;

    /* Get current parameters */
    if (imp_control_get_params(&params) < 0) {
        IMP_LOG_ERR(TAG, "Failed to get current IMP parameters");
        return -1;
    }

    /* Update AE compensation */
    params.ae_compensation = ae_comp;

    /* Apply updated parameters */
    return imp_control_set_params(&params);
}

int imp_control_set_noise_reduction_2d(unsigned char strength)
{
    imp_control_params_t params;

    /* Get current parameters */
    if (imp_control_get_params(&params) < 0) {
        IMP_LOG_ERR(TAG, "Failed to get current IMP parameters");
        return -1;
    }

    /* Update 2D noise reduction */
    params.noise_reduction_2d = strength;

    /* Apply updated parameters */
    return imp_control_set_params(&params);
}

int imp_control_set_noise_reduction_3d(unsigned char strength)
{
    imp_control_params_t params;

    /* Get current parameters */
    if (imp_control_get_params(&params) < 0) {
        IMP_LOG_ERR(TAG, "Failed to get current IMP parameters");
        return -1;
    }

    /* Update 3D noise reduction */
    params.noise_reduction_3d = strength;

    /* Apply updated parameters */
    return imp_control_set_params(&params);
}

int imp_control_set_flip_horizontal(bool enable)
{
    imp_control_params_t params;

    /* Get current parameters */
    if (imp_control_get_params(&params) < 0) {
        IMP_LOG_ERR(TAG, "Failed to get current IMP parameters");
        return -1;
    }

    /* Update horizontal flip */
    params.flip_horizontal = enable;

    /* Apply updated parameters */
    return imp_control_set_params(&params);
}

int imp_control_set_flip_vertical(bool enable)
{
    imp_control_params_t params;

    /* Get current parameters */
    if (imp_control_get_params(&params) < 0) {
        IMP_LOG_ERR(TAG, "Failed to get current IMP parameters");
        return -1;
    }

    /* Update vertical flip */
    params.flip_vertical = enable;

    /* Apply updated parameters */
    return imp_control_set_params(&params);
}

int imp_control_set_day_night_mode(imp_control_day_night_mode_t mode)
{
    imp_control_params_t params;

    /* Get current parameters */
    if (imp_control_get_params(&params) < 0) {
        IMP_LOG_ERR(TAG, "Failed to get current IMP parameters");
        return -1;
    }

    /* Update day/night mode */
    params.day_night_mode = mode;

    /* Apply updated parameters */
    return imp_control_set_params(&params);
}

int imp_control_set_anti_flicker(imp_control_antiflicker_mode_t mode)
{
    imp_control_params_t params;

    /* Get current parameters */
    if (imp_control_get_params(&params) < 0) {
        IMP_LOG_ERR(TAG, "Failed to get current IMP parameters");
        return -1;
    }

    /* Update anti-flicker mode */
    params.anti_flicker = mode;

    /* Apply updated parameters */
    return imp_control_set_params(&params);
}

int imp_control_set_backlight_compensation(unsigned char strength)
{
    imp_control_params_t params;

    /* Get current parameters */
    if (imp_control_get_params(&params) < 0) {
        IMP_LOG_ERR(TAG, "Failed to get current IMP parameters");
        return -1;
    }

    /* Update backlight compensation */
    params.backlight_compensation = strength;

    /* Apply updated parameters */
    return imp_control_set_params(&params);
}

int imp_control_set_highlight_suppression(unsigned char strength)
{
    imp_control_params_t params;

    /* Get current parameters */
    if (imp_control_get_params(&params) < 0) {
        IMP_LOG_ERR(TAG, "Failed to get current IMP parameters");
        return -1;
    }

    /* Update highlight suppression */
    params.highlight_suppression = strength;

    /* Apply updated parameters */
    return imp_control_set_params(&params);
}

int imp_control_set_white_balance_mode(imp_control_wb_mode_t mode)
{
    imp_control_params_t params;

    /* Get current parameters */
    if (imp_control_get_params(&params) < 0) {
        IMP_LOG_ERR(TAG, "Failed to get current IMP parameters");
        return -1;
    }

    /* Update white balance mode */
    params.white_balance_mode = mode;

    /* Apply updated parameters */
    return imp_control_set_params(&params);
}

int imp_control_set_white_balance_gains(unsigned short r_gain, unsigned short b_gain)
{
    imp_control_params_t params;

    /* Get current parameters */
    if (imp_control_get_params(&params) < 0) {
        IMP_LOG_ERR(TAG, "Failed to get current IMP parameters");
        return -1;
    }

    /* Update white balance gains */
    params.white_balance_r_gain = r_gain;
    params.white_balance_b_gain = b_gain;

    /* Apply updated parameters */
    return imp_control_set_params(&params);
}

int imp_control_set_drc_strength(unsigned char strength)
{
    imp_control_params_t params;

    /* Get current parameters */
    if (imp_control_get_params(&params) < 0) {
        IMP_LOG_ERR(TAG, "Failed to get current IMP parameters");
        return -1;
    }

    /* Update DRC strength */
    params.drc_strength = strength;

    /* Apply updated parameters */
    return imp_control_set_params(&params);
}

int imp_control_set_defog_strength(unsigned char strength)
{
    imp_control_params_t params;

    /* Get current parameters */
    if (imp_control_get_params(&params) < 0) {
        IMP_LOG_ERR(TAG, "Failed to get current IMP parameters");
        return -1;
    }

    /* Update defog strength */
    params.defog_strength = strength;

    /* Apply updated parameters */
    return imp_control_set_params(&params);
}

int imp_control_reset_to_defaults(void)
{
    if (!g_imp_control_state.initialized) {
        IMP_LOG_ERR(TAG, "IMP control module not initialized");
        return -1;
    }

    imp_control_params_t default_params = {
        .brightness = g_imp_control_state.config.brightness,
        .contrast = g_imp_control_state.config.contrast,
        .saturation = g_imp_control_state.config.saturation,
        .sharpness = g_imp_control_state.config.sharpness,
        .hue = g_imp_control_state.config.hue,
        .ae_compensation = g_imp_control_state.config.ae_compensation,
        .noise_reduction_2d = g_imp_control_state.config.noise_reduction_2d,
        .noise_reduction_3d = g_imp_control_state.config.noise_reduction_3d,
        .flip_horizontal = g_imp_control_state.config.flip_horizontal,
        .flip_vertical = g_imp_control_state.config.flip_vertical,
        .day_night_mode = g_imp_control_state.config.day_night_mode,
        .anti_flicker = g_imp_control_state.config.anti_flicker,
        .backlight_compensation = g_imp_control_state.config.backlight_compensation,
        .highlight_suppression = g_imp_control_state.config.highlight_suppression,
        .white_balance_mode = g_imp_control_state.config.white_balance_mode,
        .white_balance_r_gain = g_imp_control_state.config.white_balance_r_gain,
        .white_balance_b_gain = g_imp_control_state.config.white_balance_b_gain,
        .drc_strength = g_imp_control_state.config.drc_strength,
        .defog_strength = g_imp_control_state.config.defog_strength
    };

    IMP_LOG_INFO(TAG, "Resetting IMP parameters to defaults");
    return imp_control_set_params(&default_params);
}

/* Parameter validation functions */
bool imp_control_validate_param(unsigned char value)
{
    return (value >= IMP_CONTROL_MIN_VALUE && value <= IMP_CONTROL_MAX_VALUE);
}

bool imp_control_validate_params(const imp_control_params_t* params)
{
    if (!params) {
        IMP_LOG_ERR(TAG, "Invalid parameters pointer");
        return false;
    }

    return (imp_control_validate_param(params->brightness) &&
            imp_control_validate_param(params->contrast) &&
            imp_control_validate_param(params->saturation) &&
            imp_control_validate_param(params->sharpness));
}

/* HTTP API handlers */
void imp_control_handle_get_params(int client_socket)
{
    if (!IMP_CONTROL_ENDPOINTS_ENABLED) {
        http_send_error(client_socket, HTTP_STATUS_NOT_FOUND, "Endpoint disabled");
        return;
    }

    imp_control_params_t params;
    if (imp_control_get_params(&params) < 0) {
        http_send_json(client_socket, "{\"error\":\"Failed to get IMP parameters\"}");
        return;
    }

    send_params_json_response(client_socket, &params, NULL);
}

void imp_control_handle_set_params(int client_socket, const char* request_body)
{
    if (!IMP_CONTROL_ENDPOINTS_ENABLED) {
        http_send_error(client_socket, HTTP_STATUS_NOT_FOUND, "Endpoint disabled");
        return;
    }

    if (!request_body) {
        http_send_json(client_socket, "{\"error\":\"Missing request body\"}");
        return;
    }

    /* Simple JSON parsing for parameters */
    imp_control_params_t params = g_imp_control_state.current_params; /* Start with current values */

    /* Parse brightness */
    const char* brightness_str = strstr(request_body, "\"brightness\":");
    if (brightness_str) {
        brightness_str += strlen("\"brightness\":");
        while (*brightness_str && isspace(*brightness_str)) brightness_str++;
        params.brightness = (unsigned char)atoi(brightness_str);
    }

    /* Parse contrast */
    const char* contrast_str = strstr(request_body, "\"contrast\":");
    if (contrast_str) {
        contrast_str += strlen("\"contrast\":");
        while (*contrast_str && isspace(*contrast_str)) contrast_str++;
        params.contrast = (unsigned char)atoi(contrast_str);
    }

    /* Parse saturation */
    const char* saturation_str = strstr(request_body, "\"saturation\":");
    if (saturation_str) {
        saturation_str += strlen("\"saturation\":");
        while (*saturation_str && isspace(*saturation_str)) saturation_str++;
        params.saturation = (unsigned char)atoi(saturation_str);
    }

    /* Parse sharpness */
    const char* sharpness_str = strstr(request_body, "\"sharpness\":");
    if (sharpness_str) {
        sharpness_str += strlen("\"sharpness\":");
        while (*sharpness_str && isspace(*sharpness_str)) sharpness_str++;
        params.sharpness = (unsigned char)atoi(sharpness_str);
    }

    /* Parse hue */
    const char* hue_str = strstr(request_body, "\"hue\":");
    if (hue_str) {
        hue_str += strlen("\"hue\":");
        while (*hue_str && isspace(*hue_str)) hue_str++;
        params.hue = (unsigned char)atoi(hue_str);
    }

    /* Parse AE compensation */
    const char* ae_comp_str = strstr(request_body, "\"ae_compensation\":");
    if (ae_comp_str) {
        ae_comp_str += strlen("\"ae_compensation\":");
        while (*ae_comp_str && isspace(*ae_comp_str)) ae_comp_str++;
        params.ae_compensation = (unsigned char)atoi(ae_comp_str);
    }

    /* Parse 2D noise reduction */
    const char* nr2d_str = strstr(request_body, "\"noise_reduction_2d\":");
    if (nr2d_str) {
        nr2d_str += strlen("\"noise_reduction_2d\":");
        while (*nr2d_str && isspace(*nr2d_str)) nr2d_str++;
        params.noise_reduction_2d = (unsigned char)atoi(nr2d_str);
    }

    /* Parse 3D noise reduction */
    const char* nr3d_str = strstr(request_body, "\"noise_reduction_3d\":");
    if (nr3d_str) {
        nr3d_str += strlen("\"noise_reduction_3d\":");
        while (*nr3d_str && isspace(*nr3d_str)) nr3d_str++;
        params.noise_reduction_3d = (unsigned char)atoi(nr3d_str);
    }

    /* Parse horizontal flip */
    const char* hflip_str = strstr(request_body, "\"flip_horizontal\":");
    if (hflip_str) {
        hflip_str += strlen("\"flip_horizontal\":");
        while (*hflip_str && isspace(*hflip_str)) hflip_str++;
        params.flip_horizontal = (strncmp(hflip_str, "true", 4) == 0);
    }

    /* Parse vertical flip */
    const char* vflip_str = strstr(request_body, "\"flip_vertical\":");
    if (vflip_str) {
        vflip_str += strlen("\"flip_vertical\":");
        while (*vflip_str && isspace(*vflip_str)) vflip_str++;
        params.flip_vertical = (strncmp(vflip_str, "true", 4) == 0);
    }

    /* Parse day/night mode */
    const char* mode_str = strstr(request_body, "\"day_night_mode\":");
    if (mode_str) {
        mode_str += strlen("\"day_night_mode\":");
        while (*mode_str && isspace(*mode_str)) mode_str++;
        if (*mode_str == '"') mode_str++; /* Skip opening quote */

        char mode_value[16] = {0};
        int i = 0;
        while (*mode_str && *mode_str != '"' && i < sizeof(mode_value) - 1) {
            mode_value[i++] = *mode_str++;
        }
        params.day_night_mode = imp_control_string_to_day_night_mode(mode_value);
    }

    /* Parse Priority 2 parameters */

    /* Parse anti-flicker mode */
    const char* antiflicker_str = strstr(request_body, "\"anti_flicker\":");
    if (antiflicker_str) {
        antiflicker_str += strlen("\"anti_flicker\":");
        while (*antiflicker_str && isspace(*antiflicker_str)) antiflicker_str++;
        if (*antiflicker_str == '"') antiflicker_str++; /* Skip opening quote */

        char antiflicker_value[16] = {0};
        int i = 0;
        while (*antiflicker_str && *antiflicker_str != '"' && i < sizeof(antiflicker_value) - 1) {
            antiflicker_value[i++] = *antiflicker_str++;
        }
        params.anti_flicker = imp_control_string_to_antiflicker_mode(antiflicker_value);
    }

    /* Parse backlight compensation */
    const char* backlight_str = strstr(request_body, "\"backlight_compensation\":");
    if (backlight_str) {
        backlight_str += strlen("\"backlight_compensation\":");
        while (*backlight_str && isspace(*backlight_str)) backlight_str++;
        params.backlight_compensation = (unsigned char)atoi(backlight_str);
    }

    /* Parse highlight suppression */
    const char* highlight_str = strstr(request_body, "\"highlight_suppression\":");
    if (highlight_str) {
        highlight_str += strlen("\"highlight_suppression\":");
        while (*highlight_str && isspace(*highlight_str)) highlight_str++;
        params.highlight_suppression = (unsigned char)atoi(highlight_str);
    }

    /* Parse white balance mode */
    const char* wb_mode_str = strstr(request_body, "\"white_balance_mode\":");
    if (wb_mode_str) {
        wb_mode_str += strlen("\"white_balance_mode\":");
        while (*wb_mode_str && isspace(*wb_mode_str)) wb_mode_str++;
        if (*wb_mode_str == '"') wb_mode_str++; /* Skip opening quote */

        char wb_mode_value[16] = {0};
        int i = 0;
        while (*wb_mode_str && *wb_mode_str != '"' && i < sizeof(wb_mode_value) - 1) {
            wb_mode_value[i++] = *wb_mode_str++;
        }
        params.white_balance_mode = imp_control_string_to_wb_mode(wb_mode_value);
    }

    /* Parse white balance red gain */
    const char* wb_r_str = strstr(request_body, "\"white_balance_r_gain\":");
    if (wb_r_str) {
        wb_r_str += strlen("\"white_balance_r_gain\":");
        while (*wb_r_str && isspace(*wb_r_str)) wb_r_str++;
        params.white_balance_r_gain = (unsigned short)atoi(wb_r_str);
    }

    /* Parse white balance blue gain */
    const char* wb_b_str = strstr(request_body, "\"white_balance_b_gain\":");
    if (wb_b_str) {
        wb_b_str += strlen("\"white_balance_b_gain\":");
        while (*wb_b_str && isspace(*wb_b_str)) wb_b_str++;
        params.white_balance_b_gain = (unsigned short)atoi(wb_b_str);
    }

    /* Parse DRC strength */
    const char* drc_str = strstr(request_body, "\"drc_strength\":");
    if (drc_str) {
        drc_str += strlen("\"drc_strength\":");
        while (*drc_str && isspace(*drc_str)) drc_str++;
        params.drc_strength = (unsigned char)atoi(drc_str);
    }

    /* Parse defog strength */
    const char* defog_str = strstr(request_body, "\"defog_strength\":");
    if (defog_str) {
        defog_str += strlen("\"defog_strength\":");
        while (*defog_str && isspace(*defog_str)) defog_str++;
        params.defog_strength = (unsigned char)atoi(defog_str);
    }

    /* Apply parameters */
    if (imp_control_set_params(&params) < 0) {
        http_send_json(client_socket, "{\"error\":\"Failed to set IMP parameters\"}");
        return;
    }

    /* Return success with updated parameters */
    send_params_json_response(client_socket, &params, NULL);
}

void imp_control_handle_reset_params(int client_socket)
{
    if (!IMP_CONTROL_ENDPOINTS_ENABLED) {
        http_send_error(client_socket, HTTP_STATUS_NOT_FOUND, "Endpoint disabled");
        return;
    }

    if (imp_control_reset_to_defaults() < 0) {
        http_send_json(client_socket, "{\"error\":\"Failed to reset IMP parameters\"}");
        return;
    }

    /* Return success with default parameters */
    imp_control_params_t default_params = {
        .brightness = g_imp_control_state.config.brightness,
        .contrast = g_imp_control_state.config.contrast,
        .saturation = g_imp_control_state.config.saturation,
        .sharpness = g_imp_control_state.config.sharpness,
        .hue = g_imp_control_state.config.hue,
        .ae_compensation = g_imp_control_state.config.ae_compensation,
        .noise_reduction_2d = g_imp_control_state.config.noise_reduction_2d,
        .noise_reduction_3d = g_imp_control_state.config.noise_reduction_3d,
        .flip_horizontal = g_imp_control_state.config.flip_horizontal,
        .flip_vertical = g_imp_control_state.config.flip_vertical,
        .day_night_mode = g_imp_control_state.config.day_night_mode,
        .anti_flicker = g_imp_control_state.config.anti_flicker,
        .backlight_compensation = g_imp_control_state.config.backlight_compensation,
        .highlight_suppression = g_imp_control_state.config.highlight_suppression,
        .white_balance_mode = g_imp_control_state.config.white_balance_mode,
        .white_balance_r_gain = g_imp_control_state.config.white_balance_r_gain,
        .white_balance_b_gain = g_imp_control_state.config.white_balance_b_gain,
        .drc_strength = g_imp_control_state.config.drc_strength,
        .defog_strength = g_imp_control_state.config.defog_strength
    };

    send_params_json_response(client_socket, &default_params, "IMP parameters reset to defaults");
}

void imp_control_handle_save_params(int client_socket)
{
    if (!IMP_CONTROL_ENDPOINTS_ENABLED) {
        http_send_error(client_socket, HTTP_STATUS_NOT_FOUND, "Endpoint disabled");
        return;
    }

    /* Get current parameters */
    imp_control_params_t params;
    if (imp_control_get_params(&params) < 0) {
        http_send_json(client_socket, "{\"error\":\"Failed to get current IMP parameters\"}");
        return;
    }

    /* Save parameters to configuration file */
    if (save_params_to_config(&params) < 0) {
        http_send_json(client_socket, "{\"error\":\"Failed to save IMP parameters to configuration\"}");
        return;
    }

    /* Update internal configuration */
    g_imp_control_state.config.brightness = params.brightness;
    g_imp_control_state.config.contrast = params.contrast;
    g_imp_control_state.config.saturation = params.saturation;
    g_imp_control_state.config.sharpness = params.sharpness;
    g_imp_control_state.config.hue = params.hue;
    g_imp_control_state.config.ae_compensation = params.ae_compensation;
    g_imp_control_state.config.noise_reduction_2d = params.noise_reduction_2d;
    g_imp_control_state.config.noise_reduction_3d = params.noise_reduction_3d;
    g_imp_control_state.config.flip_horizontal = params.flip_horizontal;
    g_imp_control_state.config.flip_vertical = params.flip_vertical;
    g_imp_control_state.config.day_night_mode = params.day_night_mode;
    g_imp_control_state.config.anti_flicker = params.anti_flicker;
    g_imp_control_state.config.backlight_compensation = params.backlight_compensation;
    g_imp_control_state.config.highlight_suppression = params.highlight_suppression;
    g_imp_control_state.config.white_balance_mode = params.white_balance_mode;
    g_imp_control_state.config.white_balance_r_gain = params.white_balance_r_gain;
    g_imp_control_state.config.white_balance_b_gain = params.white_balance_b_gain;
    g_imp_control_state.config.drc_strength = params.drc_strength;
    g_imp_control_state.config.defog_strength = params.defog_strength;

    /* Return success with saved parameters */
    send_params_json_response(client_socket, &params, "IMP parameters saved to configuration");
}

/* Utility functions */
const char* imp_control_get_version(void)
{
    return IMP_CONTROL_VERSION;
}

const char* imp_control_get_name(void)
{
    return IMP_CONTROL_NAME;
}

const char* imp_control_day_night_mode_to_string(imp_control_day_night_mode_t mode)
{
    switch (mode) {
        case IMP_CONTROL_MODE_DAY:
            return "day";
        case IMP_CONTROL_MODE_NIGHT:
            return "night";
        case IMP_CONTROL_MODE_AUTO:
            return "auto";
        default:
            return "auto";
    }
}

imp_control_day_night_mode_t imp_control_string_to_day_night_mode(const char* str)
{
    if (!str) {
        return IMP_CONTROL_MODE_AUTO;
    }

    if (strcmp(str, "day") == 0) {
        return IMP_CONTROL_MODE_DAY;
    } else if (strcmp(str, "night") == 0) {
        return IMP_CONTROL_MODE_NIGHT;
    } else if (strcmp(str, "auto") == 0) {
        return IMP_CONTROL_MODE_AUTO;
    } else {
        return IMP_CONTROL_MODE_AUTO;
    }
}

const char* imp_control_antiflicker_mode_to_string(imp_control_antiflicker_mode_t mode)
{
    switch (mode) {
        case IMP_CONTROL_ANTIFLICKER_DISABLE:
            return "disable";
        case IMP_CONTROL_ANTIFLICKER_50HZ:
            return "50hz";
        case IMP_CONTROL_ANTIFLICKER_60HZ:
            return "60hz";
        default:
            return "disable";
    }
}

imp_control_antiflicker_mode_t imp_control_string_to_antiflicker_mode(const char* str)
{
    if (!str) {
        return IMP_CONTROL_ANTIFLICKER_DISABLE;
    }

    if (strcmp(str, "50hz") == 0) {
        return IMP_CONTROL_ANTIFLICKER_50HZ;
    } else if (strcmp(str, "60hz") == 0) {
        return IMP_CONTROL_ANTIFLICKER_60HZ;
    } else if (strcmp(str, "disable") == 0) {
        return IMP_CONTROL_ANTIFLICKER_DISABLE;
    } else {
        return IMP_CONTROL_ANTIFLICKER_DISABLE;
    }
}

const char* imp_control_wb_mode_to_string(imp_control_wb_mode_t mode)
{
    switch (mode) {
        case IMP_CONTROL_WB_AUTO:
            return "auto";
        case IMP_CONTROL_WB_MANUAL:
            return "manual";
        default:
            return "auto";
    }
}

imp_control_wb_mode_t imp_control_string_to_wb_mode(const char* str)
{
    if (!str) {
        return IMP_CONTROL_WB_AUTO;
    }

    if (strcmp(str, "manual") == 0) {
        return IMP_CONTROL_WB_MANUAL;
    } else if (strcmp(str, "auto") == 0) {
        return IMP_CONTROL_WB_AUTO;
    } else {
        return IMP_CONTROL_WB_AUTO;
    }
}

/* Module registration */
module_info_t imp_control_info = {
    .name = IMP_CONTROL_NAME,
    .version = IMP_CONTROL_VERSION,
    .description = "IMP image quality control module with API endpoints",
    .state = MODULE_STATE_UNREGISTERED,
    .module_data = &g_imp_control_state,

    /* Lifecycle callbacks */
    .init = imp_control_init,
    .start = imp_control_start,
    .stop = imp_control_stop,
    .cleanup = imp_control_cleanup,

    /* Configuration */
    .config_parse = imp_control_config_parse,
    .config_validate = NULL,
    .config_free = NULL,
    .config_size = sizeof(imp_control_config_t),

    /* RTSP integration - not needed for IMP control module */
    .rtsp_setup = NULL,
    .rtsp_frame_callback = NULL,
    .rtsp_cleanup = NULL,

    /* Statistics - not implemented */
    .get_stats = NULL
};

/* Auto-register module at startup */
MODULE_REGISTER(imp_control_info);

/* Module registration function for manual registration */
int register_imp_control(void)
{
    return module_register(&imp_control_info);
}

#ifdef ENABLE_HTTP
/* Handler wrapper functions */
static void handle_get_params_wrapper(int client_socket, const char* request) {
    imp_control_handle_get_params(client_socket);
}

static void handle_set_params_wrapper(int client_socket, const char* request) {
    imp_control_handle_set_params(client_socket, request);
}

static void handle_reset_params_wrapper(int client_socket, const char* request) {
    imp_control_handle_reset_params(client_socket);
}

static void handle_save_params_wrapper(int client_socket, const char* request) {
    imp_control_handle_save_params(client_socket);
}

/* Route registration for HTTP module */
int imp_control_register_routes(void)
{
    /* Define routes */
    static const http_route_t imp_control_routes[] = {
        {"/imp/params.json", HTTP_METHOD_GET, handle_get_params_wrapper, "imp_control", "Get IMP parameters (JSON)"},
        {"/imp/params", HTTP_METHOD_POST, handle_set_params_wrapper, "imp_control", "Set IMP parameters (POST JSON)"},
        {"/imp/reset", HTTP_METHOD_POST, handle_reset_params_wrapper, "imp_control", "Reset IMP parameters to defaults (POST)"},
        {"/imp/save", HTTP_METHOD_POST, handle_save_params_wrapper, "imp_control", "Save current IMP parameters to configuration (POST)"},
    };

    /* Register all routes */
    int ret = http_router_register_routes(imp_control_routes,
                                         sizeof(imp_control_routes) / sizeof(imp_control_routes[0]));
    if (ret < 0) {
        IMP_LOG_ERR(TAG, "Failed to register HTTP routes");
        return ret;
    }

    IMP_LOG_INFO(TAG, "Registered %zu HTTP routes",
                 sizeof(imp_control_routes) / sizeof(imp_control_routes[0]));
    return 0;
}
#endif
