/*
 * snapshot_fallback.c - Snapshot Fallback System
 * Saves snapshots to /tmp/ when HTTP module is not available
 * Copyright (C) 2025 Paul Philippov, Thingino Project
 */

#include "snapshot_fallback.h"
#include "common.h"
#include "config.h"
#include "frame_manager.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>

#include <imp/imp_encoder.h>
#include <imp/imp_framesource.h>

#include "hal/imp.h"

#ifdef ENABLE_HTTP_MODULE
#include "modules/http/http_module.h"
#endif

/* External global config */
extern streamer_config_t* g_config;

#define TAG "SNAPSHOT_FALLBACK"
#define DEFAULT_OUTPUT_DIR "/tmp"
#define DEFAULT_UPDATE_INTERVAL_MS 5000  /* 5 seconds */
#define MAX_CHANNELS 4

/* Snapshot fallback state */
static struct {
    bool initialized;
    bool running;
    snapshot_fallback_config_t config;
    pthread_t worker_thread;
    pthread_mutex_t mutex;
    uint64_t last_capture_time[MAX_CHANNELS];
    int consumer_id;
} g_fallback_state = {0};

/* Check if HTTP module is available */
bool snapshot_fallback_is_http_available(void)
{
#ifdef ENABLE_HTTP_MODULE
    /* Check if HTTP module is enabled in configuration */
    struct streamer_config* config = get_global_config();
    if (config && config->http.enabled) {
        return true;
    }
#endif
    return false;
}

/* Set default configuration */
void snapshot_fallback_set_default_config(snapshot_fallback_config_t* config)
{
    if (!config) return;

    memset(config, 0, sizeof(snapshot_fallback_config_t));
    config->enabled = true;
    strcpy(config->output_dir, DEFAULT_OUTPUT_DIR);
    config->update_interval_ms = DEFAULT_UPDATE_INTERVAL_MS;
    config->overwrite_existing = true;
    config->max_file_age_seconds = 3600; /* 1 hour */
}

