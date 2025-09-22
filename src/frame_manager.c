/*
 * frame_manager.c - Thingino Frame Manager Implementation
 * Frame manager implementation for Thingino Streamer
 * Central frame distribution system for modular architecture
 * Copyright (C) 2025 Paul Philippov, Thingino Project
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <errno.h>

#include <imp/imp_common.h>
#include <imp/imp_encoder.h>
#include <imp/imp_framesource.h>
#include <imp/imp_system.h>

#include "frame_manager.h"
#include "common.h"
#include "config.h"
#include "modules/rtsp/rtsp_module.h"
#include "hal/imp.h"


#define TAG "FRAME_MGR"

/* Global frame manager state */
static struct {
    frame_consumer_t consumers[MAX_FRAME_CONSUMERS];
    int consumer_count;
    pthread_mutex_t consumers_mutex;

    pthread_t processing_thread;
    bool running;
    bool initialized;

    frame_manager_stats_t stats;
    pthread_mutex_t stats_mutex;

    /* Stream parameters per channel */
    stream_params_t stream_params[FS_CHN_NUM];
    pthread_mutex_t params_mutex;
} g_frame_manager = {0};

/* Forward declarations (alphabetical order) */
static frame_type_t detect_frame_type(const uint8_t* data, size_t size);
static void distribute_frame_to_consumers(const frame_info_t* frame);
static void extract_stream_params(const frame_info_t* frame);
static void* frame_processing_thread(void* arg);
static bool is_keyframe(frame_type_t type);
static int process_frame_from_encoder(int channel);

int frame_manager_cleanup(void)
{
    if (!g_frame_manager.initialized) {
        return 0;
    }

    /* Stop if running */
    frame_manager_stop();

    /* Cleanup mutexes */
    pthread_mutex_destroy(&g_frame_manager.consumers_mutex);
    pthread_mutex_destroy(&g_frame_manager.stats_mutex);

    g_frame_manager.initialized = false;
    IMP_LOG_INFO(TAG, "Frame manager cleaned up");
    return 0;
}

int frame_manager_get_consumer_count(int channel)
{
    int count = 0;
    int channel_bit = 1 << channel;

    pthread_mutex_lock(&g_frame_manager.consumers_mutex);
    for (int i = 0; i < g_frame_manager.consumer_count; i++) {
        if (g_frame_manager.consumers[i].active &&
            (g_frame_manager.consumers[i].channel_mask & channel_bit)) {
            count++;
        }
    }
    pthread_mutex_unlock(&g_frame_manager.consumers_mutex);

    return count;
}

int frame_manager_get_stats(frame_manager_stats_t* stats)
{
    if (!stats) {
        return -1;
    }

    pthread_mutex_lock(&g_frame_manager.stats_mutex);
    memcpy(stats, &g_frame_manager.stats, sizeof(frame_manager_stats_t));
    pthread_mutex_unlock(&g_frame_manager.stats_mutex);

    return 0;
}

int frame_manager_init(void)
{
    if (g_frame_manager.initialized) {
        IMP_LOG_WARN(TAG, "Frame manager already initialized");
        return 0;
    }

    /* Initialize mutexes */
    if (pthread_mutex_init(&g_frame_manager.consumers_mutex, NULL) != 0) {
        IMP_LOG_ERR(TAG, "Failed to initialize consumers mutex");
        return -1;
    }

    if (pthread_mutex_init(&g_frame_manager.stats_mutex, NULL) != 0) {
        IMP_LOG_ERR(TAG, "Failed to initialize stats mutex");
        pthread_mutex_destroy(&g_frame_manager.consumers_mutex);
        return -1;
    }

    if (pthread_mutex_init(&g_frame_manager.params_mutex, NULL) != 0) {
        IMP_LOG_ERR(TAG, "Failed to initialize params mutex");
        pthread_mutex_destroy(&g_frame_manager.consumers_mutex);
        pthread_mutex_destroy(&g_frame_manager.stats_mutex);
        return -1;
    }

    /* Initialize state - only reset if not already initialized */
    memset(&g_frame_manager.stats, 0, sizeof(g_frame_manager.stats));
    if (g_frame_manager.consumer_count == 0) {
        /* Only reset consumer_count if no consumers are registered */
        g_frame_manager.consumer_count = 0;
    }
    g_frame_manager.running = false;
    g_frame_manager.initialized = true;

    IMP_LOG_INFO(TAG, "Frame manager initialized successfully");
    return 0;
}

