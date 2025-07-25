/*
 * motion_module.c - Motion Detection Module Implementation
 * IVS-based motion detection for Thingino Streamer
 * Copyright (C) 2025 Paul Philippov, Thingino Project
 */

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <imp/imp_common.h>
#include <imp/imp_framesource.h>
#include <imp/imp_ivs.h>
#include <imp/imp_ivs_move.h>
#include <imp/imp_system.h>
#include <sys/wait.h>

#include "motion_module.h"
#include "../../common.h"
#include "../../config.h"

#define TAG "MOTION"

/* Module state */
static motion_module_state_t g_motion_state = {0};

/* Forward declarations */
static void* motion_detection_thread(void* arg);
static int motion_setup_ivs(void);
static int motion_cleanup_ivs(void);
static int motion_process_result(IMP_IVS_MoveOutput* result);
static int motion_execute_script(const char* action);

/* Configuration parsing helper functions */
static int parse_zone_config(json_object* json, motion_module_config_t* config);
static int load_roi_config(motion_module_config_t* config);
static void motion_set_default_config(motion_module_config_t* config);
static int motion_get_channel_dimensions(int channel, int* width, int* height);

/* Motion detection thread function */
static void* motion_detection_thread(void* arg)
{
    (void)arg;

    IMP_LOG_INFO(TAG, "Motion detection thread started, waiting for initialization");

    /* Wait for initialization period */
    if (g_motion_state.config.init_time > 0) {
        IMP_LOG_INFO(TAG, "Waiting %d seconds for initialization", g_motion_state.config.init_time);
        sleep(g_motion_state.config.init_time);
    }

    IMP_LOG_INFO(TAG, "Initialization complete, starting motion detection");

    while (!g_motion_state.thread_should_exit) {
        /* Only process when RTSP clients are connected (if RTSP server is available) */
        bool should_process = true;
        if (g_motion_state.rtsp_server) {
            // Note: This would need rtsp_server_get_client_count function
            // For now, always process
            should_process = true;
        }

        if (!should_process) {
            usleep(100000); /* Sleep 100ms when no clients */
            continue;
        }

        /* Poll for IVS results */
        int ret = IMP_IVS_PollingResult(g_motion_state.ivs_channel_id, g_motion_state.config.ivs_polling_timeout);
        if (ret < 0) {
            if (g_motion_state.thread_should_exit) {
                IMP_LOG_INFO(TAG, "Motion detection thread exiting");
                break;
            }
            /* Timeout or error - continue polling */
            IMP_LOG_ERR(TAG, "IMP_IVS_PollingResult failed: %d", ret);
            continue;
        }

        /* Get motion detection result */
        IMP_IVS_MoveOutput* result = NULL;
        ret = IMP_IVS_GetResult(g_motion_state.ivs_channel_id, (void**)&result);
        if (ret < 0) {
            IMP_LOG_ERR(TAG, "IMP_IVS_GetResult failed: %d", ret);
            continue;
        }

        if (result) {
            /* Process motion detection result */
            motion_process_result(result);

            /* Release result */
            ret = IMP_IVS_ReleaseResult(g_motion_state.ivs_channel_id, (void*)result);
            if (ret < 0) {
                IMP_LOG_ERR(TAG, "IMP_IVS_ReleaseResult failed: %d", ret);
            }
        }

        /* Update statistics */
        pthread_mutex_lock(&g_motion_state.mutex);
        g_motion_state.frames_processed++;
        pthread_mutex_unlock(&g_motion_state.mutex);
    }

    IMP_LOG_INFO(TAG, "Motion detection thread exiting");
    return NULL;
}

