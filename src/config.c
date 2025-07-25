/*
 * config.c - Streamer Configuration
 * This file contains the configuration management module for Thingino Streamer
 * Copyright (C) 2025 Paul Philippov, Thingino Project
 */

#include "config.h"
#include "common.h"

#include <errno.h>
#include <libgen.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <net/if.h>

#include <json-c/json.h>
#include <sys/stat.h>
#include <sys/types.h>

#define TAG "CONFIG"



/* Configuration monitoring thread variables */
static pthread_t config_monitor_thread;
static volatile bool config_monitoring_active = false;
static streamer_config_t* monitored_config = NULL;

#define CONFIG_FILE_NAME "streamer.json"

/* Parse RGBA hex color string to uint32_t */
static uint32_t parse_rgba_color(const char* color_str)
{
    if (!color_str) {
        goto error;
    }

    /* Handle hex format: #RRGGBBAA or #RRGGBB */
    if (color_str[0] == '#') {
        char* endptr;
        uint32_t color = strtoul(color_str + 1, &endptr, 16);

        if (*endptr != '\0') {
            goto error;
        }

        /* If only 6 digits (RGB), add full alpha */
        if (strlen(color_str) == 7) {
            color = (color << 8) | 0xFF;
        }

        IMP_LOG_DBG(TAG, "Parsed hex color '%s' as 0x%08X", color_str, color);
        return color;
    }

error:
    IMP_LOG_WARN(TAG, "Invalid color format: %s, using default white", color_str ? color_str : "NULL");
    return 0xFFFFFFFF;
}

/* Function declarations */
static int config_parse_json(streamer_config_t* config);
static char* config_get_exe_dir_path(const char* filename);

/* Global configuration instance */
streamer_config_t* g_config = NULL;

streamer_config_t* config_create_new(void)
{
    streamer_config_t* config = calloc(1, sizeof(streamer_config_t));
    if (!config) {
        IMP_LOG_ERR(TAG, "Failed to allocate configuration structure");
        return NULL;
    }

    /* Set snapshot fallback defaults */
    snapshot_fallback_set_default_config(&config->snapshot_fallback);

    char* exe_config_path = config_get_exe_dir_path(CONFIG_FILE_NAME);
    if (exe_config_path) {
        if (access(exe_config_path, R_OK) == 0) {
            strcpy(config->file_path, exe_config_path);
            IMP_LOG_DBG(TAG, "Using config file %s", exe_config_path);
            return config;
        }
    }

    char fallback_path[PATH_MAX];
    snprintf(fallback_path, sizeof(fallback_path), "/etc/%s", CONFIG_FILE_NAME);
    if (access(fallback_path, R_OK) == 0) {
        strcpy(config->file_path, fallback_path);
        IMP_LOG_DBG(TAG, "Using config file %s", fallback_path);
        return config;
    }

    IMP_LOG_ERR(TAG, "Configuration file not found! Checked:");
    if (exe_config_path) {
        IMP_LOG_ERR(TAG, "  - %s", exe_config_path);
    }

    IMP_LOG_ERR(TAG, "  - %s", fallback_path);
    IMP_LOG_ERR(TAG, "Configuration file is mandatory for operation");
    free(config);
    return NULL;
}

void config_destroy(streamer_config_t* config)
{
    if (!config)
        return;

    if (config->json_config) {
        json_object_put(config->json_config);
        config->json_config = NULL;
    }

    /* Free dynamically allocated streams array */
    if (config->streams) {
        free(config->streams);
        config->streams = NULL;
        config->stream_count = 0;
    }

    free(config);
}