/* Capture snapshot from channel using IMP encoder */
static int capture_channel_snapshot(int channel, const char* output_path)
{
    /* This is similar to the HTTP module's capture_snapshot function
     * but saves directly to file instead of returning data */

    IMP_LOG_DBG(TAG, "Capturing snapshot for channel %d to %s", channel, output_path);

    /* Validate channel */
    if (channel < 0 || channel >= FS_CHN_NUM) {
        IMP_LOG_ERR(TAG, "Invalid channel %d for snapshot", channel);
        return -1;
    }

    /* Check if channel is enabled by checking if it exists in streams array */
    if (!g_config || !g_config->streams) {
        IMP_LOG_ERR(TAG, "No configuration available for channel validation");
        return -1;
    }

    /* Channel corresponds to stream index in the array */
    if (channel >= g_config->stream_count) {
        IMP_LOG_WARN(TAG, "Channel %d is beyond configured stream count (%d)", channel, g_config->stream_count);
        return -1;
    }

    if (!g_config->streams[channel].enabled) {
        IMP_LOG_WARN(TAG, "Channel %d is disabled, skipping snapshot", channel);
        return -1;
    }

    int ret;
    IMPEncoderStream stream;
    int jpeg_channel = FS_CHN_NUM + channel;

    /* Start receiving pictures for JPEG snapshot */
    ret = IMP_Encoder_StartRecvPic(jpeg_channel);
    if (ret < 0) {
        IMP_LOG_ERR(TAG, "IMP_Encoder_StartRecvPic(%d) failed for channel %d", jpeg_channel, channel);
        return -1;
    }

    /* Poll for JPEG stream with timeout */
    ret = IMP_Encoder_PollingStream(jpeg_channel, 1000); /* 1 second timeout */
    if (ret < 0) {
        IMP_LOG_WARN(TAG, "Polling JPEG stream timeout for channel %d", channel);
        IMP_Encoder_StopRecvPic(jpeg_channel);
        return -1;
    }

#if defined(PLATFORM_T31)
    /* T31 path via HAL: get stream and copy packs handling wrap-around internally */
    hal_stream_t hs = {0};
    ret = hal_stream_get(jpeg_channel, &hs, 1);
    if (ret < 0) {
        IMP_LOG_ERR(TAG, "HAL: Failed to get JPEG stream from channel %d: %d", channel, ret);
        IMP_Encoder_StopRecvPic(jpeg_channel);
        return -1;
    }

    int packCount = hal_stream_pack_count(&hs);
    if (packCount <= 0) {
        IMP_LOG_WARN(TAG, "HAL: No JPEG data available for channel %d", channel);
        hal_stream_release(jpeg_channel, &hs);
        IMP_Encoder_StopRecvPic(jpeg_channel);
        return -1;
    }

    /* Write JPEG data to file */
    FILE* fp = fopen(output_path, "wb");
    if (!fp) {
        IMP_LOG_ERR(TAG, "Failed to open output file: %s", output_path);
        hal_stream_release(jpeg_channel, &hs);
        return -1;
    }

    /* Determine maximum pack size to allocate a reusable buffer */
    uint32_t max_len = 0;
    for (int i = 0; i < packCount; i++) {
        uint32_t l = hal_stream_pack_length(&hs, i);
        if (l > max_len) max_len = l;
    }
    uint8_t *buf = max_len ? (uint8_t*)malloc(max_len) : NULL;
    if (max_len && !buf) {
        fclose(fp);
        hal_stream_release(jpeg_channel, &hs);
        IMP_Encoder_StopRecvPic(jpeg_channel);
        return -1;
    }

    size_t total_written = 0;
    for (int i = 0; i < packCount; i++) {
        uint32_t l = hal_stream_pack_length(&hs, i);
        if (l == 0) continue;
        if (buf) {
            uint32_t copied = hal_stream_copy_pack(&hs, i, buf);
            size_t w = fwrite(buf, 1, copied, fp);
            total_written += w;
            if (w != copied) {
                IMP_LOG_WARN(TAG, "Partial write: %zu/%u bytes", w, copied);
            }
        }
    }
    if (buf) free(buf);

    fclose(fp);
    hal_stream_release(jpeg_channel, &hs);

    /* Stop receiving pictures */
    IMP_Encoder_StopRecvPic(jpeg_channel);

    if (total_written > 0) {
        IMP_LOG_INFO(TAG, "Snapshot saved (HAL): %s (%zu bytes)", output_path, total_written);
        return 0;
    } else {
        IMP_LOG_ERR(TAG, "No data written to snapshot file (HAL): %s", output_path);
        unlink(output_path);
        return -1;
    }
#else
    /* Get JPEG stream from encoder */
    ret = IMP_Encoder_GetStream(jpeg_channel, &stream, 1);
    if (ret < 0) {
        IMP_LOG_ERR(TAG, "Failed to get JPEG stream from channel %d: %d", channel, ret);
        IMP_Encoder_StopRecvPic(jpeg_channel);
        return -1;
    }

    if (stream.packCount <= 0) {
        IMP_LOG_WARN(TAG, "No JPEG data available for channel %d", channel);
        IMP_Encoder_ReleaseStream(jpeg_channel, &stream);
        IMP_Encoder_StopRecvPic(jpeg_channel);
        return -1;
    }

    /* Write JPEG data to file */
    FILE* fp = fopen(output_path, "wb");
    if (!fp) {
        IMP_LOG_ERR(TAG, "Failed to open output file: %s", output_path);
        IMP_Encoder_ReleaseStream(jpeg_channel, &stream);
        return -1;
    }

    size_t total_written = 0;
    for (int i = 0; i < stream.packCount; i++) {
        IMPEncoderPack* pack = &stream.pack[i];
        if (pack->length > 0) {
#if defined(PLATFORM_T23) || defined(PLATFORM_T20)
            /* T23/T20: pack contains direct virtual address, no wrap-around handling */
            size_t written = fwrite((void*)pack->virAddr, 1, pack->length, fp);
            total_written += written;
            if (written != pack->length) {
                IMP_LOG_WARN(TAG, "Partial write: %zu/%u bytes", written, pack->length);
            }
#else
            uint32_t remSize = stream.streamSize - pack->offset;
            if (remSize < pack->length) {
                /* Handle wrap-around */
                size_t written1 = fwrite((void*)(stream.virAddr + pack->offset), 1, remSize, fp);
                size_t written2 = fwrite((void*)stream.virAddr, 1, pack->length - remSize, fp);
                total_written += written1 + written2;
                if (written1 + written2 != pack->length) {
                    IMP_LOG_WARN(TAG, "Partial write: %zu/%u bytes", written1 + written2, pack->length);
                }
            } else {
                /* Normal copy */
                size_t written = fwrite((void*)(stream.virAddr + pack->offset), 1, pack->length, fp);
                total_written += written;
                if (written != pack->length) {
                    IMP_LOG_WARN(TAG, "Partial write: %zu/%u bytes", written, pack->length);
                }
            }
#endif
        }
    }

    fclose(fp);
    IMP_Encoder_ReleaseStream(jpeg_channel, &stream);

    /* Stop receiving pictures */
    IMP_Encoder_StopRecvPic(jpeg_channel);

    if (total_written > 0) {
        IMP_LOG_INFO(TAG, "Snapshot saved: %s (%zu bytes)", output_path, total_written);
        return 0;
    } else {
        IMP_LOG_ERR(TAG, "No data written to snapshot file: %s", output_path);
        unlink(output_path); /* Remove empty file */
        return -1;
    }
#endif
}

