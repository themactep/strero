/*
 * Image Grabbing Module Implementation
 * Copyright (C) 2025 Paul Philippov, Thingino Project
 */

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <imp/imp_encoder.h>
#include <imp/imp_framesource.h>

#include "../../common.h"
#include "../../module_system.h"
#include "image_grab_module.h"

#ifdef ENABLE_HTTP
#include "../http/http_router.h"
#endif

#define TAG "IMAGE_GRAB"

/* Global module state */
static image_grab_state_t g_image_grab_state = {0};

/* External global config */
extern streamer_config_t* g_config;

/* Forward declarations */
static image_grab_result_t grab_jpeg_frame(image_grab_request_t* request);
static image_grab_result_t grab_raw_frame(image_grab_request_t* request);

/* Module lifecycle functions */
int image_grab_module_init(void* config)
{
    IMP_LOG_INFO(TAG, "Initializing image grab module");

    if (g_image_grab_state.initialized) {
        IMP_LOG_WARN(TAG, "Image grab module already initialized");
        return 0;
    }

    /* Initialize configuration with defaults */
    g_image_grab_state.config.enabled = true;
    g_image_grab_state.config.timeout_ms = 5000;  /* 5 second timeout */
    g_image_grab_state.config.default_jpeg_quality = 85;

    /* Parse module-specific config if provided */
    if (config) {
        image_grab_config_t* grab_config = (image_grab_config_t*)config;
        g_image_grab_state.config = *grab_config;
    }

    /* Initialize mutex */
    if (pthread_mutex_init(&g_image_grab_state.grab_mutex, NULL) != 0) {
        IMP_LOG_ERR(TAG, "Failed to initialize grab mutex");
        return -1;
    }

    g_image_grab_state.initialized = true;
    IMP_LOG_INFO(TAG, "Image grab module initialized successfully");
    return 0;
}

int image_grab_module_start(void)
{
    IMP_LOG_INFO(TAG, "Starting image grab module");

    if (!g_image_grab_state.initialized) {
        IMP_LOG_ERR(TAG, "Image grab module not initialized");
        return -1;
    }

    if (!g_image_grab_state.config.enabled) {
        IMP_LOG_INFO(TAG, "Image grab module disabled in configuration");
        return 0;
    }

#ifdef ENABLE_HTTP
    /* Register HTTP routes only when module is enabled */
    if (image_grab_register_routes() < 0) {
        IMP_LOG_ERR(TAG, "Failed to register HTTP routes");
        return -1;
    }
#endif

    g_image_grab_state.running = true;
    IMP_LOG_INFO(TAG, "Image grab module started successfully");
    return 0;
}

int image_grab_module_stop(void)
{
    IMP_LOG_INFO(TAG, "Stopping image grab module");

    g_image_grab_state.running = false;
    IMP_LOG_INFO(TAG, "Image grab module stopped");
    return 0;
}

int image_grab_module_cleanup(void)
{
    IMP_LOG_INFO(TAG, "Cleaning up image grab module");

    if (g_image_grab_state.initialized) {
        pthread_mutex_destroy(&g_image_grab_state.grab_mutex);
        memset(&g_image_grab_state, 0, sizeof(g_image_grab_state));
    }

    IMP_LOG_INFO(TAG, "Image grab module cleaned up");
    return 0;
}

/* Main image capture function */
image_grab_result_t image_grab_capture(image_grab_request_t* request)
{
    if (!g_image_grab_state.initialized || !g_image_grab_state.running) {
        IMP_LOG_ERR(TAG, "Image grab module not running");
        return IMAGE_GRAB_ERROR_CAPTURE_FAILED;
    }

    if (!request || !request->output_buffer || !request->actual_size) {
        IMP_LOG_ERR(TAG, "Invalid grab request parameters");
        return IMAGE_GRAB_ERROR_CAPTURE_FAILED;
    }

    /* Validate channel */
    if (request->channel < 0 || request->channel >= g_config->stream_count) {
        IMP_LOG_ERR(TAG, "Invalid channel %d", request->channel);
        return IMAGE_GRAB_ERROR_INVALID_CHANNEL;
    }

    /* Check if channel is enabled */
    if (!g_config->streams[request->channel].enabled) {
        IMP_LOG_ERR(TAG, "Channel %d is not enabled", request->channel);
        return IMAGE_GRAB_ERROR_INVALID_CHANNEL;
    }

    /* Thread-safe capture */
    pthread_mutex_lock(&g_image_grab_state.grab_mutex);

    image_grab_result_t result;
    switch (request->format) {
        case IMAGE_FORMAT_JPEG:
            result = grab_jpeg_frame(request);
            break;
        case IMAGE_FORMAT_NV12:
        case IMAGE_FORMAT_YUYV422:
            result = grab_raw_frame(request);
            break;
        default:
            IMP_LOG_ERR(TAG, "Unsupported image format %d", request->format);
            result = IMAGE_GRAB_ERROR_INVALID_FORMAT;
            break;
    }

    pthread_mutex_unlock(&g_image_grab_state.grab_mutex);
    return result;
}

