/*
 * rtmp_consumer.h - RTMP Frame Consumer for Frame Manager
 * Receives frames from frame manager and feeds them to RTMP client
 * Copyright (C) 2025 Paul Philippov, Thingino Project
 */

#ifndef __RTMP_CONSUMER_H__
#define __RTMP_CONSUMER_H__

#include <stdbool.h>

#include "rtmp_client.h"

/**
 * Initialize RTMP consumer
 * @param rtmp_client RTMP client instance to feed frames to
 * @return 0 on success, -1 on error
 */
int rtmp_consumer_init(rtmp_client_t* rtmp_client);

/**
 * Cleanup RTMP consumer
 * @return 0 on success, -1 on error
 */
int rtmp_consumer_cleanup(void);

/**
 * Enable/disable RTMP consumer
 * @param enabled True to enable, false to disable
 * @return 0 on success, -1 on error
 */
int rtmp_consumer_set_enabled(bool enabled);

#endif /* __RTMP_CONSUMER_H__ */