/* Setup IVS motion detection */
static int motion_setup_ivs(void)
{
    int ret;

    IMP_LOG_INFO(TAG, "Setting up IVS motion detection");

    /* Create IVS group */
    g_motion_state.ivs_group_id = 0; /* Use group 0 */
    ret = IMP_IVS_CreateGroup(g_motion_state.ivs_group_id);
    if (ret < 0) {
        IMP_LOG_ERR(TAG, "IMP_IVS_CreateGroup(%d) failed: %d", g_motion_state.ivs_group_id, ret);
        return -1;
    }

    /* Setup motion detection parameters */
    IMP_IVS_MoveParam move_param;
    memset(&move_param, 0, sizeof(IMP_IVS_MoveParam));

    /* Configure frame info */
    move_param.frameInfo.width = g_motion_state.config.frame_width;
    move_param.frameInfo.height = g_motion_state.config.frame_height;

    /* Configure detection parameters */
    move_param.skipFrameCnt = g_motion_state.config.skip_frame_count;

    /* Count only "include" zones for IVS */
    int include_zone_count = 0;
    for (int i = 0; i < g_motion_state.config.zone_count && include_zone_count < IMP_IVS_MOVE_MAX_ROI_CNT; i++) {
        if (strcmp(g_motion_state.config.zones[i].type, "include") == 0) {
            include_zone_count++;
        }
    }
    move_param.roiRectCnt = include_zone_count;

    /* Setup include zones for IVS motion detection */
    int roi_index = 0;
    for (int i = 0; i < g_motion_state.config.zone_count && roi_index < IMP_IVS_MOVE_MAX_ROI_CNT; i++) {
        /* Only process "include" zones for IVS */
        if (strcmp(g_motion_state.config.zones[i].type, "include") != 0) {
            continue;
        }

        move_param.sense[roi_index] = g_motion_state.config.sensitivity;
        move_param.roiRect[roi_index].p0.x = g_motion_state.config.zones[i].x;
        move_param.roiRect[roi_index].p0.y = g_motion_state.config.zones[i].y;
        move_param.roiRect[roi_index].p1.x = g_motion_state.config.zones[i].x + g_motion_state.config.zones[i].width;
        move_param.roiRect[roi_index].p1.y = g_motion_state.config.zones[i].y + g_motion_state.config.zones[i].height;

        IMP_LOG_INFO(TAG, "Zone[%d] '%s' -> IVS ROI[%d]: (%d,%d) to (%d,%d), sensitivity=%d",
                     i, g_motion_state.config.zones[i].name, roi_index,
                     move_param.roiRect[roi_index].p0.x, move_param.roiRect[roi_index].p0.y,
                     move_param.roiRect[roi_index].p1.x, move_param.roiRect[roi_index].p1.y,
                     move_param.sense[roi_index]);

        roi_index++;
    }

    IMP_LOG_INFO(TAG, "Configured %d include zones for IVS motion detection", include_zone_count);

    /* Create motion detection interface */
    g_motion_state.ivs_interface = IMP_IVS_CreateMoveInterface(&move_param);
    if (!g_motion_state.ivs_interface) {
        IMP_LOG_ERR(TAG, "IMP_IVS_CreateMoveInterface failed");
        IMP_IVS_DestroyGroup(g_motion_state.ivs_group_id);
        return -1;
    }

    /* Create IVS channel */
    g_motion_state.ivs_channel_id = 0; /* Use channel 0 */
    ret = IMP_IVS_CreateChn(g_motion_state.ivs_channel_id, g_motion_state.ivs_interface);
    if (ret < 0) {
        IMP_LOG_ERR(TAG, "IMP_IVS_CreateChn(%d) failed: %d", g_motion_state.ivs_channel_id, ret);
        IMP_IVS_DestroyMoveInterface(g_motion_state.ivs_interface);
        IMP_IVS_DestroyGroup(g_motion_state.ivs_group_id);
        return -1;
    }

    /* Register IVS channel to group */
    ret = IMP_IVS_RegisterChn(g_motion_state.ivs_group_id, g_motion_state.ivs_channel_id);
    if (ret < 0) {
        IMP_LOG_ERR(TAG, "IMP_IVS_RegisterChn(%d, %d) failed: %d",
                     g_motion_state.ivs_group_id, g_motion_state.ivs_channel_id, ret);
        IMP_IVS_DestroyChn(g_motion_state.ivs_channel_id);
        IMP_IVS_DestroyMoveInterface(g_motion_state.ivs_interface);
        IMP_IVS_DestroyGroup(g_motion_state.ivs_group_id);
        return -1;
    }

    /* Bind frame source to IVS group */
    IMPCell framesource_cell = {DEV_ID_FS, g_motion_state.config.monitor_stream, 0};
    IMPCell ivs_cell = {DEV_ID_IVS, g_motion_state.ivs_group_id, 0};

    ret = IMP_System_Bind(&framesource_cell, &ivs_cell);
    if (ret < 0) {
        IMP_LOG_ERR(TAG, "IMP_System_Bind(FS->IVS) failed: %d", ret);
        motion_cleanup_ivs();
        return -1;
    }

    IMP_LOG_INFO(TAG, "Bound frame source channel %d to IVS group %d",
                 g_motion_state.config.monitor_stream, g_motion_state.ivs_group_id);

    /* Start IVS */
    ret = IMP_IVS_StartRecvPic(g_motion_state.ivs_channel_id);
    if (ret < 0) {
        IMP_LOG_ERR(TAG, "IMP_IVS_StartRecvPic(%d) failed: %d", g_motion_state.ivs_channel_id, ret);
        /* Unbind on failure */
        IMP_System_UnBind(&framesource_cell, &ivs_cell);
        motion_cleanup_ivs();
        return -1;
    }

    IMP_LOG_INFO(TAG, "IVS motion detection setup completed successfully");
    return 0;
}

