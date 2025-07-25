/*
 * rtmp_consumer.c - RTMP Frame Consumer for Frame Manager
 * Receives frames from frame manager and feeds them to RTMP client
 * Copyright (C) 2025 Paul Philippov, Thingino Project
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../common.h"
#include "../../frame_manager.h"
#include "rtmp_client.h"
#include "rtmp_consumer.h"

/* Module tag */
#define TAG "RTMP_CONSUMER"

/* RTMP consumer state */
static struct {
    int consumer_id;
    rtmp_client_t* rtmp_client;
    bool initialized;
    bool enabled;
} g_rtmp_consumer = {0};

/* Frame callback for RTMP consumer */
static void rtmp_frame_callback(const frame_info_t* frame, void* user_data)
{
    if (!g_rtmp_consumer.rtmp_client || !frame || !g_rtmp_consumer.enabled) {
        return;
    }

    /* Check if we have any active RTMP connections before processing */
    extern int rtmp_client_get_active_connection_count(void);
    int active_connections = rtmp_client_get_active_connection_count();
    if (active_connections == 0) {
        /* No active connections, skip frame processing */
        return;
    }

    /* Debug logging for first few frames and keyframes */
    static int frame_count = 0;
    frame_count++;
    if (frame_count <= 5 || frame->is_keyframe) {
        IMP_LOG_INFO(TAG, "RTMP consumer received frame %d: channel=%d, size=%u, type=%d, is_keyframe=%d, active_connections=%d",
                    frame_count, frame->channel, frame->size, frame->type, frame->is_keyframe, active_connections);

        /* Analyze frame content for keyframes */
        if (frame->is_keyframe && frame->size >= 10) {
            bool has_sps = false, has_pps = false, has_idr = false;
            for (size_t i = 0; i < frame->size - 4; i++) {
                if (frame->data[i] == 0x00 && frame->data[i+1] == 0x00 &&
                    frame->data[i+2] == 0x00 && frame->data[i+3] == 0x01) {
                    uint8_t nal_type = frame->data[i+4] & 0x1F;
                    if (nal_type == 7) has_sps = true;
                    else if (nal_type == 8) has_pps = true;
                    else if (nal_type == 5) has_idr = true;
                }
            }
            IMP_LOG_INFO(TAG, "Keyframe analysis: SPS=%d, PPS=%d, IDR=%d", has_sps, has_pps, has_idr);
        }
    }

    /* Convert frame_info_t to RTMP client format and send */
    int result = rtmp_client_send_frame(g_rtmp_consumer.rtmp_client,
                                       frame->channel,
                                       frame->data,
                                       frame->size,
                                       &frame->timestamp);

    /* Debug result for first few frames */
    if (frame_count <= 5) {
        IMP_LOG_INFO(TAG, "RTMP consumer frame %d send result: %d", frame_count, result);
    }
}

int rtmp_consumer_init(rtmp_client_t* rtmp_client)
{
    if (g_rtmp_consumer.initialized) {
        IMP_LOG_WARN(TAG, "RTMP consumer already initialized");
        return 0;
    }

    if (!rtmp_client) {
        IMP_LOG_ERR(TAG, "Invalid RTMP client pointer");
        return -1;
    }

    g_rtmp_consumer.rtmp_client = rtmp_client;
    g_rtmp_consumer.enabled = true;

    /* Get channel mask from RTMP client config */
    int channel_mask = 1 << rtmp_client->config.video.channel; /* Use configured channel */

    /* Register with frame manager */
    frame_consumer_t consumer = {
        .name = "RTMP_CLIENT",
        .callback = rtmp_frame_callback,
        .user_data = NULL,
        .channel_mask = channel_mask,
        .active = true
    };

    g_rtmp_consumer.consumer_id = frame_manager_register_consumer(&consumer);
    if (g_rtmp_consumer.consumer_id < 0) {
        IMP_LOG_ERR(TAG, "Failed to register RTMP consumer with frame manager");
        return -1;
    }

    /* Request IDR frame to ensure we get SPS/PPS for RTMP streaming */
    extern int IMP_Encoder_RequestIDR(int encChn);
    int ret = IMP_Encoder_RequestIDR(rtmp_client->config.video.channel);
    if (ret < 0) {
        IMP_LOG_WARN(TAG, "Failed to request IDR frame for RTMP streaming on channel %d: %d",
                    rtmp_client->config.video.channel, ret);
    } else {
        IMP_LOG_INFO(TAG, "IDR frame requested for RTMP streaming on channel %d",
                    rtmp_client->config.video.channel);
    }

    g_rtmp_consumer.initialized = true;
    IMP_LOG_INFO(TAG, "RTMP consumer registered with frame manager (ID: %d, channel_mask: 0x%02x, channel: %d)",
                g_rtmp_consumer.consumer_id, channel_mask, rtmp_client->config.video.channel);
    return 0;
}

int rtmp_consumer_cleanup(void)
{
    if (!g_rtmp_consumer.initialized) {
        return 0;
    }

    /* Unregister from frame manager */
    if (g_rtmp_consumer.consumer_id >= 0) {
        frame_manager_unregister_consumer(g_rtmp_consumer.consumer_id);
    }

    memset(&g_rtmp_consumer, 0, sizeof(g_rtmp_consumer));
    IMP_LOG_INFO(TAG, "RTMP consumer cleaned up");
    return 0;
}

int rtmp_consumer_set_enabled(bool enabled)
{
    if (!g_rtmp_consumer.initialized) {
        IMP_LOG_ERR(TAG, "RTMP consumer not initialized");
        return -1;
    }

    g_rtmp_consumer.enabled = enabled;
    IMP_LOG_INFO(TAG, "RTMP consumer %s", enabled ? "enabled" : "disabled");
    return 0;
}
