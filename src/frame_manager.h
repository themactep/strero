/*
 * frame_manager.h - Thingino Frame Manager
 * Frame manager implementation for Thingino Streamer
 * Central frame distribution system for modular architecture
 * Copyright (C) 2025 Paul Philippov, Thingino Project
 */

#ifndef __FRAME_MANAGER_H__
#define __FRAME_MANAGER_H__

#include <stdint.h>
#include <stdbool.h>
#include <sys/time.h>

/* Maximum number of frame consumers (modules) */
#define MAX_FRAME_CONSUMERS 8

/* Frame types */
typedef enum {
    FRAME_TYPE_H264_IDR = 0,
    FRAME_TYPE_H264_P   = 1,
    FRAME_TYPE_H265_IDR = 2,
    FRAME_TYPE_H265_P   = 3,
    FRAME_TYPE_JPEG     = 4
} frame_type_t;

/* Frame data structure */
typedef struct {
    uint8_t* data;                       /* Frame data pointer */
    size_t size;                         /* Frame size in bytes */
    int channel;                         /* Channel number (0, 1, etc.) */
    frame_type_t type;                   /* Frame type (IDR, P-frame, etc.) */
    struct timeval timestamp;            /* Frame timestamp */
    uint32_t sequence;                   /* Frame sequence number */
    bool is_keyframe;                    /* True if this is a keyframe */
} frame_info_t;

/* SPS/PPS parameter structure */
typedef struct {
    uint8_t* sps_data;                   /* SPS data */
    uint32_t sps_size;                   /* SPS size in bytes */
    uint8_t* pps_data;                   /* PPS data */
    uint32_t pps_size;                   /* PPS size in bytes */
    bool available;                      /* True if both SPS and PPS are available */
} stream_params_t;

/* Frame consumer callback function */
typedef void (*frame_consumer_callback_t)(const frame_info_t* frame, void* user_data);

/* Frame consumer registration structure */
typedef struct {
    const char* name;                    /* Consumer name (for debugging) */
    frame_consumer_callback_t callback;  /* Callback function */
    void* user_data;                     /* User data passed to callback */
    int channel_mask;                    /* Bitmask of channels to receive (0x01=ch0, 0x02=ch1, 0x03=both) */
    bool active;                         /* Is this consumer active? */
} frame_consumer_t;

/* Frame manager statistics */
typedef struct {
    unsigned long total_frames;          /* Total frames processed */
    unsigned long frames_per_channel[4]; /* Frames per channel */
    unsigned long consumers_active;      /* Number of active consumers */
    unsigned long last_frame_time;       /* Last frame timestamp (us) */
    double average_fps[4];               /* Average FPS per channel */
} frame_manager_stats_t;

/**
 * Initialize the frame manager
 * @return 0 on success, -1 on error
 */
int frame_manager_init(void);

/**
 * Start the frame manager (begins frame processing)
 * @return 0 on success, -1 on error
 */
int frame_manager_start(void);

/**
 * Stop the frame manager
 * @return 0 on success, -1 on error
 */
int frame_manager_stop(void);

/**
 * Cleanup the frame manager
 * @return 0 on success, -1 on error
 */
int frame_manager_cleanup(void);

/**
 * Register a frame consumer (module)
 * @param consumer Consumer configuration
 * @return Consumer ID on success, -1 on error
 */
int frame_manager_register_consumer(const frame_consumer_t* consumer);

/**
 * Unregister a frame consumer
 * @param consumer_id Consumer ID returned by register_consumer
 * @return 0 on success, -1 on error
 */
int frame_manager_unregister_consumer(int consumer_id);

/**
 * Enable/disable a consumer
 * @param consumer_id Consumer ID
 * @param enabled True to enable, false to disable
 * @return 0 on success, -1 on error
 */
int frame_manager_set_consumer_enabled(int consumer_id, bool enabled);

/**
 * Get frame manager statistics
 * @param stats Pointer to stats structure to fill
 * @return 0 on success, -1 on error
 */
int frame_manager_get_stats(frame_manager_stats_t* stats);

/**
 * Request IDR frame for a specific channel
 * @param channel Channel number
 * @return 0 on success, -1 on error
 */
int frame_manager_request_idr(int channel);

/**
 * Check if any consumers are active for a channel
 * @param channel Channel number
 * @return Number of active consumers for this channel
 */
int frame_manager_get_consumer_count(int channel);

/**
 * Get stream parameters (SPS/PPS) for a channel
 * @param channel Channel number
 * @return Stream parameters structure (check available field)
 */
stream_params_t frame_manager_get_stream_params(int channel);

#endif /* __FRAME_MANAGER_H__ */