/* Cleanup IVS motion detection */
static int motion_cleanup_ivs(void)
{
    int ret = 0;

    IMP_LOG_INFO(TAG, "Cleaning up IVS motion detection");

    /* Stop receiving pictures */
    if (IMP_IVS_StopRecvPic(g_motion_state.ivs_channel_id) < 0) {
        IMP_LOG_ERR(TAG, "IMP_IVS_StopRecvPic failed");
        ret = -1;
    }

    /* Unbind frame source from IVS group */
    IMPCell framesource_cell = {DEV_ID_FS, g_motion_state.config.monitor_stream, 0};
    IMPCell ivs_cell = {DEV_ID_IVS, g_motion_state.ivs_group_id, 0};

    if (IMP_System_UnBind(&framesource_cell, &ivs_cell) < 0) {
        IMP_LOG_ERR(TAG, "IMP_System_UnBind(FS->IVS) failed");
        ret = -1;
    }

    /* Unregister channel from group */
    if (IMP_IVS_UnRegisterChn(g_motion_state.ivs_channel_id) < 0) {
        IMP_LOG_ERR(TAG, "IMP_IVS_UnRegisterChn failed");
        ret = -1;
    }

    /* Destroy channel */
    if (IMP_IVS_DestroyChn(g_motion_state.ivs_channel_id) < 0) {
        IMP_LOG_ERR(TAG, "IMP_IVS_DestroyChn failed");
        ret = -1;
    }

    /* Destroy interface */
    if (g_motion_state.ivs_interface) {
        IMP_IVS_DestroyMoveInterface(g_motion_state.ivs_interface);
        g_motion_state.ivs_interface = NULL;
    }

    /* Destroy group */
    if (IMP_IVS_DestroyGroup(g_motion_state.ivs_group_id) < 0) {
        IMP_LOG_ERR(TAG, "IMP_IVS_DestroyGroup failed");
        ret = -1;
    }

    return ret;
}

/* Process motion detection result */
static int motion_process_result(IMP_IVS_MoveOutput* result)
{
    if (!result) {
        return -1;
    }

    bool motion_in_any_roi = false;
    unsigned long current_time = get_monotonic_time_us() / 1000000; /* Convert to seconds */

    /* Check motion in all include zones (mapped to IVS ROI regions) */
    int roi_index = 0;
    for (int i = 0; i < g_motion_state.config.zone_count && roi_index < IMP_IVS_MOVE_MAX_ROI_CNT; i++) {
        /* Only check "include" zones */
        if (strcmp(g_motion_state.config.zones[i].type, "include") != 0) {
            continue;
        }

        if (result->retRoi[roi_index] == 1) {
            motion_in_any_roi = true;
            IMP_LOG_DBG(TAG, "Motion detected in zone '%s' (IVS ROI[%d])",
                        g_motion_state.config.zones[i].name, roi_index);
            break;
        }
        roi_index++;
    }

    pthread_mutex_lock(&g_motion_state.mutex);

    if (motion_in_any_roi) {
        /* Check cooldown period */
        if (g_motion_state.last_motion_time > 0 &&
            (current_time - g_motion_state.last_motion_time) < g_motion_state.config.cooldown_time) {
            /* Still in cooldown period */
            pthread_mutex_unlock(&g_motion_state.mutex);
            return 0;
        }

        if (!g_motion_state.motion_detected) {
            /* New motion event */
            g_motion_state.motion_detected = true;
            g_motion_state.motion_start_script_executed = false;
            g_motion_state.motion_start_time = current_time;
            g_motion_state.motion_events++;

            IMP_LOG_INFO(TAG, "Motion detected! Event #%lu", g_motion_state.motion_events);
        }

        /* Execute motion start script once per motion event */
        if (!g_motion_state.motion_start_script_executed && strlen(g_motion_state.config.script_path) > 0) {
            g_motion_state.motion_start_script_executed = true;
            pthread_mutex_unlock(&g_motion_state.mutex);
            motion_execute_script("start");
            pthread_mutex_lock(&g_motion_state.mutex);
        }

        g_motion_state.last_motion_time = current_time;
    } else {
        /* No motion detected */
        if (g_motion_state.motion_detected) {
            /* Check minimum motion time */
            unsigned long motion_duration = current_time - g_motion_state.motion_start_time;
            if (motion_duration >= g_motion_state.config.min_time) {
                IMP_LOG_INFO(TAG, "Motion ended after %lu seconds", motion_duration);
                g_motion_state.motion_detected = false;

                /* Execute motion stop script */
                if (strlen(g_motion_state.config.script_path) > 0) {
                    pthread_mutex_unlock(&g_motion_state.mutex);
                    motion_execute_script("stop");
                    pthread_mutex_lock(&g_motion_state.mutex);
                }
            }
        }
    }

    pthread_mutex_unlock(&g_motion_state.mutex);
    return 0;
}

