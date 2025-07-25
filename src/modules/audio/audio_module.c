/*
 * audio_module.c - Audio module implementation
 * Self-contained audio module with config, capture, encoding and RTSP integration
 *
 * Copyright (C) 2025 Paul Philippov, Thingino Project
 */

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <imp/imp_audio.h>
#include <json-c/json.h>

#include "audio_module.h"

#define TAG "AUDIO_MODULE"

/* Global audio module state */
static audio_module_state_t g_audio_state = {0};

/* Forward declarations */
static void* audio_capture_thread(void* arg);
static int cleanup_audio_encoder(void);
static int cleanup_audio_input(void);
static int setup_audio_encoder(void);
static int setup_audio_input(void);

/* Module registration - manual registration due to symbol stripping */
module_info_t audio_module_info = {
    .name = AUDIO_MODULE_NAME,
    .version = AUDIO_MODULE_VERSION,
    .description = "Audio capture, encoding and streaming module",
    .state = MODULE_STATE_UNREGISTERED,
    .module_data = &g_audio_state,

    /* Lifecycle callbacks */
    .init = audio_module_init,
    .start = audio_module_start,
    .stop = audio_module_stop,
    .cleanup = audio_module_cleanup,

    /* Configuration callbacks */
    .config_parse = audio_module_config_parse,
    .config_validate = audio_module_config_validate,
    .config_free = audio_module_config_free,
    .config_size = sizeof(audio_module_config_t),

    /* RTSP integration */
    .rtsp_setup = audio_module_rtsp_setup,
    .rtsp_frame_callback = audio_module_rtsp_frame_callback,
    .rtsp_cleanup = audio_module_rtsp_cleanup,

    /* Statistics */
    .get_stats = audio_module_get_stats
};

/* Auto-register module at startup */
MODULE_REGISTER(audio_module_info);

int audio_module_init(void* config)
{
    if (!config) {
        IMP_LOG_ERR(TAG, "Invalid audio configuration");
        return -1;
    }

    if (g_audio_state.initialized) {
        IMP_LOG_WARN(TAG, "Audio module already initialized");
        return 0;
    }

    IMP_LOG_INFO(TAG, "Initializing audio module");

    /* Copy configuration */
    memcpy(&g_audio_state.config, config, sizeof(audio_module_config_t));

    /* Check if audio is enabled */
    if (!g_audio_state.config.input_enabled) {
        IMP_LOG_INFO(TAG, "Audio input disabled in configuration");
        g_audio_state.initialized = true;
        return 0;
    }

    /* Initialize audio parameters */
    g_audio_state.ai_device_id = AUDIO_DEVICE_ID;
    g_audio_state.ai_channel_id = AUDIO_CHANNEL_ID;
    g_audio_state.aenc_channel_id = AUDIO_ENCODER_CHANNEL_ID;

    /* Parse audio codec */
    g_audio_state.codec = string_to_audio_codec(g_audio_state.config.input_format);
    g_audio_state.sample_rate = g_audio_state.config.input_sample_rate > 0 ?
                                g_audio_state.config.input_sample_rate : AUDIO_SAMPLE_RATE_16K;
    g_audio_state.channels = 1; /* Mono */
    g_audio_state.bit_width = 16; /* 16-bit */
    g_audio_state.frame_size = AUDIO_FRAME_SIZE;

    /* Initialize mutex */
    if (pthread_mutex_init(&g_audio_state.mutex, NULL) != 0) {
        IMP_LOG_ERR(TAG, "Failed to initialize audio mutex");
        return -1;
    }

    /* Setup audio input */
    if (setup_audio_input() != 0) {
        IMP_LOG_ERR(TAG, "Failed to setup audio input");
        pthread_mutex_destroy(&g_audio_state.mutex);
        return -1;
    }

    /* Setup audio encoder */
    if (setup_audio_encoder() != 0) {
        IMP_LOG_ERR(TAG, "Failed to setup audio encoder");
        cleanup_audio_input();
        pthread_mutex_destroy(&g_audio_state.mutex);
        return -1;
    }

    g_audio_state.initialized = true;
    IMP_LOG_INFO(TAG, "Audio module initialized successfully (codec=%s, rate=%dHz)",
                 audio_codec_to_string(g_audio_state.codec), g_audio_state.sample_rate);

    return 0;
}

