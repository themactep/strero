/*
 * photosensing_module.h - Photosensing module for modular streamer
 * Self-contained photosensing module for automatic day/night mode switching
 * Copyright (C) 2025 Paul Philippov, Thingino Project
 */

#ifndef __PHOTOSENSING_MODULE_H__
#define __PHOTOSENSING_MODULE_H__

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

#include "../../module_system.h"

#define PHOTOSENSING_MODULE_VERSION "1.0.0"
#define PHOTOSENSING_MODULE_NAME "photosensing"

/* Photosensing module configuration - self-contained */
typedef struct photosensing_module_config {
    bool enabled;                     /* Enable/disable photosensing */

    /* Night mode detection thresholds */
    float night_iso_threshold;        /* ISO threshold for night mode (default: 1900000) */
    int night_count_threshold;        /* Consecutive readings for night mode (default: 5) */

    /* Day mode detection thresholds */
    float day_iso_threshold;          /* ISO threshold for day mode (default: 479832) */
    float day_gb_gain_offset;         /* GB gain offset for day mode (default: 15) */
    float day_gb_gain_threshold;      /* GB gain threshold for day mode (default: 145) */
    float day_iso_secondary_threshold; /* Secondary ISO threshold (default: 361880) */
    int day_count_threshold;          /* Consecutive readings for day mode (default: 3) */

    /* Timing configuration */
    int polling_interval_ms;          /* Polling interval in milliseconds (default: 1000) */

    /* IR cut filter configuration */
    bool ircut_enabled;               /* Enable/disable IR cut filter control */
    int ircut_day_state;              /* IR cut state for day mode (1=enabled, 0=disabled) */
    int ircut_night_state;            /* IR cut state for night mode (1=enabled, 0=disabled) */
    int ircut_gpio1;                  /* First GPIO pin for IR cut control (default: 79) */
    int ircut_gpio2;                  /* Second GPIO pin for IR cut control (default: 80) */

    /* Gain tracking configuration */
    float gb_gain_record_init;        /* Initial GB gain record value (default: 200) */
    float gr_gain_record_init;        /* Initial GR gain record value (default: 200) */

    /* Debug configuration */
    bool debug_logging;               /* Enable detailed debug logging */
} photosensing_module_config_t;

/* Photosensing mode enumeration */
typedef enum {
    PHOTOSENSING_MODE_DAY = 0,
    PHOTOSENSING_MODE_NIGHT = 1,
    PHOTOSENSING_MODE_UNKNOWN = -1
} photosensing_mode_t;

/* Photosensing statistics */
typedef struct {
    unsigned long mode_switches;      /* Total number of mode switches */
    unsigned long day_to_night_switches; /* Day to night switches */
    unsigned long night_to_day_switches; /* Night to day switches */
    photosensing_mode_t current_mode; /* Current mode */
    float last_iso_value;             /* Last ISO value read */
    float last_gb_gain;               /* Last GB gain value */
    float last_gr_gain;               /* Last GR gain value */
    int day_count;                    /* Current day detection count */
    int night_count;                  /* Current night detection count */
    unsigned long uptime_seconds;     /* Module uptime in seconds */
} photosensing_stats_t;

/* Photosensing module internal state */
typedef struct {
    /* Configuration */
    photosensing_module_config_t config;
    bool initialized;
    bool running;

    /* Threading */
    pthread_t control_thread;
    pthread_mutex_t mutex;
    volatile bool thread_should_exit;

    /* Current state */
    photosensing_mode_t current_mode;
    bool ircut_status;
    int day_count;
    int night_count;
    float gb_gain_record;
    float gr_gain_record;

    /* Statistics */
    photosensing_stats_t stats;
    time_t start_time;
} photosensing_module_state_t;

/* Photosensing module functions - these will be called by the module system */

/**
 * Initialize photosensing module
 * @param config Photosensing module configuration
 * @return 0 on success, -1 on error
 */
int photosensing_module_init(void* config);

/**
 * Start photosensing module
 * @return 0 on success, -1 on error
 */
int photosensing_module_start(void);

/**
 * Stop photosensing module
 * @return 0 on success, -1 on error
 */
int photosensing_module_stop(void);

/**
 * Cleanup photosensing module
 * @return 0 on success, -1 on error
 */
int photosensing_module_cleanup(void);

/**
 * Parse photosensing configuration from JSON
 * @param json JSON configuration object
 * @param config Photosensing configuration structure to fill
 * @return 0 on success, -1 on error
 */
int photosensing_module_config_parse(json_object* json, void* config);

/**
 * Validate photosensing configuration
 * @param config Photosensing configuration to validate
 * @return 0 if valid, -1 if invalid
 */
int photosensing_module_config_validate(void* config);

/**
 * Free photosensing configuration resources
 * @param config Photosensing configuration to free
 */
void photosensing_module_config_free(void* config);

/**
 * Get photosensing module statistics
 * @param stats_buffer Buffer to fill with statistics
 * @param buffer_size Size of statistics buffer
 * @return 0 on success, -1 on error
 */
int photosensing_module_get_stats(void* stats_buffer, size_t buffer_size);

/* Utility functions */

/**
 * Set IR cut filter state
 * @param enable 1 to enable IR cut, 0 to disable
 * @return 0 on success, -1 on error
 */
int photosensing_set_ircut(int enable);

/**
 * Get current photosensing mode as string
 * @param mode Photosensing mode enum
 * @return Mode string ("day", "night", "unknown")
 */
const char* photosensing_mode_to_string(photosensing_mode_t mode);

#endif /* __PHOTOSENSING_MODULE_H__ */