/* Execute motion detection script */
static int motion_execute_script(const char* action)
{
    if (strlen(g_motion_state.config.script_path) == 0 || !action) {
        return 0;
    }

    IMP_LOG_INFO(TAG, "Executing motion script: %s %s", g_motion_state.config.script_path, action);

    pid_t pid = fork();
    if (pid == 0) {
        /* Child process - execute script with action parameter */
        char command[512];
        snprintf(command, sizeof(command), "%s %s", g_motion_state.config.script_path, action);
        execl("/bin/sh", "sh", "-c", command, (char*)NULL);
        _exit(127); /* execl failed */
    } else if (pid > 0) {
        /* Parent process - wait for child to complete */
        int status;
        IMP_LOG_DBG(TAG, "Motion script started with PID %d (action: %s)", pid, action);

        /* Wait for script to complete (blocking wait with timeout) */
        pid_t wait_result = waitpid(pid, &status, 0);  /* Block until child completes */

        if (wait_result == pid) {
            if (WIFEXITED(status)) {
                int exit_code = WEXITSTATUS(status);
                if (exit_code == 0) {
                    IMP_LOG_DBG(TAG, "Motion script completed successfully (action: %s)", action);
                } else {
                    IMP_LOG_WARN(TAG, "Motion script exited with code %d (action: %s)", exit_code, action);
                }
            } else if (WIFSIGNALED(status)) {
                int signal = WTERMSIG(status);
                IMP_LOG_WARN(TAG, "Motion script terminated by signal %d (action: %s)", signal, action);
            }
        } else {
            IMP_LOG_ERR(TAG, "waitpid failed for motion script: %s", strerror(errno));
        }

        return 0;
    } else {
        /* Fork failed */
        IMP_LOG_ERR(TAG, "Failed to fork for motion script execution: %s", strerror(errno));
        return -1;
    }
}

/* Get frame dimensions from channel configuration */
static int motion_get_channel_dimensions(int channel, int* width, int* height)
{
    if (channel < 0 || channel >= FS_CHN_NUM) {
        IMP_LOG_ERR(TAG, "Invalid channel %d (must be 0-%d)", channel, FS_CHN_NUM - 1);
        return -1;
    }

    if (!width || !height) {
        IMP_LOG_ERR(TAG, "Invalid width/height pointers");
        return -1;
    }

    /* Try to get dimensions from global channel configuration */
    extern struct chn_conf chn[FS_CHN_NUM];

    /* Check if channel is enabled */
    if (!chn[channel].enable) {
        IMP_LOG_ERR(TAG, "Channel %d is not enabled", channel);
        return -1;
    }

    /* Get dimensions from frame source channel attributes */
    IMPFSChnAttr* fs_attr = &chn[channel].fs_chn_attr;

    /* Use scaler output dimensions if scaling is enabled */
    if (fs_attr->scaler.enable) {
        *width = fs_attr->scaler.outwidth;
        *height = fs_attr->scaler.outheight;
    } else {
        /* Use picture dimensions if no scaling */
        *width = fs_attr->picWidth;
        *height = fs_attr->picHeight;
    }

    IMP_LOG_INFO(TAG, "Channel %d dimensions: %dx%d (scaler %s)",
                 channel, *width, *height,
                 fs_attr->scaler.enable ? "enabled" : "disabled");

    return 0;
}

/* Set default configuration */
static void motion_set_default_config(motion_module_config_t* config)
{
    memset(config, 0, sizeof(motion_module_config_t));

    /* Default values based on existing motion.json */
    config->enabled = false;
    config->monitor_stream = 1;
    config->sensitivity = 1;
    config->skip_frame_count = 5;
    config->ivs_polling_timeout = 1000;

    /* Timing defaults */
    config->cooldown_time = 5;
    config->debounce_time = 0;
    config->init_time = 5;
    config->min_time = 1;
    config->post_time = 0;

    /* Frame defaults - will be overridden by channel detection */
    config->frame_width = 1920;
    config->frame_height = 1080;

    /* Zone defaults - will be loaded from roi.json or set to full frame */
    config->zone_count = 1;
    config->zones[0].id = 1;
    strncpy(config->zones[0].type, "include", sizeof(config->zones[0].type) - 1);
    strncpy(config->zones[0].name, "full_frame", sizeof(config->zones[0].name) - 1);
    config->zones[0].x = 0;
    config->zones[0].y = 0;
    config->zones[0].width = 0;  /* 0 means use full frame */
    config->zones[0].height = 0; /* 0 means use full frame */
    config->min_object_size = 5; /* Default 5% */

    /* Script path */
    strncpy(config->script_path, "/usr/sbin/motion", sizeof(config->script_path) - 1);
}

/* Parse coordinate string "x,y" */
static int parse_coordinate_string(const char* coord_str, int* x, int* y)
{
    if (!coord_str || !x || !y) {
        return -1;
    }

    if (sscanf(coord_str, "%d,%d", x, y) != 2) {
        IMP_LOG_ERR(TAG, "Invalid coordinate format '%s', expected 'x,y'", coord_str);
        return -1;
    }

    return 0;
}

/* Parse size string "wxh" */
static int parse_size_string(const char* size_str, int* width, int* height)
{
    if (!size_str || !width || !height) {
        return -1;
    }

    if (sscanf(size_str, "%dx%d", width, height) != 2) {
        IMP_LOG_ERR(TAG, "Invalid size format '%s', expected 'WxH'", size_str);
        return -1;
    }

    return 0;
}