int audio_module_start(void)
{
    if (!g_audio_state.initialized) {
        IMP_LOG_ERR(TAG, "Audio module not initialized");
        return -1;
    }

    if (!g_audio_state.config.input_enabled) {
        IMP_LOG_INFO(TAG, "Audio input disabled, not starting");
        return 0;
    }

    if (g_audio_state.running) {
        IMP_LOG_WARN(TAG, "Audio module already running");
        return 0;
    }

    IMP_LOG_INFO(TAG, "Starting audio module");

    /* Enable audio input */
    int ret = IMP_AI_EnableChn(g_audio_state.ai_device_id, g_audio_state.ai_channel_id);
    if (ret < 0) {
        IMP_LOG_ERR(TAG, "IMP_AI_EnableChn failed: %d", ret);
        return -1;
    }

    /* Audio encoder is ready after creation - no StartRecvPic needed for T31 */

    /* Start capture thread */
    g_audio_state.running = true;
    ret = pthread_create(&g_audio_state.capture_thread, NULL, audio_capture_thread, NULL);
    if (ret != 0) {
        IMP_LOG_ERR(TAG, "Failed to create audio capture thread: %d", ret);
        g_audio_state.running = false;
        IMP_AI_DisableChn(g_audio_state.ai_device_id, g_audio_state.ai_channel_id);
        return -1;
    }

    IMP_LOG_INFO(TAG, "Audio module started successfully");
    return 0;
}

int audio_module_stop(void)
{
    if (!g_audio_state.running) {
        return 0;
    }

    IMP_LOG_INFO(TAG, "Stopping audio module");

    /* Stop capture thread */
    g_audio_state.running = false;
    if (g_audio_state.capture_thread) {
        pthread_join(g_audio_state.capture_thread, NULL);
        g_audio_state.capture_thread = 0;
    }

    /* Audio encoder stops automatically when channel is destroyed */

    /* Disable audio input */
    IMP_AI_DisableChn(g_audio_state.ai_device_id, g_audio_state.ai_channel_id);

    IMP_LOG_INFO(TAG, "Audio module stopped");
    return 0;
}

int audio_module_cleanup(void)
{
    if (!g_audio_state.initialized) {
        return 0;
    }

    IMP_LOG_INFO(TAG, "Cleaning up audio module");

    /* Stop if running */
    audio_module_stop();

    /* Cleanup audio components */
    cleanup_audio_encoder();
    cleanup_audio_input();

    /* Cleanup mutex */
    pthread_mutex_destroy(&g_audio_state.mutex);

    /* Reset state */
    memset(&g_audio_state, 0, sizeof(g_audio_state));

    IMP_LOG_INFO(TAG, "Audio module cleanup complete");
    return 0;
}