/* Initialize and load complete configuration */
int config_init_and_load(void)
{
    /* Create new configuration instance */
    g_config = config_create_new();
    if (!g_config) {
        IMP_LOG_ERR(TAG, "Failed to create configuration");
        return -1;
    }

    /* Load configuration from file */
    if (config_load(g_config) != 0) {
        IMP_LOG_ERR(TAG, "Failed to load configuration");
        config_destroy(g_config);
        g_config = NULL;
        return -1;
    }

    /* Configuration can be viewed via /config.json endpoint */

    /* Start configuration monitoring for auto-reload */
    if (config_start_monitoring(g_config) != 0) {
        IMP_LOG_WARN(TAG, "Failed to start configuration monitoring, no auto-reload");
    }

    IMP_LOG_INFO(TAG, "Configuration initialized successfully");
    return 0;
}

/* Helper functions for JSON parsing */
static void parse_string_field(json_object* parent, const char* key, char* dest, size_t dest_size)
{
    json_object* obj = NULL;
    if (json_object_object_get_ex(parent, key, &obj)) {
        const char* value = json_object_get_string(obj);
        if (value) {
            strncpy(dest, value, dest_size - 1);
            dest[dest_size - 1] = '\0';
        }
    }
}

static void parse_int_field(json_object* parent, const char* key, int* dest)
{
    json_object* obj = NULL;
    if (json_object_object_get_ex(parent, key, &obj)) {
        *dest = json_object_get_int(obj);
    }
}

static void parse_bool_field(json_object* parent, const char* key, bool* dest)
{
    json_object* obj = NULL;
    if (json_object_object_get_ex(parent, key, &obj)) {
        *dest = json_object_get_boolean(obj);
    }
}

static void parse_color_field(json_object* parent, const char* key, uint32_t* dest)
{
    json_object* obj = NULL;
    if (json_object_object_get_ex(parent, key, &obj)) {
        if (json_object_is_type(obj, json_type_string)) {
            /* Parse as hex string */
            const char* color_str = json_object_get_string(obj);
            *dest = parse_rgba_color(color_str);
        }
    }
}

/* Parse resolution string like "1920x1080" into width and height */
static void parse_resolution_field(json_object* parent, const char* key, int* width, int* height, char* resolution_str, size_t resolution_size)
{
    json_object* obj = NULL;
    if (json_object_object_get_ex(parent, key, &obj)) {
        const char* resolution = json_object_get_string(obj);
        if (resolution) {
            /* Store the original string */
            strncpy(resolution_str, resolution, resolution_size - 1);
            resolution_str[resolution_size - 1] = '\0';

            /* Parse width and height */
            if (sscanf(resolution, "%dx%d", width, height) == 2) {
                IMP_LOG_DBG(TAG, "Parsed resolution '%s' as %dx%d", resolution, *width, *height);
            } else {
                IMP_LOG_WARN(TAG, "Invalid resolution format: %s", resolution);
                *width = 1920;  /* Default fallback */
                *height = 1080;
            }
        }
    }
}

/* Parse JPEG configuration */
static void parse_jpeg_config(json_object* jpeg_obj, jpeg_config_t* jpeg)
{
    if (!jpeg_obj || !jpeg)
        return;

    parse_bool_field(jpeg_obj, "enabled", &jpeg->enabled);
    parse_int_field(jpeg_obj, "jpeg_channel", &jpeg->jpeg_channel);
    parse_int_field(jpeg_obj, "jpeg_idle_fps", &jpeg->jpeg_idle_fps);
    parse_string_field(jpeg_obj, "jpeg_path", jpeg->jpeg_path, sizeof(jpeg->jpeg_path));
    parse_int_field(jpeg_obj, "jpeg_quality", &jpeg->jpeg_quality);
    parse_int_field(jpeg_obj, "jpeg_refresh", &jpeg->jpeg_refresh);
}

