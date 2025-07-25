/*
 * snapshot_fallback.h - Snapshot Fallback System
 * Saves snapshots to /tmp/ when HTTP module is not available
 * Copyright (C) 2025 Paul Philippov, Thingino Project
 */

#ifndef __SNAPSHOT_FALLBACK_H__
#define __SNAPSHOT_FALLBACK_H__

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/* Snapshot fallback configuration */
typedef struct {
    bool enabled;                   /* Enable fallback snapshot system */
    char output_dir[256];           /* Output directory (default: /tmp) */
    int update_interval_ms;         /* Update interval in milliseconds */
    bool overwrite_existing;        /* Overwrite existing files */
    int max_file_age_seconds;       /* Maximum file age before cleanup (0 = no cleanup) */
} snapshot_fallback_config_t;

/* Snapshot file information */
typedef struct {
    char filename[64];              /* Filename (e.g., "snap0.jpg") */
    char full_path[320];            /* Full path to file */
    uint64_t timestamp_us;          /* Timestamp when created */
    size_t file_size;               /* File size in bytes */
    int channel;                    /* Channel number */
} snapshot_file_info_t;

/**
 * Initialize snapshot fallback system
 * @param config Fallback configuration
 * @return 0 on success, -1 on error
 */
int snapshot_fallback_init(const snapshot_fallback_config_t* config);

/**
 * Start snapshot fallback system
 * @return 0 on success, -1 on error
 */
int snapshot_fallback_start(void);

/**
 * Stop snapshot fallback system
 * @return 0 on success, -1 on error
 */
int snapshot_fallback_stop(void);

/**
 * Cleanup snapshot fallback system
 * @return 0 on success, -1 on error
 */
int snapshot_fallback_cleanup(void);

/**
 * Check if snapshot fallback is enabled and running
 * @return true if running, false otherwise
 */
bool snapshot_fallback_is_running(void);

/**
 * Manually trigger snapshot capture for a channel
 * @param channel Channel number (0-3)
 * @return 0 on success, -1 on error
 */
int snapshot_fallback_capture_now(int channel);

/**
 * Get information about available snapshot files
 * @param files Array to store file information
 * @param max_files Maximum number of files to return
 * @return Number of files found, -1 on error
 */
int snapshot_fallback_get_file_list(snapshot_file_info_t* files, int max_files);

/**
 * Get path to latest snapshot for a channel
 * @param channel Channel number
 * @param path_buffer Buffer to store path (min 320 bytes)
 * @return 0 on success, -1 on error
 */
int snapshot_fallback_get_latest_path(int channel, char* path_buffer);

/**
 * Set default configuration
 * @param config Configuration structure to fill with defaults
 */
void snapshot_fallback_set_default_config(snapshot_fallback_config_t* config);

/**
 * Check if HTTP module is available
 * @return true if HTTP module is compiled and enabled, false otherwise
 */
bool snapshot_fallback_is_http_available(void);

/**
 * Cleanup old snapshot files based on age
 * @return Number of files cleaned up, -1 on error
 */
int snapshot_fallback_cleanup_old_files(void);

#endif /* __SNAPSHOT_FALLBACK_H__ */
