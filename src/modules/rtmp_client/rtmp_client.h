/*
 * rtmp_client.h - RTMP Client Module Implementation
 * Modular RTMP client for Thingino Streamer
 * Supports multiple concurrent RTMP connections with automatic reconnection
 * Also supports RTMPS (RTMP over TLS) for secure connections
 * Copyright (C) 2025 Paul Philippov, Thingino Project
 */

#ifndef __RTMP_CLIENT_H__
#define __RTMP_CLIENT_H__

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

#include "../../module_system.h"

#define RTMP_CLIENT_MODULE_VERSION "1.0.0"
#define RTMP_CLIENT_MODULE_NAME "rtmp_client"

/* RTMP protocol constants (shared with server) */
#define RTMP_VERSION 3
#define RTMP_HANDSHAKE_SIZE 1536
#define RTMP_DEFAULT_CHUNK_SIZE 4096
#define RTMP_DEFAULT_PORT 1935

/* RTMP message types */
#define RTMP_MSG_SET_CHUNK_SIZE     1
#define RTMP_MSG_ABORT_MESSAGE      2
#define RTMP_MSG_ACKNOWLEDGEMENT    3
#define RTMP_MSG_USER_CONTROL       4
#define RTMP_MSG_WINDOW_ACK_SIZE    5
#define RTMP_MSG_SET_PEER_BANDWIDTH 6
#define RTMP_MSG_AUDIO              8
#define RTMP_MSG_VIDEO              9
#define RTMP_MSG_DATA_AMF3          15
#define RTMP_MSG_SHARED_OBJECT_AMF3 16
#define RTMP_MSG_COMMAND_AMF3       17
#define RTMP_MSG_DATA_AMF0          18
#define RTMP_MSG_SHARED_OBJECT_AMF0 19
#define RTMP_MSG_COMMAND_AMF0       20
#define RTMP_MSG_AGGREGATE          22

/* AMF0 data types */
#define AMF0_NUMBER                 0x00
#define AMF0_BOOLEAN                0x01
#define AMF0_STRING                 0x02
#define AMF0_OBJECT                 0x03
#define AMF0_NULL                   0x05
#define AMF0_UNDEFINED              0x06
#define AMF0_OBJECT_END             0x09

/* RTMP command names */
#define RTMP_CMD_CONNECT            "connect"
#define RTMP_CMD_CREATE_STREAM      "createStream"
#define RTMP_CMD_PUBLISH            "publish"
#define RTMP_CMD_PLAY               "play"
#define RTMP_CMD_DELETE_STREAM      "deleteStream"
#define RTMP_CMD_CLOSE_STREAM       "closeStream"
#define RTMP_CMD_RESULT             "_result"
#define RTMP_CMD_ERROR              "_error"
#define RTMP_CMD_ON_STATUS          "onStatus"

/* RTMP chunk types */
#define RTMP_CHUNK_TYPE_0           0
#define RTMP_CHUNK_TYPE_1           1
#define RTMP_CHUNK_TYPE_2           2
#define RTMP_CHUNK_TYPE_3           3

/* RTMP client connection states */
typedef enum {
    RTMP_CLIENT_STATE_DISCONNECTED = 0,
    RTMP_CLIENT_STATE_CONNECTING,
    RTMP_CLIENT_STATE_HANDSHAKE_C0_SENT,
    RTMP_CLIENT_STATE_HANDSHAKE_C1_SENT,
    RTMP_CLIENT_STATE_HANDSHAKE_C2_SENT,
    RTMP_CLIENT_STATE_HANDSHAKE_DONE,
    RTMP_CLIENT_STATE_CONNECTED,
    RTMP_CLIENT_STATE_STREAM_CREATED,
    RTMP_CLIENT_STATE_PUBLISHING,
    RTMP_CLIENT_STATE_ERROR
} rtmp_client_state_t;

/* RTMP stream target configuration */
typedef struct {
    char name[64];                    /* Stream target name (e.g., "youtube", "twitch") */
    bool enabled;                     /* Enable/disable this stream target */
    char url[512];                    /* RTMP server URL */
    char stream_key[256];             /* Stream key for authentication */
    int retry_interval;               /* Retry interval in seconds */
    int max_retries;                  /* Maximum retry attempts */
    int connection_timeout;           /* Connection timeout in seconds */
} rtmp_stream_target_t;