/* Parse stream configuration */
static void parse_stream_config(json_object* stream_obj, stream_config_t* stream)
{
    if (!stream_obj || !stream)
        return;

    parse_bool_field(stream_obj, "enabled", &stream->enabled);
    parse_resolution_field(stream_obj, "resolution", &stream->width, &stream->height, stream->resolution, sizeof(stream->resolution));
    parse_int_field(stream_obj, "bitrate", &stream->bitrate);

    parse_string_field(stream_obj, "format", stream->format, sizeof(stream->format));
    parse_string_field(stream_obj, "rtsp_endpoint", stream->rtsp_endpoint, sizeof(stream->rtsp_endpoint));
    parse_string_field(stream_obj, "rtsp_info", stream->rtsp_info, sizeof(stream->rtsp_info));

    /* Parse framesource configuration */
    json_object* framesource_obj = NULL;
    if (json_object_object_get_ex(stream_obj, "framesource", &framesource_obj)) {
        parse_string_field(framesource_obj, "pixel_format", stream->framesource.pixel_format, sizeof(stream->framesource.pixel_format));
        parse_int_field(framesource_obj, "frame_rate_num", &stream->framesource.frame_rate_num);
        parse_int_field(framesource_obj, "frame_rate_den", &stream->framesource.frame_rate_den);
        parse_int_field(framesource_obj, "buffer_count", &stream->framesource.buffer_count);
        parse_string_field(framesource_obj, "channel_type", stream->framesource.channel_type, sizeof(stream->framesource.channel_type));
        parse_int_field(framesource_obj, "picture_width", &stream->framesource.picture_width);
        parse_int_field(framesource_obj, "picture_height", &stream->framesource.picture_height);

        /* Parse crop configuration */
        json_object* crop_obj = NULL;
        if (json_object_object_get_ex(framesource_obj, "crop", &crop_obj)) {
            parse_bool_field(crop_obj, "enabled", &stream->framesource.crop.enabled);
            parse_int_field(crop_obj, "top", &stream->framesource.crop.top);
            parse_int_field(crop_obj, "left", &stream->framesource.crop.left);
            parse_int_field(crop_obj, "width", &stream->framesource.crop.width);
            parse_int_field(crop_obj, "height", &stream->framesource.crop.height);
        }

        /* Parse scaler configuration */
        json_object* scaler_obj = NULL;
        if (json_object_object_get_ex(framesource_obj, "scaler", &scaler_obj)) {
            parse_bool_field(scaler_obj, "enabled", &stream->framesource.scaler.enabled);
            parse_int_field(scaler_obj, "output_width", &stream->framesource.scaler.output_width);
            parse_int_field(scaler_obj, "output_height", &stream->framesource.scaler.output_height);
        }
    }
}

