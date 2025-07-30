/*
 * motion_module.h - Motion Detection Module Implementation
 * Copyright (C) 2025 Paul Philippov, Thingino Project
 * Modular implementation of motion detection with Ingenic IVS
 */

#ifndef __MOTION_MODULE_H__
#define __MOTION_MODULE_H__

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

#include <imp/imp_ivs.h>
#include <imp/imp_ivs_move.h>

#include "../../module_system.h"

#define MOTION_MODULE_VERSION "1.0.0"
#define MOTION_MODULE_NAME "motion"

/* Maximum number of ROI regions for motion detection */
#define MOTION_MAX_ROI_COUNT 4

/* Motion detection configuration structure */
typedef struct {
    bool enabled;                     /* Enable/disable motion detection */
    int monitor_stream;               /* Stream channel to monitor (0 or 1) */
    int sensitivity;                  /* Motion sensitivity (0-4 for normal cameras, 0-8 for panoramic) */
    int skip_frame_count;             /* Number of frames to skip between detections */
    int ivs_polling_timeout;          /* IVS polling timeout in milliseconds */

    /* Timing parameters */
    int cooldown_time;                /* Cooldown time between motion events (seconds) */
    int debounce_time;                /* Debounce time for motion detection (seconds) */
    int init_time;                    /* Initialization time before detection starts (seconds) */
    int min_time;                     /* Minimum time for motion event (seconds) */
    int post_time;                    /* Post-motion recording time (seconds) */

    /* Frame parameters - obtained dynamically from channel */
    int frame_width;                  /* Frame width (auto-detected from channel) */
    int frame_height;                 /* Frame height (auto-detected from channel) */

    /* Zone configuration (compatible with web UI) */
    int zone_count;                   /* Number of zones (1-4) */
    struct {
        int id;                       /* Zone ID */
        char type[16];                /* "include" or "exclude" */
        int x, y;                     /* Zone top-left coordinates */
        int width, height;            /* Zone dimensions */
        char name[64];                /* Zone name/identifier */
    } zones[MOTION_MAX_ROI_COUNT];

    /* Motion settings (from web UI) */
    int min_object_size;              /* Minimum object size percentage */

    /* Script execution */
    char script_path[256];            /* Path to script to execute on motion detection */
} motion_module_config_t;

/* Motion detection state */
typedef struct {
    bool initialized;
    bool running;
    motion_module_config_t config;

    /* IVS components */
    IMPIVSInterface* ivs_interface;
    int ivs_group_id;
    int ivs_channel_id;

    /* Threading */
    pthread_t detection_thread;
    bool thread_should_exit;
    pthread_mutex_t mutex;

    /* Motion state */
    bool motion_detected;
    bool motion_start_script_executed;  /* Flag to track if start script was executed */
    unsigned long last_motion_time;
    unsigned long motion_start_time;

    /* Statistics */
    unsigned long motion_events;
    unsigned long frames_processed;
    unsigned long false_positives;

    /* Frame manager integration */
    struct rtsp_server* rtsp_server;
} motion_module_state_t;

/* Module interface */
extern module_info_t motion_module_info;

/* Module lifecycle functions */
int motion_module_init(void* config);
int motion_module_start(void);
int motion_module_stop(void);
int motion_module_cleanup(void);

/* Configuration functions */
int motion_module_config_parse(json_object* json, void* config);
int motion_module_config_validate(void* config);
void motion_module_config_free(void* config);

/* RTSP integration */
int motion_module_set_rtsp_server(struct rtsp_server* server);

/* Zone information for OSD visualization */
int motion_module_get_zones(int* zone_count, void** zones_data);
int motion_module_enable_zone_visualization(bool enabled);
int motion_module_test_zone_visualization(void);

/* Module registration function */
int register_motion_module(void);

/* Motion detection API functions */
bool motion_module_is_motion_detected(void);
unsigned long motion_module_get_motion_events(void);
unsigned long motion_module_get_frames_processed(void);

/* Internal functions */
static void* motion_detection_thread(void* arg);
static int motion_setup_ivs(void);
static int motion_cleanup_ivs(void);
static int motion_process_result(IMP_IVS_MoveOutput* result);
static int motion_execute_script(const char* action);

#endif /* __MOTION_MODULE_H__ */
