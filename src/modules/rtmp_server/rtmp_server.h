/*
 * rtmp_server.h - RTMP Server Module Implementation
 * Modular RTMP server for Thingino Streamer
 * Supports multiple concurrent RTMP connections with authentication
 * Also supports RTMPS (RTMP over TLS) for secure connections
 * Copyright (C) 2025 Paul Philippov, Thingino Project
 */

#ifndef __RTMP_SERVER_H__
#define __RTMP_SERVER_H__

#include "rtmp_server.h"
#include <stdbool.h>
#include <stdint.h>

#include "../../module_system.h"

#define RTMP_SERVER_MODULE_VERSION "1.0.0"
#define RTMP_SERVER_MODULE_NAME "rtmp_server"

/* RTMP protocol constants */
#define RTMP_VERSION 3
#define RTMP_HANDSHAKE_SIZE 1536
#define RTMP_DEFAULT_CHUNK_SIZE 65536
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
#define RTMP_MSG_SHARED_OBJ_AMF3    16
#define RTMP_MSG_COMMAND_AMF3       17
#define RTMP_MSG_DATA_AMF0          18
#define RTMP_MSG_SHARED_OBJ_AMF0    19
#define RTMP_MSG_COMMAND_AMF0       20
#define RTMP_MSG_AGGREGATE          22

/* AMF0 data types */
#define AMF0_NUMBER                 0x00
#define AMF0_BOOLEAN                0x01
#define AMF0_STRING                 0x02
#define AMF0_OBJECT                 0x03
#define AMF0_MOVIECLIP              0x04
#define AMF0_NULL                   0x05
#define AMF0_UNDEFINED              0x06
#define AMF0_REFERENCE              0x07
#define AMF0_ECMA_ARRAY             0x08
#define AMF0_OBJECT_END             0x09
#define AMF0_STRICT_ARRAY           0x0A
#define AMF0_DATE                   0x0B
#define AMF0_LONG_STRING            0x0C
#define AMF0_UNSUPPORTED            0x0D
#define AMF0_RECORDSET              0x0E
#define AMF0_XML_DOCUMENT           0x0F
#define AMF0_TYPED_OBJECT           0x10

/* RTMP command names */
#define RTMP_CMD_CONNECT            "connect"
#define RTMP_CMD_CALL               "call"
#define RTMP_CMD_CLOSE              "close"
#define RTMP_CMD_CREATE_STREAM      "createStream"
#define RTMP_CMD_DELETE_STREAM      "deleteStream"
#define RTMP_CMD_PUBLISH            "publish"
#define RTMP_CMD_PLAY               "play"
#define RTMP_CMD_PAUSE              "pause"
#define RTMP_CMD_SEEK               "seek"
#define RTMP_CMD_RESULT             "_result"
#define RTMP_CMD_ERROR              "_error"
#define RTMP_CMD_ON_STATUS          "onStatus"

/* RTMP status codes */
#define RTMP_STATUS_CONNECT_SUCCESS     "NetConnection.Connect.Success"
#define RTMP_STATUS_CONNECT_FAILED      "NetConnection.Connect.Failed"
#define RTMP_STATUS_CONNECT_REJECTED    "NetConnection.Connect.Rejected"
#define RTMP_STATUS_PUBLISH_START       "NetStream.Publish.Start"
#define RTMP_STATUS_PUBLISH_FAILED      "NetStream.Publish.Failed"
#define RTMP_STATUS_PUBLISH_REJECTED    "NetStream.Publish.BadName"
#define RTMP_STATUS_PLAY_START          "NetStream.Play.Start"
#define RTMP_STATUS_PLAY_FAILED         "NetStream.Play.Failed"
#define RTMP_STATUS_PLAY_STREAM_NOT_FOUND "NetStream.Play.StreamNotFound"

/* AMF value structure */
typedef struct amf_value {
    uint8_t type;
    union {
        double number;
        uint8_t boolean;
        struct {
            char* data;
            uint16_t length;
        } string;
        struct {
            struct amf_property* properties;
            int count;
        } object;
        struct {
            struct amf_value* values;
            uint32_t count;
        } array;
    } value;
} amf_value_t;

/* AMF object property */
typedef struct amf_property {
    char* name;
    amf_value_t value;
} amf_property_t;

/* RTMP command structure */
typedef struct {
    char* command_name;
    double transaction_id;
    amf_value_t command_object;
    amf_value_t* arguments;
    int argument_count;
} rtmp_command_t;

/* RTMP chunk types */
#define RTMP_CHUNK_TYPE_0 0  /* 11-byte header */
#define RTMP_CHUNK_TYPE_1 1  /* 7-byte header */
#define RTMP_CHUNK_TYPE_2 2  /* 3-byte header */
#define RTMP_CHUNK_TYPE_3 3  /* no header */

/* RTMP connection states */
typedef enum {
    RTMP_STATE_UNINITIALIZED,
    RTMP_STATE_VERSION_SENT,
    RTMP_STATE_ACK_SENT,
    RTMP_STATE_HANDSHAKE_DONE,
    RTMP_STATE_CONNECTED
} rtmp_state_t;

/* RTMP chunk header */
typedef struct {
    uint8_t fmt;              /* Chunk type (2 bits) */
    uint32_t chunk_stream_id; /* Chunk stream ID */
    uint32_t timestamp;       /* Message timestamp */
    uint32_t message_length;  /* Message length */
    uint8_t message_type_id;  /* Message type ID */
    uint32_t message_stream_id; /* Message stream ID */
} rtmp_chunk_header_t;

/* RTMP message */
typedef struct {
    rtmp_chunk_header_t header;
    uint8_t* payload;
    uint32_t payload_size;
    uint32_t bytes_read;
} rtmp_message_t;

