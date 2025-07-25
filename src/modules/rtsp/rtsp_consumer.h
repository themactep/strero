/*
 * rtsp_consumer.h - RTSP Frame Consumer for Frame Manager
 * Receives frames from frame manager and feeds them to RTSP server
 * Copyright (C) 2025 Paul Philippov, Thingino Project
 */

#ifndef __RTSP_CONSUMER_H__
#define __RTSP_CONSUMER_H__

#include <stdbool.h>

#include "rtsp_server.h"

/**
 * Initialize RTSP consumer
 * @param rtsp_server RTSP server instance to feed frames to
 * @return 0 on success, -1 on error
 */
int rtsp_consumer_init(rtsp_server_t* rtsp_server);

/**
 * Cleanup RTSP consumer
 * @return 0 on success, -1 on error
 */
int rtsp_consumer_cleanup(void);

/**
 * Enable/disable RTSP consumer
 * @param enabled True to enable, false to disable
 * @return 0 on success, -1 on error
 */
int rtsp_consumer_set_enabled(bool enabled);

#endif /* __RTSP_CONSUMER_H__ */