/* Parse zone configuration from JSON (web UI format) */
static int parse_zone_config(json_object* json, motion_module_config_t* config)
{
    /* Look for zones array (web UI format) */
    json_object* zones_array = NULL;
    if (json_object_object_get_ex(json, "zones", &zones_array) && json_object_is_type(zones_array, json_type_array)) {
        /* Web UI zones format */
        int array_len = json_object_array_length(zones_array);
        config->zone_count = (array_len > MOTION_MAX_ROI_COUNT) ? MOTION_MAX_ROI_COUNT : array_len;

        IMP_LOG_INFO(TAG, "Parsing %d zones from web UI format", config->zone_count);

        for (int i = 0; i < config->zone_count; i++) {
            json_object* zone_obj = json_object_array_get_idx(zones_array, i);
            if (!zone_obj) continue;

            /* Parse zone ID */
            json_object* id_obj = NULL;
            if (json_object_object_get_ex(zone_obj, "id", &id_obj)) {
                config->zones[i].id = json_object_get_int(id_obj);
            }

            /* Parse zone type */
            json_object* type_obj = NULL;
            if (json_object_object_get_ex(zone_obj, "type", &type_obj)) {
                const char* type = json_object_get_string(type_obj);
                if (type) {
                    strncpy(config->zones[i].type, type, sizeof(config->zones[i].type) - 1);
                    config->zones[i].type[sizeof(config->zones[i].type) - 1] = '\0';
                }
            }

            /* Parse zone name */
            json_object* name_obj = NULL;
            if (json_object_object_get_ex(zone_obj, "name", &name_obj)) {
                const char* name = json_object_get_string(name_obj);
                if (name) {
                    strncpy(config->zones[i].name, name, sizeof(config->zones[i].name) - 1);
                    config->zones[i].name[sizeof(config->zones[i].name) - 1] = '\0';
                }
            }

            /* Parse coordinates */
            json_object* x_obj = NULL, *y_obj = NULL, *width_obj = NULL, *height_obj = NULL;
            if (json_object_object_get_ex(zone_obj, "x", &x_obj)) {
                config->zones[i].x = json_object_get_int(x_obj);
            }
            if (json_object_object_get_ex(zone_obj, "y", &y_obj)) {
                config->zones[i].y = json_object_get_int(y_obj);
            }
            if (json_object_object_get_ex(zone_obj, "width", &width_obj)) {
                config->zones[i].width = json_object_get_int(width_obj);
            }
            if (json_object_object_get_ex(zone_obj, "height", &height_obj)) {
                config->zones[i].height = json_object_get_int(height_obj);
            }

            IMP_LOG_INFO(TAG, "Zone[%d] ID=%d '%s' type=%s: (%d,%d) %dx%d",
                         i, config->zones[i].id, config->zones[i].name, config->zones[i].type,
                         config->zones[i].x, config->zones[i].y, config->zones[i].width, config->zones[i].height);
        }

        /* Parse settings */
        json_object* settings_obj = NULL;
        if (json_object_object_get_ex(json, "settings", &settings_obj)) {
            /* Parse sensitivity from settings */
            json_object* sensitivity_obj = NULL;
            if (json_object_object_get_ex(settings_obj, "sensitivity", &sensitivity_obj)) {
                const char* sens_str = json_object_get_string(sensitivity_obj);
                if (sens_str) {
                    config->sensitivity = atoi(sens_str) / 10; /* Convert 0-100 to 0-10 scale */
                    if (config->sensitivity > 8) config->sensitivity = 8;
                    if (config->sensitivity < 0) config->sensitivity = 0;
                }
            }

            /* Parse minimum object size */
            json_object* min_obj_size_obj = NULL;
            if (json_object_object_get_ex(settings_obj, "minObjectSize", &min_obj_size_obj)) {
                const char* size_str = json_object_get_string(min_obj_size_obj);
                if (size_str) {
                    config->min_object_size = atoi(size_str);
                }
            }

            IMP_LOG_INFO(TAG, "Motion settings: sensitivity=%d, min_object_size=%d",
                         config->sensitivity, config->min_object_size);
        }
    } else {
        /* No zones defined - use full frame */
        config->zone_count = 1;
        config->zones[0].id = 1;
        strncpy(config->zones[0].type, "include", sizeof(config->zones[0].type) - 1);
        strncpy(config->zones[0].name, "full_frame", sizeof(config->zones[0].name) - 1);
        config->zones[0].x = 0;
        config->zones[0].y = 0;
        config->zones[0].width = 0;  /* Will be set to full frame */
        config->zones[0].height = 0; /* Will be set to full frame */

        IMP_LOG_INFO(TAG, "No zones defined, using full frame detection");
    }

    return 0;
}

