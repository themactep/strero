/*
 * Image Grabbing Module Header
 * Copyright (C) 2025 Paul Philippov, Thingino Project
 */

#ifndef __IMAGE_GRAB_MODULE_H__
#define __IMAGE_GRAB_MODULE_H__

#include <stdbool.h>
#include <stdint.h>
#include <pthread.h>

#include "../../module_system.h"
#include "../../config.h"

#define IMAGE_GRAB_MODULE_VERSION "1.0.0"
#define IMAGE_GRAB_MODULE_NAME "image_grab"

/* Image format types */
typedef enum {
    IMAGE_FORMAT_JPEG = 0,
    IMAGE_FORMAT_NV12,
    IMAGE_FORMAT_YUYV422    /* Changed from YUV420 to YUYV422 (hardware supported) */
} image_format_t;

/* Image grab request structure */
typedef struct {
    int channel;                    /* Stream channel (0 or 1) */
    image_format_t format;          /* Image format */
    int width;                      /* Image width (0 = use channel default) */
    int height;                     /* Image height (0 = use channel default) */
    int quality;                    /* JPEG quality (1-100, ignored for raw formats) */
    char* output_buffer;            /* Output buffer (allocated by caller) */
    size_t buffer_size;             /* Size of output buffer */
    size_t* actual_size;            /* Actual size of captured image */
} image_grab_request_t;

/* Image grab result codes */
typedef enum {
    IMAGE_GRAB_SUCCESS = 0,
    IMAGE_GRAB_ERROR_INVALID_CHANNEL,
    IMAGE_GRAB_ERROR_INVALID_FORMAT,
    IMAGE_GRAB_ERROR_BUFFER_TOO_SMALL,
    IMAGE_GRAB_ERROR_CAPTURE_FAILED,
    IMAGE_GRAB_ERROR_ENCODE_FAILED,
    IMAGE_GRAB_ERROR_TIMEOUT
} image_grab_result_t;

/* Module configuration */
typedef struct {
    bool enabled;                   /* Module enabled */
    int timeout_ms;                 /* Capture timeout in milliseconds */
    int default_jpeg_quality;       /* Default JPEG quality */
} image_grab_config_t;

/* Module interface */
extern module_info_t image_grab_module_info;

/* Module state */
typedef struct {
    bool initialized;
    bool running;
    image_grab_config_t config;
    pthread_mutex_t grab_mutex;     /* Mutex for thread-safe grabbing */
} image_grab_state_t;

/* Module lifecycle functions */
int image_grab_module_init(void* config);
int image_grab_module_start(void);
int image_grab_module_stop(void);
int image_grab_module_cleanup(void);
int image_grab_module_get_config_size(void);
int image_grab_module_set_defaults(void* config);

/* Image grabbing functions */
image_grab_result_t image_grab_capture(image_grab_request_t* request);
const char* image_grab_result_string(image_grab_result_t result);

/* Utility functions */
size_t image_grab_calculate_buffer_size(int width, int height, image_format_t format);

/* HTTP endpoint handlers */
void handle_image0_jpg(int client_socket, const char* query_string);
void handle_image1_jpg(int client_socket, const char* query_string);
void handle_image0_nv12(int client_socket, const char* query_string);
void handle_image1_nv12(int client_socket, const char* query_string);
void handle_image0_yuyv422(int client_socket, const char* query_string);
void handle_image1_yuyv422(int client_socket, const char* query_string);

/* Route registration function */
int image_grab_register_routes(void);

#endif /* __IMAGE_GRAB_MODULE_H__ */