/* Parse JSON configuration and update config values */
static int config_parse_json(streamer_config_t* config)
{
    if (!config || !config->json_config) {
        IMP_LOG_ERR(TAG, "Invalid config or JSON object for parsing");
        return -1;
    }

    IMP_LOG_DBG(TAG, "Parsing JSON configuration values...");

    /* Parse general configuration */
    json_object* general_obj = NULL;
    if (json_object_object_get_ex(config->json_config, "general", &general_obj)) {
        parse_string_field(general_obj, "loglevel", config->general.loglevel, sizeof(config->general.loglevel));
        parse_int_field(general_obj, "osd_pool_size", &config->general.osd_pool_size);
        parse_int_field(general_obj, "imp_polling_timeout", &config->general.imp_polling_timeout);
        parse_bool_field(general_obj, "memory_monitoring_enabled", &config->general.memory_monitoring_enabled);
        parse_bool_field(general_obj, "allocation_tracking_enabled", &config->general.allocation_tracking_enabled);
        parse_bool_field(general_obj, "zero_copy_enabled", &config->general.zero_copy_enabled);
        parse_int_field(general_obj, "zero_copy_buffer_pool_size", &config->general.zero_copy_buffer_pool_size);
        parse_string_field(general_obj, "server_ip", config->general.server_ip, sizeof(config->general.server_ip));
        parse_int_field(general_obj, "http_port", &config->general.http_port);
    }

    /* Set default values if not specified */
    if (strlen(config->general.server_ip) == 0) {
        /* Try to get local IP address */
        get_device_ip_address(config->general.server_ip, sizeof(config->general.server_ip));
    }

    /* Set default HTTP port if not specified */
    if (config->general.http_port == 0) {
        config->general.http_port = 8080;
    }

    /* Parse sensor configuration */
    json_object* sensor_obj = NULL;
    if (json_object_object_get_ex(config->json_config, "sensor", &sensor_obj)) {
        parse_int_field(sensor_obj, "fps", &config->sensor.fps);
    }

    /* Parse JPEG configuration */
    json_object* jpeg_obj = NULL;
    if (json_object_object_get_ex(config->json_config, "jpeg", &jpeg_obj)) {
        parse_jpeg_config(jpeg_obj, &config->jpeg);
        IMP_LOG_DBG(TAG,
                    "Parsed JPEG config: enabled=%s, quality=%d, channel=%d",
                    config->jpeg.enabled ? "true" : "false",
                    config->jpeg.jpeg_quality,
                    config->jpeg.jpeg_channel);
    }

    /* Parse streams array from root level (streams are core system config) */
    json_object* streams_array = NULL;
    if (json_object_object_get_ex(config->json_config, "streams", &streams_array)) {
        IMP_LOG_DBG(TAG, "Found streams array at root level");
        if (streams_array) {
            int array_len = json_object_array_length(streams_array);
            IMP_LOG_DBG(TAG, "Found streams array with %d elements", array_len);
            if (array_len > 0) {
                config->streams = calloc(array_len, sizeof(stream_config_t));
                if (config->streams) {
                    config->stream_count = array_len;
                    IMP_LOG_DBG(TAG, "Allocated streams array for %d streams", array_len);
                    for (int i = 0; i < array_len; i++) {
                        json_object* stream_obj = json_object_array_get_idx(streams_array, i);
                        if (stream_obj) {
                            parse_stream_config(stream_obj, &config->streams[i]);
                            IMP_LOG_DBG(TAG,
                                        "Parsed stream[%d]: %s, endpoint=%s",
                                        i,
                                        config->streams[i].resolution,
                                        config->streams[i].rtsp_endpoint);
                        }
                    }
                } else {
                    IMP_LOG_ERR(TAG, "Failed to allocate memory for streams array");
                    config->stream_count = 0;
                }
            } else {
                IMP_LOG_WARN(TAG, "Streams array is empty");
                config->stream_count = 0;
            }
        } else {
            IMP_LOG_WARN(TAG, "No 'streams' array found in configuration");
            config->stream_count = 0;
        }
    } else {
        IMP_LOG_WARN(TAG, "No streams configuration found");
        config->stream_count = 0;
    }

    /* Parse system info configuration */
    json_object* sysinfo_obj = NULL;
    if (json_object_object_get_ex(config->json_config, "sysinfo", &sysinfo_obj)) {
        parse_bool_field(sysinfo_obj, "enabled", &config->sysinfo.enabled);
        parse_int_field(sysinfo_obj, "update_interval", &config->sysinfo.update_interval);
        parse_bool_field(sysinfo_obj, "include_memory", &config->sysinfo.include_memory);
        parse_bool_field(sysinfo_obj, "include_load", &config->sysinfo.include_load);
    }

    /* Parse snapshot fallback configuration */
    json_object* snapshot_fallback_obj = NULL;
    if (json_object_object_get_ex(config->json_config, "snapshot_fallback", &snapshot_fallback_obj)) {
        parse_bool_field(snapshot_fallback_obj, "enabled", &config->snapshot_fallback.enabled);
        parse_string_field(snapshot_fallback_obj, "output_dir", config->snapshot_fallback.output_dir, sizeof(config->snapshot_fallback.output_dir));
        parse_int_field(snapshot_fallback_obj, "update_interval_ms", &config->snapshot_fallback.update_interval_ms);
        parse_bool_field(snapshot_fallback_obj, "overwrite_existing", &config->snapshot_fallback.overwrite_existing);
        parse_int_field(snapshot_fallback_obj, "max_file_age_seconds", &config->snapshot_fallback.max_file_age_seconds);
    }

    IMP_LOG_DBG(TAG, "JSON configuration parsed successfully");
    return 0;
}

