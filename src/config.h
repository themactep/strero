/*
 * config.h - Streamer Configuration
 * Configuration management module for Thingino Streamer
 * Copyright (C) 2025 Paul Philippov, Thingino Project
 */

#ifndef __CONFIG_H__
#define __CONFIG_H__

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include <json-c/json.h>
#include "snapshot_fallback.h"

/* Add necessary includes for network functions */
#include <ifaddrs.h>
#include <netdb.h>

/* Stream configuration */
typedef struct stream_config {
    bool enabled;                     /* Enable/disable stream */
    int width;                        /* Stream width (parsed from resolution) */
    int height;                       /* Stream height (parsed from resolution) */
    char resolution[16];              /* Resolution string "WIDTHxHEIGHT" */
    int bitrate;                      /* Bitrate in kbps */
    char format[16];                  /* "H264" or "H265" */
    char rtsp_endpoint[64];           /* RTSP endpoint name */
    char rtsp_info[128];              /* RTSP stream info */

    /* Frame source channel configuration */
    struct {
        char pixel_format[16];        /* Pixel format: "NV12", "NV21", "YUYV422", etc. */
        int frame_rate_num;           /* Frame rate numerator */
        int frame_rate_den;           /* Frame rate denominator */
        int buffer_count;             /* Number of video buffers (nrVBs) */
        char channel_type[16];        /* Channel type: "PHY", "EXT", "INJ" */

        /* Crop configuration */
        struct {
            bool enabled;             /* Enable/disable cropping */
            int top;                  /* Crop top offset */
            int left;                 /* Crop left offset */
            int width;                /* Crop width */
            int height;               /* Crop height */
        } crop;

        /* Scaler configuration */
        struct {
            bool enabled;             /* Enable/disable scaling */
            int output_width;         /* Scaled output width */
            int output_height;        /* Scaled output height */
        } scaler;

        int picture_width;            /* Final picture width */
        int picture_height;           /* Final picture height */
    } framesource;

    /* OSD configuration */
    struct {
        /* OSD configuration */
        bool enabled;                 /* Enable/disable OSD */

        /* Logo configuration */
        struct {
            bool enabled;             /* Enable/disable logo */
            char image[256];          /* Path to logo image file */
            char size[16];            /* Size as "WIDTHxHEIGHT" */
            char position[16];        /* Position as "X,Y" */
            float opacity;            /* Opacity 0.0-1.0 */
        } logo;

        /* Time configuration */
        struct {
            bool enabled;             /* Enable/disable time */
            char format[64];          /* Time format string (strftime) */
            char font[256];           /* Path to font file */
            int size;                 /* Font size in pixels */
            char color[16];           /* Color as "#RRGGBBAA" */
            char background[16];      /* Background color as "#RRGGBBAA" */
            char position[16];        /* Position as "X,Y" */
        } time;
    } osd;
} stream_config_t;

/* General configuration */
typedef struct general_config {
    char server_ip[64];               /* Server IP address */
    int http_port;                    /* HTTP port */
    char loglevel[16];                /* Logging level */
    int osd_pool_size;                /* OSD pool size (0-1024) */
    bool memory_monitoring_enabled;   /* Enable/disable memory monitoring */
    bool allocation_tracking_enabled; /* Enable/disable allocation tracking */
    bool zero_copy_enabled;           /* Enable/disable zero copy mode */
    int zero_copy_buffer_pool_size;   /* Zero copy buffer pool size */
} general_config_t;

/* JPEG configuration */
typedef struct jpeg_config {
    bool enabled;                     /* Enable/disable JPEG snapshots */
    int jpeg_channel;                 /* JPEG channel source (0 or 1) */
    int jpeg_idle_fps;                /* FPS when no requests are made */
    char jpeg_path[256];              /* File path for JPEG snapshots */
    int jpeg_quality;                 /* Quality of JPEG snapshots (1-100) */
    int jpeg_refresh;                 /* Refresh rate in milliseconds */
} jpeg_config_t;

/* Image configuration */
typedef struct image_config {
    int brightness;                   /* Image brightness (0-255) */
    int contrast;                     /* Image contrast (0-255) */
    int saturation;                   /* Image saturation (0-255) */
    int sharpness;                    /* Image sharpness (0-255) */
    int hue;                          /* Image hue (0-255) */
    bool flip_horizontal;             /* Horizontal flip (default false) */
    bool flip_vertical;               /* Vertical flip (default false) */
    char white_balance[16];           /* White balance mode (auto, manual, etc.) */
    char exposure_mode[16];           /* Exposure mode (auto, manual, etc.) */
} image_config_t;