int audio_module_config_parse(json_object* json, void* config)
{
    if (!json || !config) {
        return -1;
    }

    audio_module_config_t* audio_config = (audio_module_config_t*)config;

    /* JSON root is the audio config directly (no wrapper) */
    json_object* audio_obj = json;

    /* Set defaults first */
    audio_config->input_enabled = false;
    audio_config->output_enabled = false;
    audio_config->input_sample_rate = AUDIO_SAMPLE_RATE_16K;
    audio_config->output_sample_rate = AUDIO_SAMPLE_RATE_16K;
    audio_config->input_bitrate = 64;
    audio_config->input_vol = 80;
    audio_config->input_gain = 25;
    strcpy(audio_config->input_format, "G711A");
    audio_config->input_agc_enabled = false;
    audio_config->input_agc_target_level_dbfs = 10;
    audio_config->input_agc_compression_gain_db = 0;
    audio_config->input_alc_gain = 0;
    audio_config->input_high_pass_filter = false;
    audio_config->input_noise_suppression = 0;
    audio_config->force_stereo = false;

    /* Parse audio configuration fields */
    json_object* field = NULL;

    if (json_object_object_get_ex(audio_obj, "input_enabled", &field)) {
        audio_config->input_enabled = json_object_get_boolean(field);
    }

    if (json_object_object_get_ex(audio_obj, "output_enabled", &field)) {
        audio_config->output_enabled = json_object_get_boolean(field);
    }

    if (json_object_object_get_ex(audio_obj, "input_sample_rate", &field)) {
        audio_config->input_sample_rate = json_object_get_int(field);
    }

    if (json_object_object_get_ex(audio_obj, "output_sample_rate", &field)) {
        audio_config->output_sample_rate = json_object_get_int(field);
    }

    if (json_object_object_get_ex(audio_obj, "input_bitrate", &field)) {
        audio_config->input_bitrate = json_object_get_int(field);
    }

    if (json_object_object_get_ex(audio_obj, "input_vol", &field)) {
        audio_config->input_vol = json_object_get_int(field);
    }

    if (json_object_object_get_ex(audio_obj, "input_gain", &field)) {
        audio_config->input_gain = json_object_get_int(field);
    }

    if (json_object_object_get_ex(audio_obj, "input_format", &field)) {
        const char* format_str = json_object_get_string(field);
        if (format_str) {
            strncpy(audio_config->input_format, format_str, sizeof(audio_config->input_format) - 1);
            audio_config->input_format[sizeof(audio_config->input_format) - 1] = '\0';
        }
    }

    if (json_object_object_get_ex(audio_obj, "input_agc_enabled", &field)) {
        audio_config->input_agc_enabled = json_object_get_boolean(field);
    }

    if (json_object_object_get_ex(audio_obj, "input_agc_target_level_dbfs", &field)) {
        audio_config->input_agc_target_level_dbfs = json_object_get_int(field);
    }

    if (json_object_object_get_ex(audio_obj, "input_agc_compression_gain_db", &field)) {
        audio_config->input_agc_compression_gain_db = json_object_get_int(field);
    }

    if (json_object_object_get_ex(audio_obj, "input_alc_gain", &field)) {
        audio_config->input_alc_gain = json_object_get_int(field);
    }

    if (json_object_object_get_ex(audio_obj, "input_high_pass_filter", &field)) {
        audio_config->input_high_pass_filter = json_object_get_boolean(field);
    }

    if (json_object_object_get_ex(audio_obj, "input_noise_suppression", &field)) {
        audio_config->input_noise_suppression = json_object_get_int(field);
    }

    if (json_object_object_get_ex(audio_obj, "force_stereo", &field)) {
        audio_config->force_stereo = json_object_get_boolean(field);
    }

    IMP_LOG_INFO(TAG, "Audio config loaded:");
    IMP_LOG_INFO(TAG, "  input_enabled: %s", audio_config->input_enabled ? "true" : "false");
    IMP_LOG_INFO(TAG, "  output_enabled: %s", audio_config->output_enabled ? "true" : "false");
    IMP_LOG_INFO(TAG, "  input_sample_rate: %d Hz", audio_config->input_sample_rate);
    IMP_LOG_INFO(TAG, "  output_sample_rate: %d Hz", audio_config->output_sample_rate);
    IMP_LOG_INFO(TAG, "  input_bitrate: %d kbps", audio_config->input_bitrate);
    IMP_LOG_INFO(TAG, "  input_vol: %d", audio_config->input_vol);
    IMP_LOG_INFO(TAG, "  input_gain: %d", audio_config->input_gain);
    IMP_LOG_INFO(TAG, "  input_format: %s", audio_config->input_format);
    IMP_LOG_INFO(TAG, "  input_agc_enabled: %s", audio_config->input_agc_enabled ? "true" : "false");
    IMP_LOG_INFO(TAG, "  input_agc_target_level_dbfs: %d", audio_config->input_agc_target_level_dbfs);
    IMP_LOG_INFO(TAG, "  input_agc_compression_gain_db: %d", audio_config->input_agc_compression_gain_db);
    IMP_LOG_INFO(TAG, "  input_alc_gain: %d", audio_config->input_alc_gain);
    IMP_LOG_INFO(TAG, "  input_high_pass_filter: %s", audio_config->input_high_pass_filter ? "true" : "false");
    IMP_LOG_INFO(TAG, "  input_noise_suppression: %d", audio_config->input_noise_suppression);
    IMP_LOG_INFO(TAG, "  force_stereo: %s", audio_config->force_stereo ? "true" : "false");

    return 0;
}