int frame_manager_start(void)
{
    if (!g_frame_manager.initialized) {
        IMP_LOG_ERR(TAG, "Frame manager not initialized");
        return -1;
    }

    if (g_frame_manager.running) {
        IMP_LOG_WARN(TAG, "Frame manager already running");
        return 0;
    }

    g_frame_manager.running = true;

    /* Create frame processing thread */
    if (pthread_create(&g_frame_manager.processing_thread, NULL,
                      frame_processing_thread, NULL) != 0) {
        IMP_LOG_ERR(TAG, "Failed to create frame processing thread");
        g_frame_manager.running = false;
        return -1;
    }

    IMP_LOG_INFO(TAG, "Frame manager started successfully");
    return 0;
}

int frame_manager_stop(void)
{
    if (!g_frame_manager.running) {
        return 0;
    }

    IMP_LOG_INFO(TAG, "Stopping frame manager...");
    g_frame_manager.running = false;

    /* Wait for processing thread to finish */
    if (pthread_join(g_frame_manager.processing_thread, NULL) != 0) {
        IMP_LOG_WARN(TAG, "Failed to join frame processing thread");
    }

    IMP_LOG_INFO(TAG, "Frame manager stopped");
    return 0;
}

int frame_manager_register_consumer(const frame_consumer_t* consumer)
{
    if (!consumer || !consumer->callback || !consumer->name) {
        IMP_LOG_ERR(TAG, "Invalid consumer parameters");
        return -1;
    }

    pthread_mutex_lock(&g_frame_manager.consumers_mutex);

    if (g_frame_manager.consumer_count >= MAX_FRAME_CONSUMERS) {
        IMP_LOG_ERR(TAG, "Maximum number of consumers reached");
        pthread_mutex_unlock(&g_frame_manager.consumers_mutex);
        return -1;
    }

    int consumer_id = g_frame_manager.consumer_count;
    memcpy(&g_frame_manager.consumers[consumer_id], consumer, sizeof(frame_consumer_t));
    g_frame_manager.consumers[consumer_id].active = true;
    g_frame_manager.consumer_count++;

    pthread_mutex_unlock(&g_frame_manager.consumers_mutex);

    IMP_LOG_INFO(TAG, "Registered consumer '%s' with ID %d (channel_mask=0x%02x)",
                consumer->name, consumer_id, consumer->channel_mask);
    return consumer_id;
}

int frame_manager_request_idr(int channel)
{
    /* Request IDR frame directly from IMP encoder */
    int ret = IMP_Encoder_RequestIDR(channel);
    if (ret < 0) {
        IMP_LOG_ERR(TAG, "Failed to request IDR frame for channel %d: ret=%d", channel, ret);
        return -1;
    }

    IMP_LOG_INFO(TAG, "IDR frame requested for channel %d", channel);
    return 0;
}

int frame_manager_set_consumer_enabled(int consumer_id, bool enabled)
{
    if (consumer_id < 0 || consumer_id >= g_frame_manager.consumer_count) {
        return -1;
    }

    pthread_mutex_lock(&g_frame_manager.consumers_mutex);
    g_frame_manager.consumers[consumer_id].active = enabled;
    pthread_mutex_unlock(&g_frame_manager.consumers_mutex);

    return 0;
}

int frame_manager_unregister_consumer(int consumer_id)
{
    if (consumer_id < 0 || consumer_id >= g_frame_manager.consumer_count) {
        IMP_LOG_ERR(TAG, "Invalid consumer ID: %d", consumer_id);
        return -1;
    }

    pthread_mutex_lock(&g_frame_manager.consumers_mutex);

    const char* name = g_frame_manager.consumers[consumer_id].name;
    g_frame_manager.consumers[consumer_id].active = false;

    pthread_mutex_unlock(&g_frame_manager.consumers_mutex);

    IMP_LOG_INFO(TAG, "Unregistered consumer '%s' (ID %d)", name, consumer_id);
    return 0;
}

