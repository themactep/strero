/*
 * Image Grab HTTP Endpoints
 * Copyright (C) 2025 Paul Philippov, Thingino Project
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "image_grab_module.h"
#include "../http/http_module.h"
#include "../../common.h"

#define TAG "IMAGE_GRAB_HTTP"

/* HTTP endpoint handlers */
static void handle_image_channel_format(int client_socket, const char* query_string, int channel, image_format_t format);

/* Parse query parameters */
static int parse_quality_param(const char* query_string, int default_quality);

/* Register HTTP endpoints */
int image_grab_register_http_endpoints(void)
{
    /* Register image capture endpoints */
    /* This would need to be integrated with the HTTP module's routing system */
    IMP_LOG_INFO(TAG, "Image grab HTTP endpoints would be registered here");

    /* Available endpoints:
     * GET /image0.jpg?quality=85    - JPEG from channel 0
     * GET /image1.jpg?quality=95    - JPEG from channel 1
     * GET /image0.nv12              - NV12 raw from channel 0
     * GET /image1.nv12              - NV12 raw from channel 1
     * GET /image0.yuv420            - YUV420 raw from channel 0
     * GET /image1.yuv420            - YUV420 raw from channel 1
     */

    return 0;
}

/* Endpoint handlers for specific channel/format combinations */
void handle_image0_jpg(int client_socket, const char* query_string) {
    handle_image_channel_format(client_socket, query_string, 0, IMAGE_FORMAT_JPEG);
}

void handle_image1_jpg(int client_socket, const char* query_string) {
    handle_image_channel_format(client_socket, query_string, 1, IMAGE_FORMAT_JPEG);
}

void handle_image0_nv12(int client_socket, const char* query_string) {
    handle_image_channel_format(client_socket, query_string, 0, IMAGE_FORMAT_NV12);
}

void handle_image1_nv12(int client_socket, const char* query_string) {
    handle_image_channel_format(client_socket, query_string, 1, IMAGE_FORMAT_NV12);
}

void handle_image0_yuyv422(int client_socket, const char* query_string) {
    handle_image_channel_format(client_socket, query_string, 0, IMAGE_FORMAT_YUYV422);
}

void handle_image1_yuyv422(int client_socket, const char* query_string) {
    handle_image_channel_format(client_socket, query_string, 1, IMAGE_FORMAT_YUYV422);
}

/* Unified image capture endpoint handler */
static void handle_image_channel_format(int client_socket, const char* query_string, int channel, image_format_t format)
{
    image_grab_request_t request = {0};
    image_grab_result_t result;
    char* buffer = NULL;
    size_t buffer_size;
    size_t actual_size = 0;
    const char* content_type;

    /* Set up request parameters */
    request.channel = channel;
    request.format = format;
    request.quality = (format == IMAGE_FORMAT_JPEG) ? parse_quality_param(query_string, 85) : 0;

    /* Get actual dimensions from global config */
    extern streamer_config_t* g_config;
    if (!g_config) {
        IMP_LOG_ERR(TAG, "Global configuration not available!");
        return;
    }

    if (channel >= g_config->stream_count) {
        IMP_LOG_ERR(TAG, "Channel %d not found in config!", channel);
        return;
    }

    int width = g_config->streams[channel].width;
    int height = g_config->streams[channel].height;

    /* Determine content type and buffer size based on format */
    switch (format) {
        case IMAGE_FORMAT_JPEG:
            content_type = "image/jpeg";
            buffer_size = width * height;  /* Conservative estimate for JPEG */
            break;
        case IMAGE_FORMAT_NV12:
            content_type = "application/octet-stream";
            buffer_size = image_grab_calculate_buffer_size(width, height, IMAGE_FORMAT_NV12);
            break;
        case IMAGE_FORMAT_YUYV422:
            content_type = "application/octet-stream";
            buffer_size = image_grab_calculate_buffer_size(width, height, IMAGE_FORMAT_YUYV422);
            break;
        default:
            content_type = "application/octet-stream";
            buffer_size = width * height;
            break;
    }

    buffer = malloc(buffer_size);
    if (!buffer) {
        http_send_error(client_socket, HTTP_STATUS_INTERNAL_SERVER_ERROR, "Memory allocation error");
        return;
    }

    /* Set up request */
    request.output_buffer = buffer;
    request.buffer_size = buffer_size;
    request.actual_size = &actual_size;

    /* Capture image */
    result = image_grab_capture(&request);

    if (result == IMAGE_GRAB_SUCCESS) {
        IMP_LOG_DBG(TAG, "Captured %s image from channel %d: %zu bytes",
                    (format == IMAGE_FORMAT_JPEG) ? "JPEG" :
                    (format == IMAGE_FORMAT_NV12) ? "NV12" : "YUYV422",
                    channel, actual_size);

        /* Send HTTP response with image data */
        char header[256];
        snprintf(header, sizeof(header),
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: %s\r\n"
            "Content-Length: %zu\r\n"
            "Connection: close\r\n"
            "Cache-Control: no-cache\r\n"
            "\r\n", content_type, actual_size);

        safe_send(client_socket, header, strlen(header));
        safe_send(client_socket, buffer, actual_size);

        IMP_LOG_DBG(TAG, "Sent %s image from channel %d: %zu bytes",
                    (format == IMAGE_FORMAT_JPEG) ? "JPEG" :
                    (format == IMAGE_FORMAT_NV12) ? "NV12" : "YUYV422",
                    channel, actual_size);
    } else {
        /* Send error response using HTTP utility */
        char error_message[256];
        snprintf(error_message, sizeof(error_message), "Image capture failed: %s",
                 image_grab_result_string(result));
        http_send_error(client_socket, HTTP_STATUS_INTERNAL_SERVER_ERROR, error_message);

        IMP_LOG_ERR(TAG, "Image capture failed: %s", image_grab_result_string(result));
    }

    free(buffer);
}

/* Parameter parsing functions */
static int parse_quality_param(const char* query_string, int default_quality)
{
    if (!query_string) return default_quality;

    const char* quality_param = strstr(query_string, "quality=");
    if (quality_param) {
        int quality = atoi(quality_param + 8);
        return (quality >= 1 && quality <= 100) ? quality : default_quality;
    }

    return default_quality;
}