int audio_module_config_validate(void* config)
{
    if (!config) {
        return -1;
    }

    audio_module_config_t* audio_config = (audio_module_config_t*)config;

    /* Validate input sample rate */
    if (audio_config->input_sample_rate != AUDIO_SAMPLE_RATE_8K &&
        audio_config->input_sample_rate != AUDIO_SAMPLE_RATE_16K &&
        audio_config->input_sample_rate != AUDIO_SAMPLE_RATE_48K) {
        IMP_LOG_ERR(TAG, "Invalid input sample rate: %d", audio_config->input_sample_rate);
        return -1;
    }

    /* Validate bitrate */
    if (audio_config->input_bitrate < 8 || audio_config->input_bitrate > 320) {
        IMP_LOG_ERR(TAG, "Invalid input bitrate: %d", audio_config->input_bitrate);
        return -1;
    }

    /* Validate volume */
    if (audio_config->input_vol < 0 || audio_config->input_vol > 100) {
        IMP_LOG_ERR(TAG, "Invalid input volume: %d", audio_config->input_vol);
        return -1;
    }

    /* Validate format */
    audio_codec_t codec = string_to_audio_codec(audio_config->input_format);
    if (codec == AUDIO_CODEC_PCM && strcmp(audio_config->input_format, "PCM") != 0) {
        IMP_LOG_ERR(TAG, "Invalid audio format: %s", audio_config->input_format);
        return -1;
    }

    return 0;
}

void audio_module_config_free(void* config)
{
    /* Audio config has no dynamic allocations, nothing to free */
    (void)config;
}

int audio_module_rtsp_setup(rtsp_server_t* server)
{
    if (!server || !g_audio_state.initialized || !g_audio_state.config.input_enabled) {
        return 0;
    }

    IMP_LOG_INFO(TAG, "Setting up RTSP audio integration");

    /* TODO: Add audio stream to RTSP server */
    /* This would involve calling rtsp_server_add_audio_stream() or similar */

    return 0;
}

int audio_module_rtsp_frame_callback(rtsp_server_t* server, int channel, const uint8_t* frame_data, uint32_t frame_size, const struct timeval* timestamp)
{
    if (!server || !g_audio_state.running) {
        return 0;
    }

    /* TODO: Send audio frames to RTSP server */
    /* This would be called periodically to send audio data */

    return 0;
}

int audio_module_rtsp_cleanup(rtsp_server_t* server)
{
    if (!server) {
        return 0;
    }

    IMP_LOG_INFO(TAG, "Cleaning up RTSP audio integration");

    /* TODO: Remove audio stream from RTSP server */

    return 0;
}

int audio_module_get_stats(void* stats_buffer, size_t buffer_size)
{
    if (!stats_buffer || buffer_size < sizeof(audio_module_state_t)) {
        return -1;
    }

    pthread_mutex_lock(&g_audio_state.mutex);
    memcpy(stats_buffer, &g_audio_state, sizeof(audio_module_state_t));
    pthread_mutex_unlock(&g_audio_state.mutex);

    return 0;
}

/* Utility functions */
audio_codec_t string_to_audio_codec(const char* format_str)
{
    if (!format_str || strlen(format_str) == 0) {
        return AUDIO_CODEC_G711A; /* Default */
    }

    if (strcasecmp(format_str, "G711A") == 0)
        return AUDIO_CODEC_G711A;
    if (strcasecmp(format_str, "G711U") == 0)
        return AUDIO_CODEC_G711U;
    if (strcasecmp(format_str, "AAC") == 0)
        return AUDIO_CODEC_AAC;
    if (strcasecmp(format_str, "OPUS") == 0)
        return AUDIO_CODEC_OPUS;
    if (strcasecmp(format_str, "PCM") == 0)
        return AUDIO_CODEC_PCM;

    IMP_LOG_WARN(TAG, "Unknown audio format '%s', using G711A", format_str);
    return AUDIO_CODEC_G711A;
}