/* Load ROI configuration from separate file */
static int load_roi_config(motion_module_config_t* config)
{
    json_object* root = NULL;
    const char* roi_paths[] = {
        "./roi.json",                    /* Same directory as binary (for testing) */
        "/etc/streamer.d/roi.json"       /* System config directory */
    };

    /* Try to load ROI config from multiple paths */
    for (int i = 0; i < sizeof(roi_paths) / sizeof(roi_paths[0]); i++) {
        FILE* file = fopen(roi_paths[i], "r");
        if (!file) {
            continue;
        }

        /* Read file content */
        fseek(file, 0, SEEK_END);
        long file_size = ftell(file);
        fseek(file, 0, SEEK_SET);

        char* buffer = malloc(file_size + 1);
        if (!buffer) {
            fclose(file);
            continue;
        }

        size_t read_size = fread(buffer, 1, file_size, file);
        buffer[read_size] = '\0';
        fclose(file);

        /* Parse JSON */
        root = json_tokener_parse(buffer);
        free(buffer);

        if (root) {
            IMP_LOG_INFO(TAG, "Loaded ROI configuration from: %s", roi_paths[i]);
            break;
        }
    }

    if (!root) {
        IMP_LOG_INFO(TAG, "No ROI configuration file found, using default full-frame detection");
        /* Set default full-frame zone */
        config->zone_count = 1;
        config->zones[0].id = 1;
        strncpy(config->zones[0].type, "include", sizeof(config->zones[0].type) - 1);
        strncpy(config->zones[0].name, "full_frame", sizeof(config->zones[0].name) - 1);
        config->zones[0].x = 0;
        config->zones[0].y = 0;
        config->zones[0].width = 0;  /* Will be set to full frame */
        config->zones[0].height = 0; /* Will be set to full frame */
        config->min_object_size = 5; /* Default 5% */
        return 0;
    }

    /* Parse zones from loaded JSON */
    int result = parse_zone_config(root, config);
    json_object_put(root);

    return result;
}

/* Module lifecycle functions */

/* Initialize motion detection module */
int motion_module_init(void* config)
{
    IMP_LOG_INFO(TAG, "Initializing motion detection module");

    if (g_motion_state.initialized) {
        IMP_LOG_WARN(TAG, "Motion module already initialized");
        return 0;
    }

    if (!config) {
        IMP_LOG_ERR(TAG, "Invalid configuration provided");
        return -1;
    }

    /* Copy configuration */
    memcpy(&g_motion_state.config, config, sizeof(motion_module_config_t));

    /* Load ROI/zone configuration from separate file */
    if (load_roi_config(&g_motion_state.config) != 0) {
        IMP_LOG_WARN(TAG, "Failed to load ROI configuration, using defaults");
    }

    /* Get frame dimensions dynamically from the selected channel */
    int actual_width, actual_height;
    if (motion_get_channel_dimensions(g_motion_state.config.monitor_stream,
                                     &actual_width, &actual_height) == 0) {
        /* Update configuration with actual dimensions */
        g_motion_state.config.frame_width = actual_width;
        g_motion_state.config.frame_height = actual_height;

        IMP_LOG_INFO(TAG, "Using channel %d dimensions: %dx%d",
                     g_motion_state.config.monitor_stream, actual_width, actual_height);

        /* Adjust zone regions to fit actual frame size */
        for (int i = 0; i < g_motion_state.config.zone_count; i++) {
            /* If zone extends beyond actual frame, adjust it */
            if (g_motion_state.config.zones[i].x + g_motion_state.config.zones[i].width > actual_width) {
                g_motion_state.config.zones[i].width = actual_width - g_motion_state.config.zones[i].x;
                IMP_LOG_WARN(TAG, "Adjusted zone[%d] '%s' width to %d to fit frame",
                            i, g_motion_state.config.zones[i].name, g_motion_state.config.zones[i].width);
            }
            if (g_motion_state.config.zones[i].y + g_motion_state.config.zones[i].height > actual_height) {
                g_motion_state.config.zones[i].height = actual_height - g_motion_state.config.zones[i].y;
                IMP_LOG_WARN(TAG, "Adjusted zone[%d] '%s' height to %d to fit frame",
                            i, g_motion_state.config.zones[i].name, g_motion_state.config.zones[i].height);
            }

            /* If zone dimensions are 0, use actual frame size */
            if (g_motion_state.config.zones[i].width == 0 && g_motion_state.config.zones[i].height == 0) {
                g_motion_state.config.zones[i].width = actual_width;
                g_motion_state.config.zones[i].height = actual_height;
                IMP_LOG_INFO(TAG, "Set zone[%d] '%s' to full frame dimensions: %dx%d",
                            i, g_motion_state.config.zones[i].name, actual_width, actual_height);
            }
        }
    } else {
        IMP_LOG_WARN(TAG, "Failed to get channel dimensions, using configured values: %dx%d",
                     g_motion_state.config.frame_width, g_motion_state.config.frame_height);
    }

    /* Initialize mutex */
    if (pthread_mutex_init(&g_motion_state.mutex, NULL) != 0) {
        IMP_LOG_ERR(TAG, "Failed to initialize mutex");
        return -1;
    }

    /* Reset state */
    g_motion_state.motion_detected = false;
    g_motion_state.motion_start_script_executed = false;
    g_motion_state.last_motion_time = 0;
    g_motion_state.motion_start_time = 0;
    g_motion_state.motion_events = 0;
    g_motion_state.frames_processed = 0;
    g_motion_state.false_positives = 0;

    g_motion_state.initialized = true;
    IMP_LOG_INFO(TAG, "Motion detection module initialized successfully");
    return 0;
}