/* RTMP client configuration */
typedef struct {
    bool enabled;                     /* Enable/disable RTMP client */
    rtmp_stream_target_t* targets;    /* Array of stream targets */
    int target_count;                 /* Number of stream targets */

    /* Video settings */
    struct {
        int channel;                  /* Video channel to stream (0 or 1) */
        int bitrate_limit;            /* Maximum bitrate in kbps (0 = no limit) */
        int fps_limit;                /* Maximum FPS (0 = no limit) */
    } video;

    /* Connection settings */
    struct {
        int timeout;                  /* Connection timeout in seconds */
        int chunk_size;               /* RTMP chunk size */
        int keepalive_interval;       /* Keepalive interval in seconds */
    } connection;
} rtmp_client_config_t;

/* RTMP chunk header */
typedef struct {
    uint8_t fmt;                      /* Chunk type (0-3) */
    uint32_t chunk_stream_id;         /* Chunk stream ID */
    uint32_t timestamp;               /* Message timestamp */
    uint32_t message_length;          /* Message length */
    uint8_t message_type_id;          /* Message type */
    uint32_t message_stream_id;       /* Message stream ID */
} rtmp_client_chunk_header_t;

/* RTMP message */
typedef struct {
    rtmp_client_chunk_header_t header;
    uint8_t* payload;
    uint32_t payload_size;
    uint32_t bytes_written;
} rtmp_client_message_t;

/* RTMP client connection */
typedef struct rtmp_client_connection {
    /* Connection info */
    char name[64];                    /* Target name */
    char url[512];                    /* RTMP URL */
    char stream_key[256];             /* Stream key */
    bool use_tls;                     /* Use TLS/SSL encryption */

    /* Parsed URL components (cached to avoid re-parsing) */
    char host[256];                   /* Parsed hostname */
    int port;                         /* Parsed port number */
    char app[256];                    /* Parsed application name */
    char stream[256];                 /* Parsed stream name (usually empty) */

    /* Per-connection stream state */
    bool avc_header_sent;             /* Whether AVC sequence header was sent for this connection */
    unsigned long avc_wait_start_us;  /* When connection started waiting for AVC header parameters */
    int frames_without_keyframe;      /* Frame counter for keyframe detection */
    uint8_t* cached_sps;              /* Cached SPS data for this connection */
    uint32_t cached_sps_size;         /* Size of cached SPS data */
    uint8_t* cached_pps;              /* Cached PPS data for this connection */
    uint32_t cached_pps_size;         /* Size of cached PPS data */

    /* Network */
    int socket_fd;                    /* Socket file descriptor */
    rtmp_client_state_t state;        /* Connection state */

    /* TLS/SSL context (for RTMPS) */
    void* ssl_context;                /* SSL context (OpenSSL SSL* or mbedTLS ssl_context) */
    void* ssl_config;                 /* SSL configuration (OpenSSL SSL_CTX* or mbedTLS ssl_config) */
    void* entropy_context;            /* Random context (mbedTLS only) */
    void* ctr_drbg_context;           /* Random generator context (mbedTLS only) */

    /* Threading */
    pthread_t thread;                 /* Connection thread */
    bool thread_running;              /* Thread running flag */

    /* RTMP protocol */
    uint32_t chunk_size_in;           /* Incoming chunk size */
    uint32_t chunk_size_out;          /* Outgoing chunk size */
    uint32_t window_ack_size;         /* Window acknowledgement size */
    double stream_id;                 /* Stream ID from createStream */

    /* RTMP acknowledgement tracking */
    uint32_t bytes_received;          /* Total bytes received */
    uint32_t last_ack_sent;           /* Last acknowledgement sent */

    /* Handshake data */
    uint8_t c1_s1_data[RTMP_HANDSHAKE_SIZE];
    uint8_t c2_s2_data[RTMP_HANDSHAKE_SIZE];

    /* Statistics */
    uint64_t bytes_sent;              /* Total bytes sent */
    uint64_t frames_sent;             /* Total frames sent */
    time_t connected_time;            /* Connection start time */
    time_t last_frame_time;           /* Last frame sent time */

    /* Retry logic */
    int retry_count;                  /* Current retry count */
    time_t last_retry_time;           /* Last retry attempt time */

    /* Next connection in list */
    struct rtmp_client_connection* next;
} rtmp_client_connection_t;