const char* audio_codec_to_string(audio_codec_t codec)
{
    switch (codec) {
        case AUDIO_CODEC_G711A: return "G711A";
        case AUDIO_CODEC_G711U: return "G711U";
        case AUDIO_CODEC_AAC: return "AAC";
        case AUDIO_CODEC_OPUS: return "OPUS";
        case AUDIO_CODEC_PCM: return "PCM";
        default: return "UNKNOWN";
    }
}

int audio_codec_to_rtp_payload_type(audio_codec_t codec)
{
    switch (codec) {
        case AUDIO_CODEC_G711A: return 8;   /* PCMA */
        case AUDIO_CODEC_G711U: return 0;   /* PCMU */
        case AUDIO_CODEC_AAC: return 97;    /* Dynamic */
        case AUDIO_CODEC_OPUS: return 98;   /* Dynamic */
        case AUDIO_CODEC_PCM: return 99;    /* Dynamic */
        default: return 97;
    }
}

/* Helper functions implementation */
static int setup_audio_input(void)
{
    IMP_LOG_INFO(TAG, "Setting up audio input");

    /* Set audio input device attributes */
    IMPAudioIOAttr ai_attr;
    ai_attr.samplerate = (IMPAudioSampleRate)g_audio_state.sample_rate;
    ai_attr.bitwidth = AUDIO_BIT_WIDTH_16;
    ai_attr.soundmode = AUDIO_SOUND_MODE_MONO;
    ai_attr.frmNum = 40;
    ai_attr.numPerFrm = g_audio_state.frame_size;
    ai_attr.chnCnt = 1;

    int ret = IMP_AI_SetPubAttr(g_audio_state.ai_device_id, &ai_attr);
    if (ret != 0) {
        IMP_LOG_ERR(TAG, "IMP_AI_SetPubAttr failed: %d", ret);
        return -1;
    }

    /* Enable audio input device */
    ret = IMP_AI_Enable(g_audio_state.ai_device_id);
    if (ret != 0) {
        IMP_LOG_ERR(TAG, "IMP_AI_Enable failed: %d", ret);
        return -1;
    }

    /* Set audio input channel attributes */
    IMPAudioIChnParam ai_chn_param;
    ai_chn_param.usrFrmDepth = 40;

    ret = IMP_AI_SetChnParam(g_audio_state.ai_device_id, g_audio_state.ai_channel_id, &ai_chn_param);
    if (ret != 0) {
        IMP_LOG_ERR(TAG, "IMP_AI_SetChnParam failed: %d", ret);
        IMP_AI_Disable(g_audio_state.ai_device_id);
        return -1;
    }

    IMP_LOG_INFO(TAG, "Audio input setup complete (rate=%d, channels=%d)",
                 g_audio_state.sample_rate, g_audio_state.channels);

    return 0;
}

static int setup_audio_encoder(void)
{
    IMP_LOG_INFO(TAG, "Setting up audio encoder");

    /* Set audio encoder attributes */
    IMPAudioEncChnAttr aenc_attr;
    aenc_attr.type = PT_G711A; /* Default, will be updated based on codec */
    aenc_attr.bufSize = 30;

    /* Set encoder type based on codec */
    switch (g_audio_state.codec) {
        case AUDIO_CODEC_G711A:
            aenc_attr.type = PT_G711A;
            break;
        case AUDIO_CODEC_G711U:
            aenc_attr.type = PT_G711U;
            break;
        case AUDIO_CODEC_AAC:
            IMP_LOG_WARN(TAG, "AAC codec not supported on T31, using G711A");
            aenc_attr.type = PT_G711A;
            break;
        default:
            IMP_LOG_WARN(TAG, "Unsupported codec %d, using G711A", g_audio_state.codec);
            aenc_attr.type = PT_G711A;
            break;
    }

    int ret = IMP_AENC_CreateChn(g_audio_state.aenc_channel_id, &aenc_attr);
    if (ret != 0) {
        IMP_LOG_ERR(TAG, "IMP_AENC_CreateChn failed: %d", ret);
        return -1;
    }

    IMP_LOG_INFO(TAG, "Audio encoder setup complete (codec=%s)",
                 audio_codec_to_string(g_audio_state.codec));

    return 0;
}

