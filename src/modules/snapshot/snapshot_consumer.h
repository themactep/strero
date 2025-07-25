/*
 * snapshot_consumer.h - Snapshot Frame Consumer for Frame Manager
 * Receives frames from frame manager and saves snapshots/timelapses
 * Copyright (C) 2025 Paul Philippov, Thingino Project
 */

#ifndef __SNAPSHOT_CONSUMER_H__
#define __SNAPSHOT_CONSUMER_H__

#include <stdbool.h>

/* Snapshot configuration */
typedef struct {
    const char* output_path;        /* Output directory for snapshots */
    int interval_seconds;           /* Interval between snapshots */
    int channel;                    /* Channel to capture from */
    bool timelapse_enabled;         /* Enable timelapse mode */
    int max_snapshots;              /* Maximum snapshots to keep (0 = unlimited) */
} snapshot_config_t;

/**
 * Initialize snapshot consumer
 * @param config Snapshot configuration
 * @return 0 on success, -1 on error
 */
int snapshot_consumer_init(const snapshot_config_t* config);

/**
 * Cleanup snapshot consumer
 * @return 0 on success, -1 on error
 */
int snapshot_consumer_cleanup(void);

/**
 * Enable/disable snapshot consumer
 * @param enabled True to enable, false to disable
 * @return 0 on success, -1 on error
 */
int snapshot_consumer_set_enabled(bool enabled);

/**
 * Take immediate snapshot
 * @param filename Optional filename (NULL for auto-generated)
 * @return 0 on success, -1 on error
 */
int snapshot_consumer_take_now(const char* filename);

#endif /* __SNAPSHOT_CONSUMER_H__ */