/* Frame processing thread - the heart of the system */
static void* frame_processing_thread(void* arg)
{
    IMP_LOG_INFO(TAG, "Frame processing thread started");

    /* Frame rate monitoring */
    static unsigned long last_fps_report_time = 0;
    static int frame_count_since_last_report = 0;

    extern struct streamer_config* g_config;
    if (!g_config) {
        IMP_LOG_ERR(TAG, "Global configuration not available");
        return NULL;
    }

    /* Calculate polling timeout based on frame rate */
    uint32_t configured_fps = g_config->sensor.fps > 0 ? g_config->sensor.fps : 30;
    int polling_timeout_ms = 1000 / configured_fps; /* Frame interval in milliseconds */

    /* For low-memory devices, use more aggressive polling to compensate for lack of buffering */
    extern int is_low_memory_device(void);
    if (is_low_memory_device()) {
        polling_timeout_ms = polling_timeout_ms / 2; /* Half the timeout for more frequent polling */
        IMP_LOG_INFO(TAG, "64MB SoC: Using aggressive polling timeout: %dms (was %dms)",
                     polling_timeout_ms, 1000 / configured_fps);
    }

    /* Debug: Log consumer counts */
    for (int channel = 0; channel < 4; channel++) {
        int consumer_count = frame_manager_get_consumer_count(channel);
        if (consumer_count > 0) {
            IMP_LOG_INFO(TAG, "Channel %d has %d consumers", channel, consumer_count);
        }
    }

    while (g_frame_manager.running) {
        bool frame_processed = false;

        /* Process frames from all enabled channels */
        for (int channel = 0; channel < 4; channel++) {
            /* Check if any consumers want this channel */
            if (frame_manager_get_consumer_count(channel) > 0) {
                if (process_frame_from_encoder(channel) == 0) {
                    frame_processed = true;
                    frame_count_since_last_report++;
                }
            }
        }

        /* Report frame rate every 5 seconds */
        unsigned long current_time = get_monotonic_time_us();
        if (last_fps_report_time == 0) {
            last_fps_report_time = current_time;
        } else if (current_time - last_fps_report_time >= 5000000) { /* 5 seconds */
            double elapsed_seconds = (current_time - last_fps_report_time) / 1000000.0;
            double actual_fps = frame_count_since_last_report / elapsed_seconds;
            IMP_LOG_INFO(TAG, "Actual frame processing rate: %.2f fps (%d frames in %.2f seconds)",
                        actual_fps, frame_count_since_last_report, elapsed_seconds);
            last_fps_report_time = current_time;
            frame_count_since_last_report = 0;
        }

        /* If no frames were processed, sleep briefly to avoid busy waiting */
        if (!frame_processed) {
            usleep(polling_timeout_ms * 1000); /* Convert ms to us */
        }
    }

    IMP_LOG_INFO(TAG, "Frame processing thread stopped");
    return NULL;
}