static int cleanup_audio_input(void)
{
    IMP_LOG_INFO(TAG, "Cleaning up audio input");

    /* Disable audio input device */
    IMP_AI_Disable(g_audio_state.ai_device_id);

    return 0;
}

static int cleanup_audio_encoder(void)
{
    IMP_LOG_INFO(TAG, "Cleaning up audio encoder");

    /* Destroy audio encoder channel */
    IMP_AENC_DestroyChn(g_audio_state.aenc_channel_id);

    return 0;
}

/* Audio capture thread function */
static void* audio_capture_thread(void* arg)
{
    (void)arg;

    IMP_LOG_INFO(TAG, "Audio capture thread started");

    while (g_audio_state.running) {
        /* Get audio frame from input */
        IMPAudioFrame audio_frame;
        int ret = IMP_AI_PollingFrame(g_audio_state.ai_device_id, g_audio_state.ai_channel_id, 1000);
        if (ret < 0) {
            if (g_audio_state.running) {
                IMP_LOG_WARN(TAG, "Audio polling timeout or error: %d", ret);
            }
            continue;
        }

        ret = IMP_AI_GetFrame(g_audio_state.ai_device_id, g_audio_state.ai_channel_id, &audio_frame, BLOCK);
        if (ret < 0) {
            IMP_LOG_ERR(TAG, "IMP_AI_GetFrame failed: %d", ret);
            continue;
        }

        /* Update statistics */
        pthread_mutex_lock(&g_audio_state.mutex);
        g_audio_state.frames_captured++;
        pthread_mutex_unlock(&g_audio_state.mutex);

        /* Send frame to encoder */
        ret = IMP_AENC_SendFrame(g_audio_state.aenc_channel_id, &audio_frame);
        if (ret < 0) {
            IMP_LOG_ERR(TAG, "IMP_AENC_SendFrame failed: %d", ret);
            IMP_AI_ReleaseFrame(g_audio_state.ai_device_id, g_audio_state.ai_channel_id, &audio_frame);
            continue;
        }

        /* Release input frame */
        IMP_AI_ReleaseFrame(g_audio_state.ai_device_id, g_audio_state.ai_channel_id, &audio_frame);

        /* Get encoded stream */
        IMPAudioStream encoded_stream;
        ret = IMP_AENC_PollingStream(g_audio_state.aenc_channel_id, 1000);
        if (ret < 0) {
            continue;
        }

        ret = IMP_AENC_GetStream(g_audio_state.aenc_channel_id, &encoded_stream, BLOCK);
        if (ret < 0) {
            IMP_LOG_ERR(TAG, "IMP_AENC_GetStream failed: %d", ret);
            continue;
        }

        /* Update statistics */
        pthread_mutex_lock(&g_audio_state.mutex);
        g_audio_state.frames_encoded++;
        g_audio_state.bytes_encoded += encoded_stream.len;
        pthread_mutex_unlock(&g_audio_state.mutex);

        /* TODO: Send encoded frame to RTSP server via callback */
        if (g_audio_state.frame_callback) {
            audio_frame_t frame = {
                .data = (uint8_t*)encoded_stream.stream,
                .size = encoded_stream.len,
                .timestamp = encoded_stream.timeStamp,
                .sequence = g_audio_state.frames_encoded,
                .codec = g_audio_state.codec
            };
            g_audio_state.frame_callback(&frame, g_audio_state.user_data);
        }

        /* Release encoded stream */
        IMP_AENC_ReleaseStream(g_audio_state.aenc_channel_id, &encoded_stream);
    }

    IMP_LOG_INFO(TAG, "Audio capture thread stopped");
    return NULL;
}