/* Start motion detection module */
int motion_module_start(void)
{
    IMP_LOG_INFO(TAG, "Starting motion detection module");

    if (!g_motion_state.initialized) {
        IMP_LOG_ERR(TAG, "Motion module not initialized");
        return -1;
    }

    if (g_motion_state.running) {
        IMP_LOG_WARN(TAG, "Motion module already running");
        return 0;
    }

    if (!g_motion_state.config.enabled) {
        IMP_LOG_INFO(TAG, "Motion detection disabled in configuration");
        return 0;
    }

    /* Setup IVS motion detection */
    if (motion_setup_ivs() != 0) {
        IMP_LOG_ERR(TAG, "Failed to setup IVS motion detection");
        return -1;
    }

    /* Start detection thread */
    g_motion_state.thread_should_exit = false;
    if (pthread_create(&g_motion_state.detection_thread, NULL, motion_detection_thread, NULL) != 0) {
        IMP_LOG_ERR(TAG, "Failed to create motion detection thread");
        motion_cleanup_ivs();
        return -1;
    }

    g_motion_state.running = true;
    IMP_LOG_INFO(TAG, "Motion detection module started successfully");
    return 0;
}

/* Stop motion detection module */
int motion_module_stop(void)
{
    IMP_LOG_INFO(TAG, "Stopping motion detection module");

    if (!g_motion_state.running) {
        IMP_LOG_WARN(TAG, "Motion module not running");
        return 0;
    }

    /* Signal thread to exit */
    g_motion_state.thread_should_exit = true;

    /* Wait for thread to finish */
    if (pthread_join(g_motion_state.detection_thread, NULL) != 0) {
        IMP_LOG_ERR(TAG, "Failed to join motion detection thread");
        return -1;
    }

    /* Cleanup IVS */
    motion_cleanup_ivs();

    g_motion_state.running = false;
    IMP_LOG_INFO(TAG, "Motion detection module stopped successfully");
    return 0;
}

/* Cleanup motion detection module */
int motion_module_cleanup(void)
{
    IMP_LOG_INFO(TAG, "Cleaning up motion detection module");

    if (g_motion_state.running) {
        motion_module_stop();
    }

    if (!g_motion_state.initialized) {
        return 0;
    }

    /* Destroy mutex */
    pthread_mutex_destroy(&g_motion_state.mutex);

    /* Reset state */
    memset(&g_motion_state, 0, sizeof(g_motion_state));

    IMP_LOG_INFO(TAG, "Motion detection module cleaned up successfully");
    return 0;
}

/* Configuration functions */

/* Parse motion detection configuration from JSON */
int motion_module_config_parse(json_object* json, void* config)
{
    if (!json || !config) {
        return -1;
    }

    motion_module_config_t* motion_config = (motion_module_config_t*)config;

    /* Set defaults first */
    motion_set_default_config(motion_config);

    /* Parse basic configuration directly from JSON root */
    json_object* enabled_obj = NULL;
    if (json_object_object_get_ex(json, "enabled", &enabled_obj)) {
        motion_config->enabled = json_object_get_boolean(enabled_obj);
    }

    json_object* monitor_stream_obj = NULL;
    if (json_object_object_get_ex(json, "monitor_stream", &monitor_stream_obj)) {
        motion_config->monitor_stream = json_object_get_int(monitor_stream_obj);
    }

    json_object* sensitivity_obj = NULL;
    if (json_object_object_get_ex(json, "sensitivity", &sensitivity_obj)) {
        motion_config->sensitivity = json_object_get_int(sensitivity_obj);
    }

    json_object* skip_frame_count_obj = NULL;
    if (json_object_object_get_ex(json, "skip_frame_count", &skip_frame_count_obj)) {
        motion_config->skip_frame_count = json_object_get_int(skip_frame_count_obj);
    }

    json_object* ivs_polling_timeout_obj = NULL;
    if (json_object_object_get_ex(json, "ivs_polling_timeout", &ivs_polling_timeout_obj)) {
        motion_config->ivs_polling_timeout = json_object_get_int(ivs_polling_timeout_obj);
    }

    /* Parse timing parameters */
    json_object* cooldown_time_obj = NULL;
    if (json_object_object_get_ex(json, "cooldown_time", &cooldown_time_obj)) {
        motion_config->cooldown_time = json_object_get_int(cooldown_time_obj);
    }



    json_object* debounce_time_obj = NULL;
    if (json_object_object_get_ex(json, "debounce_time", &debounce_time_obj)) {
        motion_config->debounce_time = json_object_get_int(debounce_time_obj);
    }

    json_object* init_time_obj = NULL;
    if (json_object_object_get_ex(json, "init_time", &init_time_obj)) {
        motion_config->init_time = json_object_get_int(init_time_obj);
    }

    json_object* min_time_obj = NULL;
    if (json_object_object_get_ex(json, "min_time", &min_time_obj)) {
        motion_config->min_time = json_object_get_int(min_time_obj);
    }

    json_object* post_time_obj = NULL;
    if (json_object_object_get_ex(json, "post_time", &post_time_obj)) {
        motion_config->post_time = json_object_get_int(post_time_obj);
    }

    /* Frame parameters are now auto-detected from channel configuration */
    /* Skip parsing frame_width and frame_height - they will be set dynamically */

    /* Parse script path */
    json_object* script_path_obj = NULL;
    if (json_object_object_get_ex(json, "script_path", &script_path_obj)) {
        const char* script_path = json_object_get_string(script_path_obj);
        if (script_path) {
            strncpy(motion_config->script_path, script_path, sizeof(motion_config->script_path) - 1);
            motion_config->script_path[sizeof(motion_config->script_path) - 1] = '\0';
        }
    }

    /* ROI/zone configuration is loaded separately from roi.json */
    /* No need to parse zones here - they will be loaded in motion_module_init */

    return 0;
}