static int process_frame_from_encoder(int channel)
{
    extern struct streamer_config* g_config;
    /* Calculate polling timeout based on frame rate */
    uint32_t configured_fps = g_config->sensor.fps > 0 ? g_config->sensor.fps : 30;
    int polling_timeout_ms = 1000 / configured_fps; /* Frame interval in milliseconds */

    /* For low-memory devices, use more aggressive polling to compensate for lack of buffering */
    extern int is_low_memory_device(void);
    if (is_low_memory_device()) {
        polling_timeout_ms = polling_timeout_ms / 2; /* Half the timeout for more frequent polling */
    }

    /* Poll via HAL */
    int ret = hal_stream_poll(channel, polling_timeout_ms);
    if (ret < 0) return -1; /* No frame available or error */

    hal_stream_t hs = {0};
    ret = hal_stream_get(channel, &hs, 1);
    if (ret < 0) return -1;

    /* Compute total size */
    uint32_t frame_size = 0;
    int packCount = hal_stream_pack_count(&hs);
    for (int i = 0; i < packCount; i++) frame_size += hal_stream_pack_length(&hs, i);

    /* Prepare buffer per-channel */
    static uint8_t *buf[FS_CHN_NUM] = {0};
    static uint32_t cap[FS_CHN_NUM] = {0};
    if (cap[channel] < frame_size) {
        uint8_t *nb = (uint8_t*)realloc(buf[channel], frame_size);
        if (!nb) { hal_stream_release(channel, &hs); return -1; }
        buf[channel] = nb; cap[channel] = frame_size;
    }

    /* Copy packs */
    uint32_t offset = 0;
    for (int i = 0; i < packCount; i++) offset += hal_stream_copy_pack(&hs, i, buf[channel] + offset);

    /* Timestamp */
    struct timeval tv = {0};
    if (packCount > 0) {
        uint64_t ts = hal_stream_pack_timestamp_us(&hs, 0);
        tv.tv_sec = ts / 1000000; tv.tv_usec = ts % 1000000;
    }

    /* Sequence counter per channel */
    static uint32_t seq[FS_CHN_NUM] = {0};

    /* Create frame info */
    frame_info_t frame = (frame_info_t){0};
    frame.data = buf[channel];
    frame.size = frame_size;
    frame.channel = channel;
    frame.timestamp = tv;
    frame.sequence = ++seq[channel];
    frame.type = detect_frame_type(frame.data, frame.size);
    frame.is_keyframe = is_keyframe(frame.type);

    /* Extract params and distribute */
    extract_stream_params(&frame);
    distribute_frame_to_consumers(&frame);

#ifdef ENABLE_METRICS
    extern void metrics_update_stream_frame(int channel, unsigned int frame_size, bool is_error);
    metrics_update_stream_frame(channel, frame.size, false);
#endif

    pthread_mutex_lock(&g_frame_manager.stats_mutex);
    g_frame_manager.stats.total_frames++;
    g_frame_manager.stats.frames_per_channel[channel]++;
    g_frame_manager.stats.last_frame_time = get_monotonic_time_us();
    pthread_mutex_unlock(&g_frame_manager.stats_mutex);

    hal_stream_release(channel, &hs);
    return 0;
}

static void distribute_frame_to_consumers(const frame_info_t* frame)
{
    int channel_bit = 1 << frame->channel;

    pthread_mutex_lock(&g_frame_manager.consumers_mutex);

    /* Debug: Log frame distribution details */
    static int debug_count = 0;
    debug_count++;
    if (debug_count <= 5 || debug_count % 1000 == 0) {
        IMP_LOG_INFO(TAG, "Distributing frame: channel=%d, channel_bit=0x%02x, consumers=%d",
                     frame->channel, channel_bit, g_frame_manager.consumer_count);
    }

    for (int i = 0; i < g_frame_manager.consumer_count; i++) {
        frame_consumer_t* consumer = &g_frame_manager.consumers[i];

        if (debug_count <= 5) {
            IMP_LOG_INFO(TAG, "Consumer %d: name='%s', active=%d, channel_mask=0x%02x, match=%d",
                         i, consumer->name, consumer->active, consumer->channel_mask,
                         (consumer->active && (consumer->channel_mask & channel_bit)) ? 1 : 0);
        }

        /* Check if consumer is active and wants this channel */
        if (consumer->active && (consumer->channel_mask & channel_bit)) {
            /* Call the consumer callback */
            consumer->callback(frame, consumer->user_data);
        }
    }

    pthread_mutex_unlock(&g_frame_manager.consumers_mutex);
}