/* Worker thread for periodic snapshot capture */
static void* snapshot_worker_thread(void* arg)
{
    IMP_LOG_INFO(TAG, "Snapshot fallback worker thread started");

    while (g_fallback_state.running) {
        uint64_t now = get_monotonic_time_us();

        if (!g_config || !g_config->streams) {
            usleep(1000000); /* 1 second */
            continue;
        }

        /* Check each enabled channel */
        int max_channels = (g_config->stream_count < MAX_CHANNELS) ? g_config->stream_count : MAX_CHANNELS;
        for (int channel = 0; channel < max_channels; channel++) {
            /* Channel corresponds to stream index in the array */
            if (!g_config->streams[channel].enabled) {
                continue;
            }

            /* Check if it's time to update this channel */
            uint64_t elapsed_us = now - g_fallback_state.last_capture_time[channel];
            uint64_t interval_us = g_fallback_state.config.update_interval_ms * 1000;

            if (elapsed_us >= interval_us) {
                char filename[64];
                char full_path[320];

                snprintf(filename, sizeof(filename), "snap%d.jpg", channel);
                snprintf(full_path, sizeof(full_path), "%s/%s",
                        g_fallback_state.config.output_dir, filename);

                /* Capture snapshot */
                if (capture_channel_snapshot(channel, full_path) == 0) {
                    pthread_mutex_lock(&g_fallback_state.mutex);
                    g_fallback_state.last_capture_time[channel] = now;
                    pthread_mutex_unlock(&g_fallback_state.mutex);
                }
            }
        }

        /* Sleep for a short interval */
        usleep(500000); /* 500ms */
    }

    IMP_LOG_INFO(TAG, "Snapshot fallback worker thread stopped");
    return NULL;
}

/* Initialize snapshot fallback system */
int snapshot_fallback_init(const snapshot_fallback_config_t* config)
{
    if (g_fallback_state.initialized) {
        IMP_LOG_WARN(TAG, "Snapshot fallback already initialized");
        return 0;
    }

    if (!config) {
        IMP_LOG_ERR(TAG, "Invalid configuration");
        return -1;
    }

    /* Copy configuration */
    memcpy(&g_fallback_state.config, config, sizeof(snapshot_fallback_config_t));

    /* Initialize mutex */
    if (pthread_mutex_init(&g_fallback_state.mutex, NULL) != 0) {
        IMP_LOG_ERR(TAG, "Failed to initialize mutex");
        return -1;
    }

    /* Create output directory if it doesn't exist */
    struct stat st = {0};
    if (stat(g_fallback_state.config.output_dir, &st) == -1) {
        if (mkdir(g_fallback_state.config.output_dir, 0755) != 0) {
            IMP_LOG_ERR(TAG, "Failed to create output directory: %s",
                       g_fallback_state.config.output_dir);
            pthread_mutex_destroy(&g_fallback_state.mutex);
            return -1;
        }
    }

    /* Initialize capture times */
    memset(g_fallback_state.last_capture_time, 0, sizeof(g_fallback_state.last_capture_time));

    g_fallback_state.initialized = true;
    IMP_LOG_INFO(TAG, "Snapshot fallback initialized (dir=%s, interval=%dms)",
                g_fallback_state.config.output_dir, g_fallback_state.config.update_interval_ms);

    return 0;
}