/* Validate motion detection configuration */
int motion_module_config_validate(void* config)
{
    if (!config) {
        return -1;
    }

    motion_module_config_t* motion_config = (motion_module_config_t*)config;

    /* Validate sensitivity range */
    if (motion_config->sensitivity < 0 || motion_config->sensitivity > 8) {
        IMP_LOG_ERR(TAG, "Invalid sensitivity %d (must be 0-8)", motion_config->sensitivity);
        return -1;
    }

    /* Frame dimensions are auto-detected, skip validation here */

    /* Validate zone count */
    if (motion_config->zone_count < 1 || motion_config->zone_count > MOTION_MAX_ROI_COUNT) {
        IMP_LOG_ERR(TAG, "Invalid zone count %d (must be 1-%d)", motion_config->zone_count, MOTION_MAX_ROI_COUNT);
        return -1;
    }

    /* Validate zone regions */
    for (int i = 0; i < motion_config->zone_count; i++) {
        /* Skip validation for zones with 0 dimensions (will be set to full frame) */
        if (motion_config->zones[i].width == 0 && motion_config->zones[i].height == 0) {
            continue;
        }

        if (motion_config->zones[i].width <= 0 || motion_config->zones[i].height <= 0) {
            IMP_LOG_ERR(TAG, "Invalid zone[%d] '%s' dimensions %dx%d",
                       i, motion_config->zones[i].name, motion_config->zones[i].width, motion_config->zones[i].height);
            return -1;
        }

        if (motion_config->zones[i].x < 0 || motion_config->zones[i].y < 0) {
            IMP_LOG_ERR(TAG, "Invalid zone[%d] '%s' position (%d,%d)",
                       i, motion_config->zones[i].name, motion_config->zones[i].x, motion_config->zones[i].y);
            return -1;
        }

        /* Validate zone type */
        if (strcmp(motion_config->zones[i].type, "include") != 0 &&
            strcmp(motion_config->zones[i].type, "exclude") != 0) {
            IMP_LOG_ERR(TAG, "Invalid zone[%d] '%s' type '%s' (must be 'include' or 'exclude')",
                       i, motion_config->zones[i].name, motion_config->zones[i].type);
            return -1;
        }
    }

    return 0;
}

/* Free motion detection configuration */
void motion_module_config_free(void* config)
{
    /* Nothing to free for this configuration */
    (void)config;
}

/* RTSP integration */
int motion_module_set_rtsp_server(struct rtsp_server* server)
{
    g_motion_state.rtsp_server = server;
    IMP_LOG_INFO(TAG, "RTSP server reference set for motion detection module");
    return 0;
}

/* API functions */

/* Check if motion is currently detected */
bool motion_module_is_motion_detected(void)
{
    pthread_mutex_lock(&g_motion_state.mutex);
    bool detected = g_motion_state.motion_detected;
    pthread_mutex_unlock(&g_motion_state.mutex);
    return detected;
}

/* Get total number of motion events */
unsigned long motion_module_get_motion_events(void)
{
    pthread_mutex_lock(&g_motion_state.mutex);
    unsigned long events = g_motion_state.motion_events;
    pthread_mutex_unlock(&g_motion_state.mutex);
    return events;
}

/* Get total number of frames processed */
unsigned long motion_module_get_frames_processed(void)
{
    pthread_mutex_lock(&g_motion_state.mutex);
    unsigned long frames = g_motion_state.frames_processed;
    pthread_mutex_unlock(&g_motion_state.mutex);
    return frames;
}

/* Module registration */
module_info_t motion_module_info = {
    .name = MOTION_MODULE_NAME,
    .version = MOTION_MODULE_VERSION,
    .description = "IVS-based motion detection with configurable ROI regions",
    .state = MODULE_STATE_UNREGISTERED,
    .module_data = &g_motion_state,

    /* Lifecycle callbacks */
    .init = motion_module_init,
    .start = motion_module_start,
    .stop = motion_module_stop,
    .cleanup = motion_module_cleanup,

    /* Configuration */
    .config_size = sizeof(motion_module_config_t),
    .config_parse = motion_module_config_parse,
    .config_validate = motion_module_config_validate,
    .config_free = motion_module_config_free,

    /* RTSP integration */
    .rtsp_setup = motion_module_set_rtsp_server,
    .rtsp_frame_callback = NULL,
    .rtsp_cleanup = NULL,

    /* Statistics - not implemented */
    .get_stats = NULL
};

/* Module registration function for manual registration */
int register_motion_module(void)
{
    return module_register(&motion_module_info);
}
