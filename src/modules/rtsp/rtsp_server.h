/*
 * rtsp_server.h - Thingino RTSP Server
 * Handles basic RTSP protocol for H264/H265 streaming
 * Based on RFC 2326 (RTSP) and RFC 3984 (H.264 RTP)
 * Copyright (C) 2025 Paul Philippov, Thingino Project
 */

#ifndef __RTSP_SERVER_H__
#define __RTSP_SERVER_H__

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

#include <netinet/in.h>
#include <sys/time.h>

#include "../../auth_utils.h"

/* Forward declaration for stream_info_t */
typedef struct {
    int stream_num;
    bool initialized;
} stream_info_t;

/* Forward declaration for rtsp_server_t */
typedef struct rtsp_server rtsp_server_t;

/* Callback function types */
typedef void (*video_frame_callback_t)(rtsp_server_t* server, void* user_data);

/* Configuration constants */
#define MAX_RTSP_CLIENTS 8
#define MAX_RTSP_URL_LEN 256
#define MAX_RTSP_HEADER_LEN 1024
#define MAX_RTSP_BODY_LEN 4096
#define MAX_SDP_LEN 2048
#define MAX_RTP_PACKET_SIZE 1200 /* Conservative MTU-safe RTP packet size */
#define DEFAULT_RTSP_PORT 554

/* Maximum number of video streams */
#define MAX_VIDEO_STREAMS 2

/* RTSP methods */
typedef enum {
    RTSP_METHOD_UNKNOWN = 0,
    RTSP_METHOD_OPTIONS,
    RTSP_METHOD_DESCRIBE,
    RTSP_METHOD_SETUP,
    RTSP_METHOD_PLAY,
    RTSP_METHOD_PAUSE,
    RTSP_METHOD_TEARDOWN,
    RTSP_METHOD_GET_PARAMETER,
    RTSP_METHOD_SET_PARAMETER
} rtsp_method_t;

/* RTSP client state */
typedef enum {
    RTSP_CLIENT_STATE_INIT = 0,
    RTSP_CLIENT_STATE_READY,
    RTSP_CLIENT_STATE_PLAYING,
    RTSP_CLIENT_STATE_PAUSED
} rtsp_client_state_t;

/* Video codec types - match SDK values */
typedef enum {
    VIDEO_CODEC_H264 = 0,  /* IMP_ENC_TYPE_AVC */
    VIDEO_CODEC_H265 = 1   /* IMP_ENC_TYPE_HEVC */
} video_codec_t;

/* RTSP transport modes */
typedef enum { RTSP_TRANSPORT_UDP = 0, RTSP_TRANSPORT_TCP } rtsp_transport_mode_t;

/* RTSP client structure */
typedef struct rtsp_client {
    int socket_fd;
    struct sockaddr_in client_addr;
    rtsp_client_state_t state;

    /* Session info */
    char session_id[32];
    uint32_t cseq;

    /* RTP info */
    struct sockaddr_in rtp_addr;
    struct sockaddr_in rtcp_addr;
    int rtp_port;
    int rtcp_port;
    int rtp_socket_fd;
    uint16_t rtp_seq;
    uint32_t rtp_timestamp;
    uint32_t rtp_ssrc;

    /* Stream info */
    int video_channel;
    video_codec_t codec;
    rtsp_transport_mode_t transport_mode;

    /* Buffers */
    char request_buffer[MAX_RTSP_HEADER_LEN];
    int request_len;

    /* Timing */
    unsigned long last_activity_us;
    bool active;

    /* IDR frame handling */
    bool needs_idr;
    unsigned long idr_wait_start_us;  /* When client started waiting for IDR frame */

    /* ONVIF backchannel support */
    bool supports_backchannel;       /* Whether client supports ONVIF audio backchannel */

    /* TLS/SSL context (for RTSPS) */
    bool use_tls;                     /* Whether this client is using TLS */
    void* ssl_context;                /* SSL context (OpenSSL SSL* or mbedTLS ssl_context) */
    void* ssl_config;                 /* SSL configuration (OpenSSL SSL_CTX* or mbedTLS ssl_config) */
    void* entropy_context;            /* Random context (mbedTLS only) */
    void* ctr_drbg_context;           /* Random generator context (mbedTLS only) */
    pthread_mutex_t ssl_mutex;        /* Mutex to protect SSL operations from concurrent access */
} rtsp_client_t;

/* Video stream configuration */
typedef struct video_stream_config {
    int channel;
    video_codec_t codec;
    int width;
    int height;
    int fps;
    int bitrate;
    char stream_name[64];  /* e.g., "main", "sub" */
    char stream_info[128]; /* Description */
} video_stream_config_t;

/* RTSP server configuration */
typedef struct rtsp_server_config {
    int port;
    auth_config_t auth;               /* Authentication configuration */
    char server_name[128];
    int max_clients;
    int session_timeout;

    /* TLS/SSL configuration (for RTSPS) */
    bool tls_enabled;                 /* Enable RTSPS (RTSP over TLS) */
    int tls_port;                     /* RTSPS port (default 322) */
    char cert_file[256];              /* Path to certificate file */
    char key_file[256];               /* Path to private key file */
    bool tls_verify_client;           /* Require client certificate verification */
} rtsp_server_config_t;

