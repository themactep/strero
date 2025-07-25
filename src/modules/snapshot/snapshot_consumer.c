/*
 * snapshot_consumer.c - Snapshot Frame Consumer for Frame Manager
 * Receives frames from frame manager and saves snapshots/timelapses
 * Copyright (C) 2025 Paul Philippov, Thingino Project
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sys/stat.h>

#include "../../frame_manager.h"
#include "snapshot_consumer.h"

/* Snapshot consumer state */
static struct {
    int consumer_id;
    snapshot_config_t config;
    time_t last_snapshot_time;
    bool initialized;
    bool take_next_keyframe;
} g_snapshot_consumer = {0};

/* Frame callback for snapshot consumer */
static void snapshot_frame_callback(const frame_info_t* frame, void* user_data)
{
    if (!frame || frame->channel != g_snapshot_consumer.config.channel) {
        return;
    }

    time_t now = time(NULL);
    bool should_take_snapshot = false;

    /* Check if we should take a snapshot */
    if (g_snapshot_consumer.take_next_keyframe && frame->is_keyframe) {
        should_take_snapshot = true;
        g_snapshot_consumer.take_next_keyframe = false;
    } else if (g_snapshot_consumer.config.timelapse_enabled) {
        /* Timelapse mode - take snapshot at intervals */
        if (now - g_snapshot_consumer.last_snapshot_time >= g_snapshot_consumer.config.interval_seconds) {
            if (frame->is_keyframe) { /* Only take keyframes for better quality */
                should_take_snapshot = true;
            }
        }
    }

    if (should_take_snapshot) {
        /* Generate filename */
        char filename[256];
        struct tm* tm_info = localtime(&now);
        snprintf(filename, sizeof(filename), "%s/snapshot_%04d%02d%02d_%02d%02d%02d.jpg",
                g_snapshot_consumer.config.output_path,
                tm_info->tm_year + 1900, tm_info->tm_mon + 1, tm_info->tm_mday,
                tm_info->tm_hour, tm_info->tm_min, tm_info->tm_sec);

        /* Save frame to file (simplified - would need JPEG encoding) */
        FILE* fp = fopen(filename, "wb");
        if (fp) {
            fwrite(frame->data, 1, frame->size, fp);
            fclose(fp);
            g_snapshot_consumer.last_snapshot_time = now;
            LOG_INFO("Snapshot saved: %s (%zu bytes)", filename, frame->size);
        } else {
            LOG_ERROR("Failed to save snapshot: %s", filename);
        }
    }
}

int snapshot_consumer_init(const snapshot_config_t* config)
{
    if (g_snapshot_consumer.initialized) {
        LOG_WARN("Snapshot consumer already initialized");
        return 0;
    }

    if (!config || !config->output_path) {
        LOG_ERROR("Invalid snapshot configuration");
        return -1;
    }

    /* Create output directory if it doesn't exist */
    struct stat st = {0};
    if (stat(config->output_path, &st) == -1) {
        if (mkdir(config->output_path, 0755) != 0) {
            LOG_ERROR("Failed to create snapshot directory: %s", config->output_path);
            return -1;
        }
    }

    /* Copy configuration */
    memcpy(&g_snapshot_consumer.config, config, sizeof(snapshot_config_t));
    g_snapshot_consumer.last_snapshot_time = 0;
    g_snapshot_consumer.take_next_keyframe = false;

    /* Register with frame manager */
    frame_consumer_t consumer = {
        .name = "Snapshot",
        .callback = snapshot_frame_callback,
        .user_data = NULL,
        .channel_mask = 1 << config->channel, /* Only the specified channel */
        .active = true
    };

    g_snapshot_consumer.consumer_id = frame_manager_register_consumer(&consumer);
    if (g_snapshot_consumer.consumer_id < 0) {
        LOG_ERROR("Failed to register snapshot consumer with frame manager");
        return -1;
    }

    g_snapshot_consumer.initialized = true;
    LOG_INFO("Snapshot consumer initialized (channel=%d, interval=%ds, path=%s)",
            config->channel, config->interval_seconds, config->output_path);
    return 0;
}

int snapshot_consumer_cleanup(void)
{
    if (!g_snapshot_consumer.initialized) {
        return 0;
    }

    /* Unregister from frame manager */
    if (g_snapshot_consumer.consumer_id >= 0) {
        frame_manager_unregister_consumer(g_snapshot_consumer.consumer_id);
    }

    g_snapshot_consumer.initialized = false;
    LOG_INFO("Snapshot consumer cleaned up");
    return 0;
}

int snapshot_consumer_set_enabled(bool enabled)
{
    if (!g_snapshot_consumer.initialized) {
        return -1;
    }

    return frame_manager_set_consumer_enabled(g_snapshot_consumer.consumer_id, enabled);
}

int snapshot_consumer_take_now(const char* filename)
{
    if (!g_snapshot_consumer.initialized) {
        return -1;
    }

    /* Flag to take the next keyframe */
    g_snapshot_consumer.take_next_keyframe = true;
    LOG_INFO("Snapshot requested - will capture next keyframe");
    return 0;
}
