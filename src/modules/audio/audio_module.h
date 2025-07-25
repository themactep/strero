/*
 * audio_module.h - Audio module for modular streamer
 * Self-contained audio module with config, capture, encoding and RTSP integration
 *
 * Copyright (C) 2025 Paul Philippov, Thingino Project
 */

#ifndef __AUDIO_MODULE_H__
#define __AUDIO_MODULE_H__

#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>

#include "../../module_system.h"

#define AUDIO_MODULE_VERSION "1.0.0"
#define AUDIO_MODULE_NAME "audio"

/* Audio module configuration - self-contained */
typedef struct audio_module_config {
    bool input_enabled;                /* Enable/disable audio input */
    bool output_enabled;               /* Enable/disable audio output */
    int input_sample_rate;             /* Input sampling rate in Hz */
    int output_sample_rate;            /* Output sampling rate in Hz */
    int input_bitrate;                 /* Input bitrate in kbps */
    int input_vol;                     /* Input volume (0-100) */
    int input_gain;                    /* Input gain */
    char input_format[16];             /* Input format (OPUS, AAC, PCM, G711A, G711U, G726) */
    bool input_agc_enabled;            /* Automatic gain control */
    int input_agc_target_level_dbfs;   /* AGC target level */
    int input_agc_compression_gain_db; /* AGC compression gain */
    int input_alc_gain;                /* Automatic level control gain */
    bool input_high_pass_filter;       /* High pass filter */
    int input_noise_suppression;       /* Noise suppression level */
    bool force_stereo;                 /* Force stereo output */
} audio_module_config_t;

/* Audio constants */
#define AUDIO_DEVICE_ID 0
#define AUDIO_CHANNEL_ID 0
#define AUDIO_ENCODER_CHANNEL_ID 0
#define AUDIO_FRAME_SIZE 320           /* 20ms at 16kHz */
#define AUDIO_SAMPLE_RATE_8K 8000
#define AUDIO_SAMPLE_RATE_16K 16000
#define AUDIO_SAMPLE_RATE_48K 48000

/* Audio codec types */
typedef enum {
    AUDIO_CODEC_G711A = 0,
    AUDIO_CODEC_G711U,
    AUDIO_CODEC_AAC,
    AUDIO_CODEC_OPUS,
    AUDIO_CODEC_PCM
} audio_codec_t;

/* Audio frame structure */
typedef struct {
    uint8_t* data;
    uint32_t size;
    unsigned long timestamp;
    uint32_t sequence;
    audio_codec_t codec;
} audio_frame_t;

/* Audio callback function type */
typedef void (*audio_frame_callback_t)(const audio_frame_t* frame, void* user_data);

/* Audio module internal state */
typedef struct {
    /* Configuration */
    audio_module_config_t config;
    bool initialized;
    bool running;

    /* IMP Audio handles */
    int ai_device_id;
    int ai_channel_id;
    int aenc_channel_id;

    /* Threading */
    pthread_t capture_thread;
    pthread_mutex_t mutex;

    /* Callback */
    audio_frame_callback_t frame_callback;
    void* user_data;

    /* Statistics */
    unsigned long frames_captured;
    unsigned long frames_encoded;
    unsigned long bytes_encoded;

    /* Audio properties */
    audio_codec_t codec;
    int sample_rate;
    int channels;
    int bit_width;
    int frame_size;
} audio_module_state_t;

/* Audio module functions - these will be called by the module system */

/**
 * Initialize audio module
 * @param config Audio module configuration
 * @return 0 on success, -1 on error
 */
int audio_module_init(void* config);

/**
 * Start audio module
 * @return 0 on success, -1 on error
 */
int audio_module_start(void);

/**
 * Stop audio module
 * @return 0 on success, -1 on error
 */
int audio_module_stop(void);

/**
 * Cleanup audio module
 * @return 0 on success, -1 on error
 */
int audio_module_cleanup(void);

/**
 * Parse audio configuration from JSON
 * @param json JSON configuration object
 * @param config Audio configuration structure to fill
 * @return 0 on success, -1 on error
 */
int audio_module_config_parse(json_object* json, void* config);

/**
 * Validate audio configuration
 * @param config Audio configuration to validate
 * @return 0 if valid, -1 if invalid
 */
int audio_module_config_validate(void* config);

/**
 * Free audio configuration resources
 * @param config Audio configuration to free
 */
void audio_module_config_free(void* config);

/**
 * Setup RTSP integration for audio
 * @param server RTSP server instance
 * @return 0 on success, -1 on error
 */
int audio_module_rtsp_setup(rtsp_server_t* server);

/**
 * RTSP frame callback for audio
 * @param server RTSP server instance
 * @param channel Video channel (audio is independent)
 * @return 0 on success, -1 on error
 */
int audio_module_rtsp_frame_callback(rtsp_server_t* server, int channel);

/**
 * Cleanup RTSP integration for audio
 * @param server RTSP server instance
 * @return 0 on success, -1 on error
 */
int audio_module_rtsp_cleanup(rtsp_server_t* server);

/**
 * Get audio module statistics
 * @param stats_buffer Buffer to fill with statistics
 * @param buffer_size Size of statistics buffer
 * @return 0 on success, -1 on error
 */
int audio_module_get_stats(void* stats_buffer, size_t buffer_size);

/* Utility functions */

/**
 * Convert audio format string to codec enum
 * @param format_str Format string ("G711A", "G711U", "AAC", "OPUS", "PCM")
 * @return audio_codec_t enum value
 */
audio_codec_t string_to_audio_codec(const char* format_str);

/**
 * Convert audio codec enum to string
 * @param codec Audio codec enum
 * @return Format string
 */
const char* audio_codec_to_string(audio_codec_t codec);

/**
 * Get RTP payload type for audio codec
 * @param codec Audio codec enum
 * @return RTP payload type
 */
int audio_codec_to_rtp_payload_type(audio_codec_t codec);

#endif /* __AUDIO_MODULE_H__ */