int config_load(streamer_config_t* config)
{
    if (!config) {
        IMP_LOG_ERR(TAG, "Invalid configuration pointer");
        return -1;
    }

    if (access(config->file_path, R_OK) != 0) {
        IMP_LOG_ERR(TAG, "Configuration file not found or not readable: %s", config->file_path);
        return -1;
    }

    /* Get file modification time for monitoring */
    struct stat file_stat;
    if (stat(config->file_path, &file_stat) == 0) {
        config->last_modified_time = file_stat.st_mtime;
    } else {
        config->last_modified_time = 0;
    }

    IMP_LOG_INFO(TAG, "Loading configuration from: %s", config->file_path);

    config->json_config = json_object_from_file(config->file_path);
    if (!config->json_config) {
        IMP_LOG_ERR(TAG, "Failed to parse JSON configuration file: %s", config->file_path);
        return -1;
    }

    IMP_LOG_INFO(TAG, "JSON configuration loaded successfully");

    int parse_result = config_parse_json(config);
    if (parse_result < 0) {
        IMP_LOG_ERR(TAG, "Failed to parse JSON configuration values");
        return -1;
    }

    config->config_loaded = true;

    /* Validate sensor configuration */
    if (config->sensor.fps <= 0 || config->sensor.fps > 60) {
        IMP_LOG_ERR(TAG, "Invalid sensor fps: %d (must be 1-60)", config->sensor.fps);
        return -1;
    }

    /* Validate streams array */
    for (int i = 0; i < config->stream_count; i++) {
        stream_config_t* stream = &config->streams[i];

        if (stream->width <= 0 || stream->height <= 0) {
            IMP_LOG_ERR(TAG,
                        "Invalid stream[%d] resolution: %dx%d",
                        i,
                        stream->width,
                        stream->height);
            return -1;
        }
    }

    /* Log all configured streams */
    for (int i = 0; i < config->stream_count; i++) {
        stream_config_t* stream = &config->streams[i];
        IMP_LOG_DBG(TAG,
                    "  Stream[%d]: %s, bitrate=%dkbps, endpoint='%s'",
                    i,
                    stream->resolution,
                    stream->bitrate,
                    stream->rtsp_endpoint);
    }

    return 0;
}

int config_reload(streamer_config_t* config)
{
    if (!config) {
        IMP_LOG_ERR(TAG, "Invalid configuration pointer");
        return -1;
    }

    if (config->json_config) {
        json_object_put(config->json_config);
        config->json_config = NULL;
    }

    return config_load(config);
}

int config_validate(streamer_config_t* config)
{
    if (!config) {
        IMP_LOG_ERR(TAG, "Invalid configuration pointer");
        return -1;
    }

    /* Validate all streams */
    for (int i = 0; i < config->stream_count; i++) {
        stream_config_t* stream = &config->streams[i];
        if (stream->width <= 0 || stream->height <= 0) {
            IMP_LOG_ERR(TAG,
                        "Invalid stream[%d] resolution: %dx%d",
                        i,
                        stream->width,
                        stream->height);
            return -1;
        }
    }

    return 0;
}

/* Simple getter implementations with JSON path lookup */
const char* config_get_string(streamer_config_t* config, const char* path, const char* default_value)
{
    if (!config || !config->json_config || !path) {
        return default_value;
    }

    json_object* obj = NULL;
    if (json_object_object_get_ex(config->json_config, path, &obj)) {
        const char* value = json_object_get_string(obj);
        return value ? value : default_value;
    }

    return default_value;
}

int config_get_int(streamer_config_t* config, const char* path, int default_value)
{
    if (!config || !config->json_config || !path) {
        return default_value;
    }

    json_object* obj = NULL;
    if (json_object_object_get_ex(config->json_config, path, &obj)) {
        return json_object_get_int(obj);
    }

    return default_value;
}