static void extract_stream_params(const frame_info_t* frame)
{
    if (!frame || frame->channel >= FS_CHN_NUM) {
        return;
    }

    pthread_mutex_lock(&g_frame_manager.params_mutex);

    /* Skip if we already have both SPS and PPS for this channel */
    if (g_frame_manager.stream_params[frame->channel].available) {
        pthread_mutex_unlock(&g_frame_manager.params_mutex);
        return;
    }

    stream_params_t* params = &g_frame_manager.stream_params[frame->channel];

    /* Search for SPS and PPS NAL units in the frame */
    for (size_t offset = 0; offset < frame->size - 4; offset++) {
        bool found_start_code = false;
        size_t nal_start = 0;

        /* Check for 4-byte start code (00 00 00 01) */
        if (frame->data[offset] == 0x00 && frame->data[offset + 1] == 0x00 &&
            frame->data[offset + 2] == 0x00 && frame->data[offset + 3] == 0x01) {
            found_start_code = true;
            nal_start = offset + 4;
        }
        /* Check for 3-byte start code (00 00 01) */
        else if (offset < frame->size - 3 &&
                 frame->data[offset] == 0x00 && frame->data[offset + 1] == 0x00 &&
                 frame->data[offset + 2] == 0x01) {
            found_start_code = true;
            nal_start = offset + 3;
        }

        if (found_start_code && nal_start < frame->size) {
            uint8_t nal_type = frame->data[nal_start] & 0x1F;

            if (nal_type == 7 || nal_type == 8) { /* SPS or PPS */
                /* Find end of this NAL unit */
                size_t nal_end = frame->size;

                for (size_t j = nal_start + 1; j < frame->size - 3; j++) {
                    if ((frame->data[j] == 0x00 && frame->data[j + 1] == 0x00 &&
                         frame->data[j + 2] == 0x00 && frame->data[j + 3] == 0x01) ||
                        (frame->data[j] == 0x00 && frame->data[j + 1] == 0x00 &&
                         frame->data[j + 2] == 0x01)) {
                        nal_end = j;
                        break;
                    }
                }

                uint32_t nal_size = nal_end - nal_start;

                if (nal_type == 7 && !params->sps_data) { /* SPS */
                    params->sps_data = malloc(nal_size);
                    if (params->sps_data) {
                        memcpy(params->sps_data, &frame->data[nal_start], nal_size);
                        params->sps_size = nal_size;
                        IMP_LOG_INFO(TAG, "*** FOUND SPS for channel %d! Size: %u bytes ***", frame->channel, nal_size);
                    }
                } else if (nal_type == 8 && !params->pps_data) { /* PPS */
                    params->pps_data = malloc(nal_size);
                    if (params->pps_data) {
                        memcpy(params->pps_data, &frame->data[nal_start], nal_size);
                        params->pps_size = nal_size;
                        IMP_LOG_INFO(TAG, "*** FOUND PPS for channel %d! Size: %u bytes ***", frame->channel, nal_size);
                    }
                }

                /* Check if we now have both parameters */
                if (params->sps_data && params->pps_data) {
                    params->available = true;
                    IMP_LOG_INFO(TAG, "*** STREAM PARAMETERS COMPLETE for channel %d! SPS=%u bytes, PPS=%u bytes ***",
                                frame->channel, params->sps_size, params->pps_size);
                }

                /* Skip to end of this NAL unit */
                offset = nal_end - 1;
            }
        }
    }

    pthread_mutex_unlock(&g_frame_manager.params_mutex);
}

stream_params_t frame_manager_get_stream_params(int channel)
{
    stream_params_t empty_params = {0};

    if (channel < 0 || channel >= FS_CHN_NUM) {
        return empty_params;
    }

    pthread_mutex_lock(&g_frame_manager.params_mutex);
    stream_params_t params = g_frame_manager.stream_params[channel];
    pthread_mutex_unlock(&g_frame_manager.params_mutex);

    return params;
}

static frame_type_t detect_frame_type(const uint8_t* data, size_t size)
{
    /* Simple frame type detection based on NAL unit type */
    if (size < 5) return FRAME_TYPE_H264_P;

    /* Look for start code and NAL unit type */
    for (size_t i = 0; i < size - 4; i++) {
        if (data[i] == 0x00 && data[i+1] == 0x00 && data[i+2] == 0x00 && data[i+3] == 0x01) {
            uint8_t nal_type = data[i+4] & 0x1F;
            if (nal_type == 5) return FRAME_TYPE_H264_IDR; /* H.264 IDR */
            if (nal_type == 1) return FRAME_TYPE_H264_P;   /* H.264 P-frame */
            /* Add H.265 detection if needed */
        }
    }

    return FRAME_TYPE_H264_P; /* Default */
}

static bool is_keyframe(frame_type_t type)
{
    return (type == FRAME_TYPE_H264_IDR || type == FRAME_TYPE_H265_IDR);
}