/* RTMP client manager */
typedef struct {
    bool running;                     /* Client manager running */
    pthread_t manager_thread;         /* Manager thread */
    rtmp_client_connection_t* connections; /* List of connections */
    pthread_mutex_t connections_mutex; /* Connections list mutex */
    rtmp_client_config_t config;      /* Client configuration */
} rtmp_client_t;

/* AMF value structure */
typedef struct amf_client_value {
    uint8_t type;
    union {
        double number;
        uint8_t boolean;
        struct {
            char* data;
            uint16_t length;
        } string;
        struct {
            struct amf_client_property* properties;
            int count;
        } object;
    } value;
} amf_client_value_t;

/* AMF object property */
typedef struct amf_client_property {
    char* name;
    amf_client_value_t value;
} amf_client_property_t;

/* Module interface */
extern module_info_t rtmp_client_module_info;

/* Module lifecycle functions */
int rtmp_client_module_init(void* config);
int rtmp_client_module_start(void);
int rtmp_client_module_stop(void);
int rtmp_client_module_cleanup(void);
int rtmp_client_module_get_config_size(void);
int rtmp_client_module_config_parse(json_object* json, void* config);
int rtmp_client_module_config_validate(void* config);
int rtmp_client_module_set_defaults(void* config);

/* Module registration function */
int register_rtmp_client_module(void);

/* RTSP server integration */
struct rtsp_server;
int rtmp_client_module_set_rtsp_server(struct rtsp_server* server);

/* RTSP frame callback for RTMP client module */
int rtmp_client_module_rtsp_frame_callback(struct rtsp_server* server, int channel, const uint8_t* frame_data, uint32_t frame_size, const struct timeval* timestamp);

/* RTMP client access for other modules */
rtmp_client_t* rtmp_client_module_get_client(void);

/* RTMP client protocol functions */
int rtmp_client_handshake_process(rtmp_client_connection_t* conn);
int rtmp_client_connect(rtmp_client_connection_t* conn);
int rtmp_client_create_stream(rtmp_client_connection_t* conn);
int rtmp_client_publish(rtmp_client_connection_t* conn);
int rtmp_client_send_video_frame(rtmp_client_connection_t* conn, const uint8_t* frame_data, uint32_t frame_size, uint32_t timestamp);

/* RTMP client message functions */
int rtmp_client_chunk_write(rtmp_client_connection_t* conn, rtmp_client_message_t* msg);
int rtmp_client_chunk_read(rtmp_client_connection_t* conn, rtmp_client_message_t* msg);
int rtmp_client_message_parse(rtmp_client_connection_t* conn, rtmp_client_message_t* msg);

/* AMF encoding/decoding functions for client */
int amf_client_encode_number(uint8_t** buffer, size_t* buffer_size, double number);
int amf_client_encode_boolean(uint8_t** buffer, size_t* buffer_size, uint8_t boolean);
int amf_client_encode_string(uint8_t** buffer, size_t* buffer_size, const char* string);
int amf_client_encode_null(uint8_t** buffer, size_t* buffer_size);
int amf_client_encode_object_start(uint8_t** buffer, size_t* buffer_size);
int amf_client_encode_object_end(uint8_t** buffer, size_t* buffer_size);

int amf_client_decode_value(const uint8_t* buffer, size_t buffer_size, size_t* offset, amf_client_value_t* value);
void amf_client_value_free(amf_client_value_t* value);

/* RTMP client connection management */
int rtmp_client_connection_create(const rtmp_stream_target_t* target, rtmp_client_connection_t** conn);
void rtmp_client_connection_destroy(rtmp_client_connection_t* conn);
int rtmp_client_connection_start(rtmp_client_connection_t* conn);
int rtmp_client_connection_stop(rtmp_client_connection_t* conn);

/* RTMP client frame distribution */
int rtmp_client_send_frame(rtmp_client_t* client, int channel, const uint8_t* frame_data, uint32_t frame_size, const struct timeval* timestamp);

/* Utility functions */
int rtmp_client_get_active_connection_count(void);

#endif /* __RTMP_CLIENT_H__ */