bool config_get_bool(streamer_config_t* config, const char* path, bool default_value)
{
    if (!config || !config->json_config || !path) {
        return default_value;
    }

    json_object* obj = NULL;
    if (json_object_object_get_ex(config->json_config, path, &obj)) {
        return json_object_get_boolean(obj);
    }

    return default_value;
}

/* Get full path to file in same directory as executable */
static char* config_get_exe_dir_path(const char* filename)
{
    static char full_path[PATH_MAX];
    char exe_path[PATH_MAX];
    char* exe_dir;

    /* Get path to current executable */
    ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
    if (len == -1) {
        IMP_LOG_ERR(TAG, "Failed to get executable path: %s", strerror(errno));
        return NULL;
    }
    exe_path[len] = '\0';

    /* Get directory part */
    exe_dir = dirname(exe_path);

    /* Construct full path */
    snprintf(full_path, sizeof(full_path), "%s/%s", exe_dir, filename);

    return full_path;
}

/* Check if configuration file has been modified and reload if needed */
int config_check_and_reload(streamer_config_t* config)
{
    if (!config) {
        return -1;
    }

    struct stat file_stat;
    if (stat(config->file_path, &file_stat) != 0) {
        IMP_LOG_ERR(TAG, "Failed to stat config file: %s", config->file_path);
        return -1;
    }

    /* Check if file has been modified */
    if (file_stat.st_mtime != config->last_modified_time) {
        IMP_LOG_INFO(TAG, "Configuration file changed, reloading: %s", config->file_path);

        /* Reload configuration */
        if (config_reload(config) == 0) {
            IMP_LOG_INFO(TAG, "Configuration reloaded successfully");
            return 1; /* Config was reloaded */
        } else {
            IMP_LOG_ERR(TAG, "Failed to reload configuration");
            return -1;
        }
    }

    return 0; /* No change */
}

/* Configuration monitoring thread function */
static void* config_monitor_thread_func(void* arg)
{
    streamer_config_t* config = (streamer_config_t*) arg;

    IMP_LOG_INFO(TAG, "Configuration monitoring started for: %s", config->file_path);

    while (config_monitoring_active) {
        /* Check for config file changes every 2 seconds */
        sleep(2);

        if (!config_monitoring_active) {
            break;
        }

        int result = config_check_and_reload(config);
        if (result < 0) {
            IMP_LOG_WARN(TAG, "Config monitoring check failed, continuing...");
        }
    }

    IMP_LOG_INFO(TAG, "Configuration monitoring stopped");
    return NULL;
}

/* Start configuration file monitoring thread */
int config_start_monitoring(streamer_config_t* config)
{
    if (!config) {
        IMP_LOG_ERR(TAG, "Invalid configuration pointer for monitoring");
        return -1;
    }

    if (config_monitoring_active) {
        IMP_LOG_WARN(TAG, "Configuration monitoring already active");
        return 0;
    }

    monitored_config = config;
    config_monitoring_active = true;

    int ret = pthread_create(&config_monitor_thread, NULL, config_monitor_thread_func, config);
    if (ret != 0) {
        IMP_LOG_ERR(TAG, "Failed to create config monitoring thread: %s", strerror(ret));
        config_monitoring_active = false;
        monitored_config = NULL;
        return -1;
    }

    IMP_LOG_INFO(TAG, "Configuration monitoring thread started");
    return 0;
}

/* Stop configuration file monitoring thread */
void config_stop_monitoring(void)
{
    if (!config_monitoring_active) {
        return;
    }

    IMP_LOG_INFO(TAG, "Stopping configuration monitoring...");
    config_monitoring_active = false;

    /* Wait for thread to finish */
    pthread_join(config_monitor_thread, NULL);

    monitored_config = NULL;
    IMP_LOG_INFO(TAG, "Configuration monitoring stopped");
}