/* GOP (Group of Pictures) cache for immediate streaming */
#define MAX_GOP_FRAMES 60
#define MAX_FRAME_SIZE (100 * 1024) /* 100KB max per frame */

typedef struct gop_frame {
    uint8_t* data;
    size_t size;
    struct timeval timestamp;
    bool is_idr;
} gop_frame_t;

typedef struct gop_cache {
    gop_frame_t frames[MAX_GOP_FRAMES];
    int frame_count;
    bool valid;
    size_t total_size;
    pthread_mutex_t mutex;
    time_t last_idr_request; /* Throttle IDR requests */
} gop_cache_t;

/* RTSP server structure */
struct rtsp_server {
    /* Configuration */
    rtsp_server_config_t config;

    /* Network */
    int listen_socket;
    int tls_listen_socket;            /* Separate socket for RTSPS */
    struct sockaddr_in server_addr;
    struct sockaddr_in tls_server_addr;

    /* Clients */
    rtsp_client_t clients[MAX_RTSP_CLIENTS];
    int client_count;

    /* Video streams */
    video_stream_config_t* streams;
    int stream_count;

    /* Threading */
    pthread_t server_thread;
    pthread_t rtp_thread;
    volatile bool running;
    volatile bool should_stop;

    /* Statistics */
    unsigned long total_connections;
    unsigned long total_bytes_sent;
    unsigned long total_packets_sent;

    /* GOP cache per channel */
    gop_cache_t gop_cache[MAX_VIDEO_STREAMS];

    /* Callback for getting video frames */
    video_frame_callback_t frame_callback;
    void* user_data;

    /* TLS/SSL context (for RTSPS server) */
    void* tls_context;                /* SSL_CTX* for OpenSSL or ssl_config for mbedTLS */
    void* tls_entropy;                /* Entropy context (mbedTLS only) */
    void* tls_ctr_drbg;               /* Random generator (mbedTLS only) */
    void* tls_cert_context;           /* Certificate context (mbedTLS only) */
    void* tls_key_context;            /* Private key context (mbedTLS only) */
};

/* Function declarations */

/**
 * Create RTSP server
 * @param config Server configuration
 * @return Pointer to server instance or NULL on failure
 */
rtsp_server_t* rtsp_server_create(const rtsp_server_config_t* config);

/**
 * Destroy RTSP server
 * @param server Server instance
 */
void rtsp_server_destroy(rtsp_server_t* server);

/**
 * Add video stream to server
 * @param server Server instance
 * @param stream_config Stream configuration
 * @return 0 on success, negative on error
 */
int rtsp_server_add_stream(rtsp_server_t* server,
                           const video_stream_config_t* stream_config);

/**
 * Start RTSP server
 * @param server Server instance
 * @param frame_callback Callback for getting video frames
 * @param user_data User data for callback
 * @return 0 on success, negative on error
 */
int rtsp_server_start(rtsp_server_t* server,
                      video_frame_callback_t frame_callback,
                      void* user_data);

/**
 * Stop RTSP server
 * @param server Server instance
 * @return 0 on success, negative on error
 */
int rtsp_server_stop(rtsp_server_t* server);

/**
 * Send video frame to all playing clients
 * @param server Server instance
 * @param channel Video channel
 * @param frame_data Frame data
 * @param frame_size Frame size
 * @param timestamp Frame timestamp
 * @return Number of clients frame was sent to
 */
int rtsp_server_send_frame(rtsp_server_t* server,
                           int channel,
                           const void* frame_data,
                           unsigned int frame_size,
                           const struct timeval* timestamp);

/**
 * Get server statistics
 * @param server Server instance
 * @param connections Total connections
 * @param bytes_sent Total bytes sent
 * @param packets_sent Total packets sent
 */
void rtsp_server_get_stats(rtsp_server_t* server,
                           unsigned long* connections,
                           unsigned long* bytes_sent,
                           unsigned long* packets_sent);

/**
 * Set default server configuration
 * @param config Configuration structure to fill
 */
void rtsp_server_set_default_config(rtsp_server_config_t* config);

/**
 * Initialize RTSP server
 * @param config Server configuration
 * @param inputs Array of stream info instances
 * @param num_inputs Number of stream inputs
 * @return 0 on success, negative on error
 */
int rtsp_server_init(rtsp_server_config_t* config, stream_info_t* inputs[], int num_inputs);

/**
 * Get client count for a specific channel
 * @param server Server instance
 * @param channel Video channel
 * @return Number of active clients for the channel
 */
int rtsp_server_get_client_count(rtsp_server_t* server, int channel);

/**
 * Convert format string to video codec enum
 * @param format_str Format string ("H264", "H265", "HEVC")
 * @return video_codec_t enum value
 */
video_codec_t string_to_video_codec(const char* format_str);

#endif /* __RTSP_SERVER_H__ */