/* Start snapshot fallback system */
int snapshot_fallback_start(void)
{
    if (!g_fallback_state.initialized) {
        IMP_LOG_ERR(TAG, "Snapshot fallback not initialized");
        return -1;
    }

    if (g_fallback_state.running) {
        IMP_LOG_WARN(TAG, "Snapshot fallback already running");
        return 0;
    }

    /* Check if we should enable fallback */
    if (!g_fallback_state.config.enabled) {
        IMP_LOG_INFO(TAG, "Snapshot fallback disabled in configuration");
        return 0;
    }

    if (snapshot_fallback_is_http_available()) {
        IMP_LOG_INFO(TAG, "HTTP module available, snapshot fallback not needed");
        return 0;
    }

    IMP_LOG_INFO(TAG, "HTTP module not available, starting snapshot fallback");

    /* Start worker thread */
    g_fallback_state.running = true;
    if (pthread_create(&g_fallback_state.worker_thread, NULL, snapshot_worker_thread, NULL) != 0) {
        IMP_LOG_ERR(TAG, "Failed to create worker thread");
        g_fallback_state.running = false;
        return -1;
    }

    IMP_LOG_INFO(TAG, "Snapshot fallback started");
    return 0;
}

/* Stop snapshot fallback system */
int snapshot_fallback_stop(void)
{
    if (!g_fallback_state.running) {
        return 0;
    }

    IMP_LOG_INFO(TAG, "Stopping snapshot fallback");

    g_fallback_state.running = false;

    /* Wait for worker thread to finish */
    if (pthread_join(g_fallback_state.worker_thread, NULL) != 0) {
        IMP_LOG_WARN(TAG, "Failed to join worker thread");
    }

    IMP_LOG_INFO(TAG, "Snapshot fallback stopped");
    return 0;
}

/* Cleanup snapshot fallback system */
int snapshot_fallback_cleanup(void)
{
    if (!g_fallback_state.initialized) {
        return 0;
    }

    /* Stop if running */
    snapshot_fallback_stop();

    /* Cleanup mutex */
    pthread_mutex_destroy(&g_fallback_state.mutex);

    g_fallback_state.initialized = false;
    IMP_LOG_INFO(TAG, "Snapshot fallback cleaned up");
    return 0;
}

/* Check if snapshot fallback is running */
bool snapshot_fallback_is_running(void)
{
    return g_fallback_state.running;
}

/* Manually trigger snapshot capture */
int snapshot_fallback_capture_now(int channel)
{
    if (!g_fallback_state.initialized) {
        return -1;
    }

    char filename[64];
    char full_path[320];

    snprintf(filename, sizeof(filename), "snap%d.jpg", channel);
    snprintf(full_path, sizeof(full_path), "%s/%s",
            g_fallback_state.config.output_dir, filename);

    return capture_channel_snapshot(channel, full_path);
}

/* Get path to latest snapshot for a channel */
int snapshot_fallback_get_latest_path(int channel, char* path_buffer)
{
    if (!path_buffer || channel < 0 || channel >= MAX_CHANNELS) {
        return -1;
    }

    snprintf(path_buffer, 320, "%s/snap%d.jpg",
            g_fallback_state.config.output_dir, channel);

    /* Check if file exists */
    struct stat st;
    if (stat(path_buffer, &st) == 0) {
        return 0;
    }

    return -1;
}

/* Cleanup old snapshot files */
int snapshot_fallback_cleanup_old_files(void)
{
    if (!g_fallback_state.initialized || g_fallback_state.config.max_file_age_seconds <= 0) {
        return 0;
    }

    DIR* dir = opendir(g_fallback_state.config.output_dir);
    if (!dir) {
        return -1;
    }

    time_t now = time(NULL);
    int cleaned_count = 0;
    struct dirent* entry;

    while ((entry = readdir(dir)) != NULL) {
        /* Skip non-snapshot files */
        if (strncmp(entry->d_name, "snap", 4) != 0 ||
            !strstr(entry->d_name, ".jpg")) {
            continue;
        }

        char full_path[320];
        snprintf(full_path, sizeof(full_path), "%s/%s",
                g_fallback_state.config.output_dir, entry->d_name);

        struct stat st;
        if (stat(full_path, &st) == 0) {
            if (now - st.st_mtime > g_fallback_state.config.max_file_age_seconds) {
                if (unlink(full_path) == 0) {
                    cleaned_count++;
                    IMP_LOG_DBG(TAG, "Cleaned up old snapshot: %s", entry->d_name);
                }
            }
        }
    }

    closedir(dir);

    if (cleaned_count > 0) {
        IMP_LOG_INFO(TAG, "Cleaned up %d old snapshot files", cleaned_count);
    }

    return cleaned_count;
}