/* Utility functions */
size_t image_grab_calculate_buffer_size(int width, int height, image_format_t format)
{
    switch (format) {
        case IMAGE_FORMAT_JPEG:
            /* Estimate JPEG size as width * height (conservative estimate) */
            return width * height;
        case IMAGE_FORMAT_NV12:
            /* NV12: Y plane + UV plane (Y=w*h, UV=w*h/2) */
            return width * height * 3 / 2;
        case IMAGE_FORMAT_YUYV422:
            /* YUYV422: packed format, 2 bytes per pixel (16bpp) */
            return width * height * 2;
        default:
            return 0;
    }
}

const char* image_grab_result_string(image_grab_result_t result)
{
    switch (result) {
        case IMAGE_GRAB_SUCCESS:                return "Success";
        case IMAGE_GRAB_ERROR_INVALID_CHANNEL:  return "Invalid channel";
        case IMAGE_GRAB_ERROR_INVALID_FORMAT:   return "Invalid format";
        case IMAGE_GRAB_ERROR_BUFFER_TOO_SMALL: return "Buffer too small";
        case IMAGE_GRAB_ERROR_CAPTURE_FAILED:   return "Capture failed";
        case IMAGE_GRAB_ERROR_ENCODE_FAILED:    return "Encode failed";
        case IMAGE_GRAB_ERROR_TIMEOUT:          return "Timeout";
        default:                                return "Unknown error";
    }
}

/* JPEG frame capture implementation */
static image_grab_result_t grab_jpeg_frame(image_grab_request_t* request)
{
    int jpeg_channel = 4 + request->channel;  /* JPEG channels are 4 and 5 */
    IMPEncoderStream stream;
    int ret;

    IMP_LOG_DBG(TAG, "Capturing JPEG frame from channel %d (JPEG channel %d)",
                request->channel, jpeg_channel);

    /* Start receiving pictures for JPEG capture */
    ret = IMP_Encoder_StartRecvPic(jpeg_channel);
    if (ret < 0) {
        IMP_LOG_ERR(TAG, "IMP_Encoder_StartRecvPic(%d) failed", jpeg_channel);
        return IMAGE_GRAB_ERROR_CAPTURE_FAILED;
    }
    IMP_LOG_DBG(TAG, "Started receiving pictures for JPEG channel %d", jpeg_channel);

    /* Poll for JPEG frame */
    ret = IMP_Encoder_PollingStream(jpeg_channel, g_image_grab_state.config.timeout_ms);
    if (ret < 0) {
        IMP_LOG_ERR(TAG, "JPEG polling timeout on channel %d", jpeg_channel);
        IMP_Encoder_StopRecvPic(jpeg_channel);  /* Stop receiving on timeout */
        return IMAGE_GRAB_ERROR_TIMEOUT;
    }

    ret = IMP_Encoder_GetStream(jpeg_channel, &stream, 1);
    if (ret < 0) {
        IMP_LOG_ERR(TAG, "Failed to get JPEG stream from channel %d", jpeg_channel);
        IMP_Encoder_StopRecvPic(jpeg_channel);  /* Stop receiving on error */
        return IMAGE_GRAB_ERROR_CAPTURE_FAILED;
    }

    /* Check buffer size */
    if (stream.streamSize > request->buffer_size) {
        IMP_LOG_ERR(TAG, "JPEG frame size %u exceeds buffer size %zu",
                    stream.streamSize, request->buffer_size);
        IMP_Encoder_ReleaseStream(jpeg_channel, &stream);
        IMP_Encoder_StopRecvPic(jpeg_channel);  /* Stop receiving on error */
        return IMAGE_GRAB_ERROR_BUFFER_TOO_SMALL;
    }

    /* Copy JPEG data */
    void* jpeg_data = (void*)(uintptr_t)stream.virAddr;
    memcpy(request->output_buffer, jpeg_data, stream.streamSize);
    *request->actual_size = stream.streamSize;

    /* Release stream */
    IMP_Encoder_ReleaseStream(jpeg_channel, &stream);

    /* Stop receiving pictures */
    IMP_Encoder_StopRecvPic(jpeg_channel);

    IMP_LOG_DBG(TAG, "Successfully captured JPEG frame: %zu bytes", *request->actual_size);
    return IMAGE_GRAB_SUCCESS;
}

