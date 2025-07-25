/*
 * http_module.h - HTTP Server Module Interface
 * Copyright (C) 2025 Paul Philippov, Thingino Project
 */

#ifndef __HTTP_MODULE_H__
#define __HTTP_MODULE_H__

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

#include <json-c/json.h>

#include "../../module_system.h"
#include "../../auth_utils.h"

#define HTTP_MODULE_VERSION "1.0.0"
#define HTTP_MODULE_NAME "http"

/* HTTP module configuration structure */
typedef struct {
    bool enabled;                     /* Enable/disable HTTP server */
    int port;                         /* HTTP server port */
    int max_connections;              /* Maximum concurrent connections */
    int request_timeout_ms;           /* Request timeout in milliseconds */

    /* Endpoint configuration */
    struct {
        bool snapshots;               /* Enable snapshot endpoints */
        bool mjpeg_streams;           /* Enable MJPEG streaming endpoints */
        bool json_api;                /* Enable JSON API endpoints */
        bool metrics;                 /* Enable metrics endpoints */
        bool onvif;                   /* Enable ONVIF endpoints */
        bool api_overview;            /* Enable API overview page */
    } endpoints;

    /* Security configuration */
    auth_config_t auth;               /* Authentication configuration */
    struct {
        char allowed_origins[256];    /* CORS allowed origins */
    } security;

    /* Performance tuning */
    struct {
        int buffer_size;              /* HTTP buffer size */
        int snapshot_buffer_size;     /* Snapshot buffer size */
        bool keep_alive;              /* Enable HTTP keep-alive */
        int keep_alive_timeout;       /* Keep-alive timeout seconds */
    } performance;
} http_module_config_t;

/* Module interface */
extern module_info_t http_module_info;

/* HTTP module functions */
int http_module_init(void* config);
int http_module_start(void);
int http_module_stop(void);
int http_module_cleanup(void);
int http_module_config_parse(json_object* json, void* config);
int http_module_get_config_size(void);
int http_module_set_defaults(void* config);

/* Module registration function */
int register_http_module(void);

/* HTTP server core functions (from existing code) */
int http_server_init(void);
void http_server_cleanup(void);
void* http_server_thread(void* arg);

/* Endpoint handlers (from existing code) */
void handle_snapshot_request(int client_socket, int channel);
void handle_mjpeg_stream(int client_socket, int channel);
void handle_json_request(int client_socket, const char* endpoint);
void handle_mp4_request(int client_socket, int channel);

/* Global state - exposed for compatibility with existing endpoint handlers */
extern bool http_server_running;
extern int http_server_socket;
extern pthread_t http_thread;

/* HTTP status codes - standard constants */
#define HTTP_STATUS_OK                    200
#define HTTP_STATUS_CREATED               201
#define HTTP_STATUS_NO_CONTENT            204
#define HTTP_STATUS_BAD_REQUEST           400
#define HTTP_STATUS_UNAUTHORIZED          401
#define HTTP_STATUS_FORBIDDEN             403
#define HTTP_STATUS_NOT_FOUND             404
#define HTTP_STATUS_METHOD_NOT_ALLOWED    405
#define HTTP_STATUS_INTERNAL_SERVER_ERROR 500
#define HTTP_STATUS_NOT_IMPLEMENTED       501
#define HTTP_STATUS_SERVICE_UNAVAILABLE   503

/* HTTP status code utility */
const char* http_get_status_text(int status_code);

/* HTTP response utilities - available to all modules */
void http_send_response(int client_socket, int status_code, const char* content_type, const char* body);
void http_send_error(int client_socket, int status_code, const char* message);
void http_send_json(int client_socket, const char* json_body);
void http_send_binary(int client_socket, const char* content_type, const void* data, size_t data_size);

/* MJPEG streaming utilities - specialized for multipart responses */
int http_send_mjpeg_stream_header(int client_socket);
int http_send_mjpeg_frame_header(int client_socket, size_t jpeg_size);

#endif /* __HTTP_MODULE_H__ */