/* RTMP connection */
typedef struct rtmp_connection {
    int socket_fd;
    rtmp_state_t state;

    /* Handshake data */
    uint8_t c0_s0_version;
    uint8_t c1_s1_data[RTMP_HANDSHAKE_SIZE];
    uint8_t c2_s2_data[RTMP_HANDSHAKE_SIZE];

    /* Chunk stream state */
    uint32_t chunk_size_in;
    uint32_t chunk_size_out;
    uint32_t window_ack_size;
    uint32_t bytes_received;
    uint32_t bytes_sent;

    /* Stream information */
    char app_name[256];
    char stream_key[256];
    bool publishing;

    /* Threading */
    pthread_t thread;
    bool thread_running;

    struct rtmp_connection* next;
} rtmp_connection_t;

/* RTMP server */
typedef struct {
    int server_socket;
    bool running;
    pthread_t accept_thread;
    rtmp_connection_t* connections;
    pthread_mutex_t connections_mutex;
    int port;
    int max_connections;
    int current_connections;
} rtmp_server_t;

/* RTMP module configuration structure */
typedef struct {
    bool enabled;                     /* Enable/disable RTMP server */
    int port;                         /* RTMP server port */
    int max_connections;              /* Maximum concurrent connections */
    int chunk_size;                   /* Default chunk size */
    bool auth_required;               /* Enable/disable authentication */
    char stream_key[256];             /* Required stream key for publishing */
    char app_name[64];                /* Application name (e.g., "live") */
    int connection_timeout;           /* Connection timeout in seconds */
} rtmp_server_config_t;

/* Module interface */
extern module_info_t rtmp_server_module_info;

/* Module lifecycle functions */
int rtmp_server_module_init(void* config);
int rtmp_server_module_start(void);
int rtmp_server_module_stop(void);
int rtmp_server_module_cleanup(void);
int rtmp_server_module_get_config_size(void);
int rtmp_server_module_config_parse(json_object* json, void* config);
int rtmp_server_module_config_validate(void* config);
int rtmp_server_module_set_defaults(void* config);

/* Module registration function */
int register_rtmp_server_module(void);

/* RTSP server integration */
struct rtsp_server;
int rtmp_server_module_set_rtsp_server(struct rtsp_server* server);

/* RTSP frame callback for RTMP server module */
int rtmp_server_module_rtsp_frame_callback(struct rtsp_server* server, int channel, const uint8_t* frame_data, uint32_t frame_size, const struct timeval* timestamp);

/* RTMP server access for other modules */
rtmp_server_t* rtmp_server_module_get_server(void);

/* RTMP protocol functions */
int rtmp_handshake_process(rtmp_connection_t* conn);
int rtmp_chunk_read(rtmp_connection_t* conn, rtmp_message_t* msg);
int rtmp_chunk_write(rtmp_connection_t* conn, rtmp_message_t* msg);
int rtmp_message_parse(rtmp_connection_t* conn, rtmp_message_t* msg);

/* AMF encoding/decoding functions */
int amf_encode_number(uint8_t** buffer, size_t* buffer_size, double number);
int amf_encode_boolean(uint8_t** buffer, size_t* buffer_size, uint8_t boolean);
int amf_encode_string(uint8_t** buffer, size_t* buffer_size, const char* string);
int amf_encode_null(uint8_t** buffer, size_t* buffer_size);
int amf_encode_object_start(uint8_t** buffer, size_t* buffer_size);
int amf_encode_object_property(uint8_t** buffer, size_t* buffer_size, const char* name, amf_value_t* value);
int amf_encode_object_end(uint8_t** buffer, size_t* buffer_size);

int amf_decode_value(const uint8_t* buffer, size_t buffer_size, size_t* offset, amf_value_t* value);
int amf_decode_string(const uint8_t* buffer, size_t buffer_size, size_t* offset, char** string, uint16_t* length);
int amf_decode_object(const uint8_t* buffer, size_t buffer_size, size_t* offset, amf_property_t** properties, int* count);

void amf_value_free(amf_value_t* value);
void amf_property_free(amf_property_t* property);
void amf_object_free(amf_property_t* properties, int count);

/* RTMP command handling functions */
int rtmp_command_parse(const uint8_t* buffer, size_t buffer_size, rtmp_command_t* command);
int rtmp_command_handle(rtmp_connection_t* conn, rtmp_command_t* command);
void rtmp_command_free(rtmp_command_t* command);

/* RTMP response functions */
int rtmp_send_connect_result(rtmp_connection_t* conn, double transaction_id, bool success);
int rtmp_send_create_stream_result(rtmp_connection_t* conn, double transaction_id, double stream_id);
int rtmp_send_publish_status(rtmp_connection_t* conn, const char* status_code, const char* description);
int rtmp_send_play_status(rtmp_connection_t* conn, const char* status_code, const char* description);

/* RTMP control message functions */
int rtmp_send_set_chunk_size(rtmp_connection_t* conn, uint32_t chunk_size);
int rtmp_send_acknowledgement(rtmp_connection_t* conn, uint32_t bytes_received);
int rtmp_send_window_ack_size(rtmp_connection_t* conn, uint32_t window_size);

/* RTMP video streaming functions */
int rtmp_send_video_frame(rtmp_connection_t* conn, const uint8_t* frame_data, uint32_t frame_size, uint32_t timestamp);
int rtmp_send_audio_frame(rtmp_connection_t* conn, const uint8_t* frame_data, uint32_t frame_size, uint32_t timestamp);

/* RTMP server frame distribution */
int rtmp_server_send_frame(rtmp_server_t* server, int channel, const uint8_t* frame_data, uint32_t frame_size, const struct timeval* timestamp);

#endif /* __RTMP_SERVER_H__ */