/* Raw frame capture implementation (NV12/YUV420) */
static image_grab_result_t grab_raw_frame(image_grab_request_t* request)
{
    int ret;
    IMPPixelFormat fmt;

    IMP_LOG_DBG(TAG, "Capturing raw frame from channel %d, format %d",
                request->channel, request->format);

    /* Get actual dimensions from global config */
    extern streamer_config_t* g_config;
    int width = g_config->streams[request->channel].width;
    int height = g_config->streams[request->channel].height;

    /* Determine IMP pixel format */
    switch (request->format) {
        case IMAGE_FORMAT_NV12:
            fmt = PIX_FMT_NV12;
            break;
        case IMAGE_FORMAT_YUYV422:
            fmt = PIX_FMT_YUYV422;  /* YUYV422 packed format */
            break;
        default:
            IMP_LOG_ERR(TAG, "Unsupported raw format %d", request->format);
            return IMAGE_GRAB_ERROR_INVALID_FORMAT;
    }

    /* Calculate frame size */
    size_t frame_size = image_grab_calculate_buffer_size(width, height, request->format);

    /* Check buffer size */
    if (frame_size > request->buffer_size) {
        IMP_LOG_ERR(TAG, "Raw frame size %zu exceeds buffer size %zu",
                    frame_size, request->buffer_size);
        return IMAGE_GRAB_ERROR_BUFFER_TOO_SMALL;
    }

    /* Use SnapFrame for direct raw capture */
    IMPFrameInfo frame_info;
    ret = IMP_FrameSource_SnapFrame(request->channel, fmt, width, height,
                                    request->output_buffer, &frame_info);
    if (ret < 0) {
        IMP_LOG_ERR(TAG, "IMP_FrameSource_SnapFrame failed for channel %d: %d",
                    request->channel, ret);
        return IMAGE_GRAB_ERROR_CAPTURE_FAILED;
    }

    /* SnapFrame copies data directly to our buffer */
    *request->actual_size = frame_size;

    IMP_LOG_DBG(TAG, "Successfully captured raw frame: %dx%d, %zu bytes",
                width, height, *request->actual_size);
    return IMAGE_GRAB_SUCCESS;
}

#ifdef ENABLE_HTTP
/* Route registration for HTTP module */
int image_grab_register_routes(void)
{
    /* Import handler functions from image_grab_http.c */
    extern void handle_image0_jpg(int client_socket, const char* query_string);
    extern void handle_image1_jpg(int client_socket, const char* query_string);
    extern void handle_image0_nv12(int client_socket, const char* query_string);
    extern void handle_image1_nv12(int client_socket, const char* query_string);
    extern void handle_image0_yuyv422(int client_socket, const char* query_string);
    extern void handle_image1_yuyv422(int client_socket, const char* query_string);

    /* Define routes */
    static const http_route_t image_grab_routes[] = {
        {"/image0.jpg", HTTP_METHOD_GET, handle_image0_jpg, "image_grab", "Channel 0 JPEG capture"},
        {"/image1.jpg", HTTP_METHOD_GET, handle_image1_jpg, "image_grab", "Channel 1 JPEG capture"},
        {"/image0.nv12", HTTP_METHOD_GET, handle_image0_nv12, "image_grab", "Channel 0 NV12 raw capture"},
        {"/image1.nv12", HTTP_METHOD_GET, handle_image1_nv12, "image_grab", "Channel 1 NV12 raw capture"},
        {"/image0.yuyv422", HTTP_METHOD_GET, handle_image0_yuyv422, "image_grab", "Channel 0 YUYV422 raw capture"},
        {"/image1.yuyv422", HTTP_METHOD_GET, handle_image1_yuyv422, "image_grab", "Channel 1 YUYV422 raw capture"},
    };

    /* Register all routes */
    int ret = http_router_register_routes(image_grab_routes,
                                         sizeof(image_grab_routes) / sizeof(image_grab_routes[0]));
    if (ret < 0) {
        IMP_LOG_ERR(TAG, "Failed to register HTTP routes");
        return ret;
    }

    IMP_LOG_INFO(TAG, "Registered %zu HTTP routes",
                 sizeof(image_grab_routes) / sizeof(image_grab_routes[0]));
    return 0;
}
#endif

int image_grab_module_get_config_size(void)
{
    return sizeof(image_grab_config_t);
}

int image_grab_module_set_defaults(void* config)
{
    if (!config) {
        return -1;
    }

    image_grab_config_t* grab_config = (image_grab_config_t*)config;
    memset(grab_config, 0, sizeof(image_grab_config_t));

    /* Set default values */
    grab_config->enabled = true;
    grab_config->timeout_ms = 5000;  /* 5 second timeout */
    grab_config->default_jpeg_quality = 85;

    return 0;
}

/* Module registration - following the established pattern */
module_info_t image_grab_module_info = {
    .name = IMAGE_GRAB_MODULE_NAME,
    .version = IMAGE_GRAB_MODULE_VERSION,
    .description = "Image grabbing module for JPEG/NV12/YUV capture",
    .state = MODULE_STATE_UNREGISTERED,
    .module_data = &g_image_grab_state,

    /* Lifecycle callbacks */
    .init = image_grab_module_init,
    .start = image_grab_module_start,
    .stop = image_grab_module_stop,
    .cleanup = image_grab_module_cleanup,

    /* Configuration */
    .config_size = sizeof(image_grab_config_t),

    /* RTSP integration - not needed for this module */
    .rtsp_setup = NULL,
    .rtsp_frame_callback = NULL,
    .rtsp_cleanup = NULL,

    /* Statistics - not implemented */
    .get_stats = NULL
};

/* Auto-register module at startup */
MODULE_REGISTER(image_grab_module_info);