/* WebSocket configuration */
typedef struct websocket_config {
    bool enabled;                     /* Enable/disable WebSocket server */
    int port;                         /* WebSocket server port */
    char allowed_origins[256];        /* Allowed origins for WebSocket connections */
    bool auth_required;               /* Require authentication for WebSocket connections */
} websocket_config_t;

/* System info configuration */
typedef struct sysinfo_config {
    bool enabled;                     /* Enable/disable system info API */
    int update_interval;              /* System info update interval in seconds */
    bool include_memory;              /* Include memory info in system info API */
    bool include_load;                /* Include load info in system info API */
} sysinfo_config_t;

/* Sensor configuration */
typedef struct sensor_config {
    int fps;                          /* Sensor frame rate (applies to all streams) */
} sensor_config_t;

/* ONVIF configuration */
typedef struct {
    bool enabled;                     /* Enable/disable ONVIF services */
    char device_name[64];             /* ONVIF device name */
    char device_location[64];         /* Device location description */
    char manufacturer[64];            /* Manufacturer name */
    char model[64];                   /* Device model */
    char serial_number[64];           /* Device serial number */
    char firmware_version[32];        /* Firmware version string */
    char hardware_id[32];             /* Hardware identifier */
} onvif_config_t;

/* Main configuration */
typedef struct streamer_config {
    /* Configuration state */
    bool config_loaded;               /* Configuration loaded flag */
    json_object* json_config;         /* JSON configuration object */
    char file_path[256];              /* Configuration file path */
    time_t last_modified_time;        /* For config file monitoring */

    /* Configuration sections */
    general_config_t general;         /* General configuration */
    sensor_config_t sensor;           /* Sensor configuration */
    jpeg_config_t jpeg;               /* JPEG configuration */
    image_config_t image;             /* Image configuration */
    websocket_config_t websocket;     /* WebSocket configuration */
    sysinfo_config_t sysinfo;         /* System info configuration */
    onvif_config_t onvif;             /* ONVIF configuration */
    snapshot_fallback_config_t snapshot_fallback; /* Snapshot fallback configuration */

    /* Dynamic streams array - parsed from rtsp.streams[] */
    stream_config_t* streams;         /* Dynamic streams array */
    int stream_count;                 /* Number of streams in streams array */
} streamer_config_t;

/* Global configuration instance - declared extern, defined in config.c */
extern streamer_config_t* g_config;   /* Global configuration instance */

/* Function declarations */

/**
 * Create new configuration instance
 * @return Pointer to streamer_config_t or NULL on failure
 */
streamer_config_t* config_create_new(void);

/**
 * Destroy configuration instance
 * @param config Pointer to configuration instance
 */
void config_destroy(streamer_config_t* config);

/**
 * Load configuration from file
 * @param config Pointer to configuration instance
 * @return 0 on success, negative on error
 */
int config_load(streamer_config_t* config);

/**
 * Reload configuration from file
 * @param config Pointer to configuration instance
 * @return 0 on success, negative on error
 */
int config_reload(streamer_config_t* config);

/**
 * Check if configuration file has been modified and reload if needed
 * @param config Configuration instance to check
 * @return 1 if config was reloaded, 0 if no change, -1 on error
 */
int config_check_and_reload(streamer_config_t* config);

/**
 * Start configuration file monitoring thread
 * @param config Configuration instance to monitor
 * @return 0 on success, -1 on failure
 */
int config_start_monitoring(streamer_config_t* config);

/**
 * Stop configuration file monitoring thread
 */
void config_stop_monitoring(void);

/**
 * Validate configuration values
 * @param config Pointer to configuration instance
 * @return 0 if valid, negative on error
 */
int config_validate(streamer_config_t* config);

/**
 * Get string value from configuration
 * @param config Pointer to configuration instance
 * @param path JSON path (e.g., "rtsp.username")
 * @param default_value Default value if not found
 * @return String value or default_value
 */
const char* config_get_string(streamer_config_t* config,
                              const char* path,
                              const char* default_value);

/**
 * Get integer value from configuration
 * @param config Pointer to configuration instance
 * @param path JSON path (e.g., "rtsp.port")
 * @param default_value Default value if not found
 * @return Integer value or default_value
 */
int config_get_int(streamer_config_t* config, const char* path, int default_value);

/**
 * Get boolean value from configuration
 * @param config Pointer to configuration instance
 * @param path JSON path (e.g., "rtsp.auth_required")
 * @param default_value Default value if not found
 * @return Boolean value or default_value
 */
bool config_get_bool(streamer_config_t* config, const char* path, bool default_value);

/**
 * Initialize and load complete configuration
 * Creates configuration instance, loads from file, displays config, and starts monitoring
 * @return 0 on success, negative on error
 */
int config_init_and_load(void);

#endif /* __CONFIG_H__ */
