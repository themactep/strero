/*
 * rtsp_consumer.c - RTSP Frame Consumer for Frame Manager
 * Receives frames from frame manager and feeds them to RTSP server
 * Copyright (C) 2025 Paul Philippov, Thingino Project
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../common.h"
#include "../../frame_manager.h"
#include "rtsp_consumer.h"
#include "rtsp_server.h"

/* Module tag */
#define TAG "RTSP_CONSUMER"

/* RTSP consumer state */
static struct {
    int consumer_id;
    rtsp_server_t* rtsp_server;
    bool initialized;
} g_rtsp_consumer = {0};

/* Frame callback for RTSP consumer */
static void rtsp_frame_callback(const frame_info_t* frame, void* user_data)
{
    if (!g_rtsp_consumer.rtsp_server || !frame) {
        return;
    }

    /* Convert frame_info_t to RTSP server format and send */
    rtsp_server_send_frame(g_rtsp_consumer.rtsp_server,
                          frame->channel,
                          frame->data,
                          frame->size,
                          &frame->timestamp);
}

int rtsp_consumer_init(rtsp_server_t* rtsp_server)
{
    if (g_rtsp_consumer.initialized) {
        IMP_LOG_WARN(TAG, "RTSP consumer already initialized");
        return 0;
    }

    if (!rtsp_server) {
        IMP_LOG_ERR(TAG, "Invalid RTSP server pointer");
        return -1;
    }

    g_rtsp_consumer.rtsp_server = rtsp_server;

    /* Register with frame manager */
    frame_consumer_t consumer = {
        .name = "RTSP",
        .callback = rtsp_frame_callback,
        .user_data = NULL,
        .channel_mask = 0x03, /* Channels 0 and 1 */
        .active = true
    };

    g_rtsp_consumer.consumer_id = frame_manager_register_consumer(&consumer);
    if (g_rtsp_consumer.consumer_id < 0) {
        IMP_LOG_ERR(TAG, "Failed to register RTSP consumer with frame manager");
        return -1;
    }

    g_rtsp_consumer.initialized = true;
    // IMP_LOG_INFO(TAG, "RTSP consumer initialized successfully (ID: %d)", g_rtsp_consumer.consumer_id);
    return 0;
}

int rtsp_consumer_cleanup(void)
{
    if (!g_rtsp_consumer.initialized) {
        return 0;
    }

    /* Unregister from frame manager */
    if (g_rtsp_consumer.consumer_id >= 0) {
        frame_manager_unregister_consumer(g_rtsp_consumer.consumer_id);
    }

    g_rtsp_consumer.initialized = false;
    // IMP_LOG_INFO(TAG, "RTSP consumer cleaned up");
    return 0;
}

int rtsp_consumer_set_enabled(bool enabled)
{
    if (!g_rtsp_consumer.initialized) {
        return -1;
    }

    return frame_manager_set_consumer_enabled(g_rtsp_consumer.consumer_id, enabled);
}
