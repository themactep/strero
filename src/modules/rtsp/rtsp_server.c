/*
 * rtsp_server.c - Thingino RTSP Server
 * Handles basic RTSP protocol for H264/H265 streaming
 * Based on RFC 2326 (RTSP) and RFC 3984 (H.264 RTP)
 * Copyright (C) 2025 Paul Philippov, Thingino Project
 */

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <arpa/inet.h>

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>

#include "../../common.h"
#include "../../config.h"
#include "rtsp_server.h"
#include "hal/imp.h"


/* TLS includes for RTSPS support */
#ifdef RTSPS_BACKEND_OPENSSL
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/rand.h>
#elif defined(RTSPS_BACKEND_MBEDTLS)
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/error.h>
#include <mbedtls/x509.h>
#include <mbedtls/pk.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/x509_csr.h>
#include <mbedtls/rsa.h>
#include <mbedtls/bignum.h>
#endif

#define TAG "RTSP"

/* External function to get stream info by channel */
extern stream_info_t* get_video_input(int channel);

/* RTSP response codes */
#define RTSP_STATUS_OK 200
#define RTSP_STATUS_BAD_REQUEST 400
#define RTSP_STATUS_UNAUTHORIZED 401
#define RTSP_STATUS_NOT_FOUND 404
#define RTSP_STATUS_METHOD_NOT_ALLOWED 405
#define RTSP_STATUS_UNSUPPORTED_TRANSPORT 461
#define RTSP_STATUS_SECURE_CONNECTION_FAILURE 472  /* RFC 7826 - Failure to Establish Secure Connection */
#define RTSP_STATUS_INTERNAL_ERROR 500

/* RTP payload types */
#define RTP_PAYLOAD_TYPE_H264 96
#define RTP_PAYLOAD_TYPE_H265 97

/* Maximum buffer sizes */
#define MAX_RTSP_BUFFER_SIZE 4096
#define MAX_SDP_SIZE 2048

/* RTP header size */
#define RTP_HEADER_SIZE 12

/* Static function declarations */
static rtsp_client_t* find_free_client_slot(rtsp_server_t* server);
static void cleanup_client(rtsp_client_t* client);
static int extract_stream_name_from_url(rtsp_server_t* server, const char* url, char* stream_name, size_t stream_name_size);
static int generate_sdp(rtsp_server_t* server, const char* stream_name, char* sdp_buffer, size_t buffer_size, rtsp_client_t* client);
static void generate_session_id(char* session_id, size_t size);
static int handle_client_connection(rtsp_server_t* server, int client_index);
static int handle_describe_request(rtsp_server_t* server, rtsp_client_t* client, const char* url);
static int handle_options_request(rtsp_server_t* server, rtsp_client_t* client);
static int handle_pause_request(rtsp_server_t* server, rtsp_client_t* client);
static int handle_play_request(rtsp_server_t* server, rtsp_client_t* client);
static int handle_setup_request(rtsp_server_t* server, rtsp_client_t* client, const char* url, const char* transport);
static int handle_teardown_request(rtsp_server_t* server, rtsp_client_t* client);
static int parse_rtsp_request(const char* request, rtsp_method_t* method, char* url, int* cseq);
static void* rtp_thread_func(void* arg);

/* TLS-aware I/O functions */
static int rtsp_server_tls_init(rtsp_server_t* server);
static void rtsp_server_tls_cleanup(rtsp_server_t* server);
static int rtsp_client_tls_accept(rtsp_server_t* server, rtsp_client_t* client, int client_fd);
static void rtsp_client_tls_cleanup(rtsp_client_t* client);
static int rtsp_client_tls_read(rtsp_client_t* client, char* buffer, size_t length);
static int rtsp_client_tls_write(rtsp_client_t* client, const char* buffer, size_t length);
static int send_rtp_packet(rtsp_client_t* client, const void* data, unsigned int size, uint32_t timestamp, bool marker);
static int send_rtsp_response(rtsp_client_t* client, int status_code, const char* status_text, const char* headers, const char* body);
static void* server_thread_func(void* arg);

rtsp_server_t* rtsp_server_create(const rtsp_server_config_t* config)
{
    IMP_LOG_DBG(TAG, "Creating RTSP server on port %d", config->port);
    rtsp_server_t* server = calloc(1, sizeof(rtsp_server_t));
    if (!server) {
        IMP_LOG_ERR(TAG, "Failed to allocate RTSP server structure");
        return NULL;
    }

    /* Copy configuration */
    IMP_LOG_DBG(TAG, "Copying RTSP server configuration");
    memcpy(&server->config, config, sizeof(rtsp_server_config_t));

    /* Initialize state */
    server->listen_socket = -1;
    server->tls_listen_socket = -1;
    server->client_count = 0;
    server->stream_count = 0;
    server->streams = NULL;
    server->running = false;
    server->should_stop = false;

    /* Initialize TLS contexts */
    server->tls_context = NULL;
    server->tls_entropy = NULL;
    server->tls_ctr_drbg = NULL;

    /* Initialize statistics */
    server->total_connections = 0;
    server->total_bytes_sent = 0;
    server->total_packets_sent = 0;

    /* Initialize GOP cache */
    IMP_LOG_DBG(TAG, "Initializing GOP cache");
    for (int i = 0; i < MAX_VIDEO_STREAMS; i++) {
        IMP_LOG_DBG(TAG, "Initializing GOP cache for channel %d", i);
        server->gop_cache[i].frame_count = 0;
        server->gop_cache[i].valid = false;
        server->gop_cache[i].total_size = 0;
        server->gop_cache[i].last_idr_request = 0;
        pthread_mutex_init(&server->gop_cache[i].mutex, NULL);

        /* Initialize frame buffers */
        IMP_LOG_DBG(TAG, "Initializing frame buffers for channel %d", i);
        for (int j = 0; j < MAX_GOP_FRAMES; j++) {
            IMP_LOG_DBG(TAG, "Allocating frame buffer %d for channel %d", j, i);
            server->gop_cache[i].frames[j].data = malloc(MAX_FRAME_SIZE);
            server->gop_cache[i].frames[j].size = 0;
            server->gop_cache[i].frames[j].is_idr = false;
        }
    }

    /* Initialize clients */
    IMP_LOG_DBG(TAG, "Initializing clients");
    for (int i = 0; i < MAX_RTSP_CLIENTS; i++) {
        IMP_LOG_DBG(TAG, "Initializing client %d", i);
        memset(&server->clients[i], 0, sizeof(rtsp_client_t));
        server->clients[i].socket_fd = -1;
        server->clients[i].rtp_socket_fd = -1;
        server->clients[i].active = false;
        /* Initialize SSL mutex */
        pthread_mutex_init(&server->clients[i].ssl_mutex, NULL);
    }

    IMP_LOG_DBG(TAG, "RTSP server created successfully");
    return server;
}

void rtsp_server_destroy(rtsp_server_t* server)
{
    IMP_LOG_DBG(TAG, "Destroying RTSP server");
    if (!server) {
        IMP_LOG_ERR(TAG, "Invalid server for destroy");
        return;
    }

    /* Stop server if running */
    IMP_LOG_DBG(TAG, "Stopping RTSP server if running");
    if (server->running) {
        rtsp_server_stop(server);
        IMP_LOG_DBG(TAG, "RTSP server stopped");
    }

    /* Clean up streams */
    IMP_LOG_DBG(TAG, "Cleaning up streams");
    if (server->streams) {
        IMP_LOG_DBG(TAG, "Freeing streams array");
        free(server->streams);
    }

    /* Clean up clients */
    IMP_LOG_DBG(TAG, "Cleaning up clients");
    for (int i = 0; i < MAX_RTSP_CLIENTS; i++) {
        IMP_LOG_DBG(TAG, "Cleaning up client %d", i);
        cleanup_client(&server->clients[i]);
    }

    /* Clean up GOP cache */
    IMP_LOG_DBG(TAG, "Cleaning up GOP cache");
    for (int i = 0; i < MAX_VIDEO_STREAMS; i++) {
        IMP_LOG_DBG(TAG, "Cleaning up GOP cache for channel %d", i);
        pthread_mutex_destroy(&server->gop_cache[i].mutex);
        for (int j = 0; j < MAX_GOP_FRAMES; j++) {
            if (server->gop_cache[i].frames[j].data) {
                IMP_LOG_DBG(TAG, "Freeing frame buffer %d for channel %d", j, i);
                free(server->gop_cache[i].frames[j].data);
                server->gop_cache[i].frames[j].data = NULL;
            }
        }
    }

    /* Close listen socket */
    IMP_LOG_DBG(TAG, "Closing listen socket");
    if (server->listen_socket >= 0) {
        IMP_LOG_DBG(TAG, "Closing listen socket %d", server->listen_socket);
        close(server->listen_socket);
    }

    /* Close TLS listen socket */
    IMP_LOG_DBG(TAG, "Closing TLS listen socket");
    if (server->tls_listen_socket >= 0) {
        IMP_LOG_DBG(TAG, "Closing TLS listen socket %d", server->tls_listen_socket);
        close(server->tls_listen_socket);
    }

    /* Clean up TLS context */
    IMP_LOG_DBG(TAG, "Cleaning up TLS context");
    rtsp_server_tls_cleanup(server);

    /* Free server structure */
    free(server);
    IMP_LOG_DBG(TAG, "RTSP server destroyed");
}

int rtsp_server_add_stream(rtsp_server_t* server,
                           const video_stream_config_t* stream_config)
{
    if (!server || !stream_config) {
        IMP_LOG_ERR(TAG, "Invalid parameters for adding stream");
        return -1;
    }

    IMP_LOG_DBG(TAG,
                "Adding stream: %s (channel %d, %s)",
                stream_config->stream_name,
                stream_config->channel,
                stream_config->codec == VIDEO_CODEC_H265 ? "H265" : "H264");

    /* Reallocate streams array */
    IMP_LOG_DBG(TAG, "Reallocating streams array (current count: %d)", server->stream_count);
    video_stream_config_t* new_streams = realloc(server->streams,
                                                 (server->stream_count + 1)
                                                     * sizeof(video_stream_config_t));
    if (!new_streams) {
        IMP_LOG_ERR(TAG, "Failed to reallocate streams array");
        return -1;
    }

    server->streams = new_streams;

    /* Copy stream configuration */
    IMP_LOG_DBG(TAG, "Copying stream configuration");
    video_stream_config_t* stream = &server->streams[server->stream_count];
    memcpy(stream, stream_config, sizeof(video_stream_config_t));

    server->stream_count++;

    IMP_LOG_DBG(TAG, "Stream added successfully (total streams: %d)", server->stream_count);
    return 0;
}

/* Request IDR frame for immediate streaming when client connects */
static void request_idr_frame(int channel)
{
    extern int IMP_Encoder_RequestIDR(int encChn);

    /* Request IDR frame once */
    IMP_LOG_INFO(TAG, "Requesting IDR frame for channel %d", channel);
    int ret = IMP_Encoder_RequestIDR(channel);
    if (ret < 0) {
        IMP_LOG_ERR(TAG,
                    "WARNING: Failed to request IDR frame for channel %d: ret=%d",
                    channel,
                    ret);
    } else {
        IMP_LOG_INFO(TAG, "IDR frame requested for channel %d", channel);
    }
}

/* Global timestamp for immediate IDR requests to avoid conflicts */
static time_t g_last_immediate_idr = 0;

int rtsp_server_start(rtsp_server_t* server,
                              video_frame_callback_t frame_callback,
                              void* user_data)
{
    if (!server) {
        IMP_LOG_ERR(TAG, "Invalid server for start");
        return -1;
    }

    if (server->running) {
        IMP_LOG_ERR(TAG, "RTSP server already running");
        return 0;
    }

    IMP_LOG_INFO(TAG, "Starting RTSP server");

    /* Store callback */
    server->frame_callback = frame_callback;
    server->user_data = user_data;

    /* Create listen socket */
    IMP_LOG_INFO(TAG, "Creating listen socket...");
    server->listen_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server->listen_socket < 0) {
        IMP_LOG_ERR(TAG, "Failed to create listen socket: %s", strerror(errno));
        return -1;
    }
    IMP_LOG_INFO(TAG, "Listen socket created successfully");

    /* Set socket options */
    int opt = 1;
    if (setsockopt(server->listen_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        IMP_LOG_ERR(TAG, "Failed to set SO_REUSEADDR: %s", strerror(errno));
    }

    /* Bind socket */
    IMP_LOG_INFO(TAG, "Binding socket to port %d...", server->config.port);
    memset(&server->server_addr, 0, sizeof(server->server_addr));
    server->server_addr.sin_family = AF_INET;
    server->server_addr.sin_addr.s_addr = INADDR_ANY;
    server->server_addr.sin_port = htons(server->config.port);

    if (bind(server->listen_socket,
             (struct sockaddr*) &server->server_addr,
             sizeof(server->server_addr))
        < 0) {
        IMP_LOG_ERR(TAG, "Failed to bind socket: %s", strerror(errno));
        close(server->listen_socket);
        server->listen_socket = -1;
        return -1;
    }
    IMP_LOG_INFO(TAG, "Socket bound successfully to port %d", server->config.port);

    /* Listen for connections */
    IMP_LOG_INFO(TAG, "Setting socket to listen mode...");
    if (listen(server->listen_socket, server->config.max_clients) < 0) {
        IMP_LOG_ERR(TAG, "Failed to listen on socket: %s", strerror(errno));
        close(server->listen_socket);
        server->listen_socket = -1;
        return -1;
    }
    IMP_LOG_INFO(TAG, "Socket listening successfully");

    /* Initialize TLS if enabled */
    if (server->config.tls_enabled) {
        IMP_LOG_INFO(TAG, "Initializing TLS for RTSPS server...");
        if (rtsp_server_tls_init(server) < 0) {
            IMP_LOG_ERR(TAG, "Failed to initialize TLS");
            close(server->listen_socket);
            server->listen_socket = -1;
            return -1;
        }

        /* Create TLS listen socket */
        IMP_LOG_INFO(TAG, "Creating TLS listen socket...");
        server->tls_listen_socket = socket(AF_INET, SOCK_STREAM, 0);
        if (server->tls_listen_socket < 0) {
            IMP_LOG_ERR(TAG, "Failed to create TLS listen socket: %s", strerror(errno));
            rtsp_server_tls_cleanup(server);
            close(server->listen_socket);
            server->listen_socket = -1;
            return -1;
        }

        /* Set TLS socket options */
        if (setsockopt(server->tls_listen_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
            IMP_LOG_ERR(TAG, "Failed to set SO_REUSEADDR on TLS socket: %s", strerror(errno));
        }

        /* Bind TLS socket */
        IMP_LOG_INFO(TAG, "Binding TLS socket to port %d...", server->config.tls_port);
        memset(&server->tls_server_addr, 0, sizeof(server->tls_server_addr));
        server->tls_server_addr.sin_family = AF_INET;
        server->tls_server_addr.sin_addr.s_addr = INADDR_ANY;
        server->tls_server_addr.sin_port = htons(server->config.tls_port);

        if (bind(server->tls_listen_socket,
                 (struct sockaddr*) &server->tls_server_addr,
                 sizeof(server->tls_server_addr)) < 0) {
            IMP_LOG_ERR(TAG, "Failed to bind TLS socket: %s", strerror(errno));
            close(server->tls_listen_socket);
            server->tls_listen_socket = -1;
            rtsp_server_tls_cleanup(server);
            close(server->listen_socket);
            server->listen_socket = -1;
            return -1;
        }
        IMP_LOG_INFO(TAG, "TLS socket bound successfully to port %d", server->config.tls_port);

        /* Listen for TLS connections */
        if (listen(server->tls_listen_socket, server->config.max_clients) < 0) {
            IMP_LOG_ERR(TAG, "Failed to listen on TLS socket: %s", strerror(errno));
            close(server->tls_listen_socket);
            server->tls_listen_socket = -1;
            rtsp_server_tls_cleanup(server);
            close(server->listen_socket);
            server->listen_socket = -1;
            return -1;
        }
        IMP_LOG_INFO(TAG, "TLS socket listening successfully on port %d", server->config.tls_port);
    }

    /* STEP 2: Re-enable RTSP server thread */
    IMP_LOG_INFO(TAG, "Starting RTSP server thread...");
    server->running = true;
    server->should_stop = false;

    int ret = pthread_create(&server->server_thread, NULL, server_thread_func, server);
    if (ret != 0) {
        IMP_LOG_ERR(TAG, "Failed to create server thread: %s", strerror(ret));
        server->running = false;
        close(server->listen_socket);
        server->listen_socket = -1;
        return -1;
    }
    IMP_LOG_INFO(TAG, "RTSP server thread started successfully");

    /* STEP 2: Re-enable RTP thread */
    IMP_LOG_INFO(TAG, "Starting RTP thread...");
    ret = pthread_create(&server->rtp_thread, NULL, rtp_thread_func, server);
    if (ret != 0) {
        IMP_LOG_ERR(TAG, "Failed to create RTP thread: %s", strerror(ret));
        server->should_stop = true;
        pthread_join(server->server_thread, NULL);
        server->running = false;
        close(server->listen_socket);
        server->listen_socket = -1;
        return -1;
    }
    IMP_LOG_INFO(TAG, "RTP thread started successfully");

    IMP_LOG_INFO(TAG, "RTSP server started successfully on port %d", server->config.port);
    return 0;
}

int rtsp_server_stop(rtsp_server_t* server)
{
    if (!server || !server->running) {
        return 0;
    }

    IMP_LOG_INFO(TAG, "Stopping RTSP server");

    /* Signal threads to stop */
    server->should_stop = true;

    /* Close listen socket to unblock accept() */
    if (server->listen_socket >= 0) {
        IMP_LOG_INFO(TAG, "Closing listen socket %d", server->listen_socket);
        close(server->listen_socket);
        server->listen_socket = -1;
    }

    /* Wait for threads to finish */
    IMP_LOG_INFO(TAG, "Waiting for server thread to finish...");
    pthread_join(server->server_thread, NULL);
    pthread_join(server->rtp_thread, NULL);

    /* Mark server as stopped */
    server->running = false;

    IMP_LOG_INFO(TAG, "RTSP server stopped");
    return 0;
}

int rtsp_server_get_client_count(rtsp_server_t* server, int channel)
{
    if (!server) {
        IMP_LOG_ERR(TAG, "Invalid server pointer");
        return 0;
    }

    if (channel < 0 || channel >= FS_CHN_NUM) {
        IMP_LOG_ERR(TAG, "Invalid channel %d (must be 0-%d)", channel, FS_CHN_NUM - 1);
        return 0;
    }

    /* Count active clients for the specified channel */
    // IMP_LOG_DBG(TAG, "Counting clients for channel %d", channel);
    int count = 0;
    for (int i = 0; i < MAX_RTSP_CLIENTS; i++) {
        // IMP_LOG_DBG(TAG, "Checking client %d: active=%s, state=%d, video_channel=%d",
        //            i,
        //            server->clients[i].active ? "true" : "false",
        //            server->clients[i].state,
        //            server->clients[i].video_channel);
        if (server->clients[i].active && server->clients[i].state == RTSP_CLIENT_STATE_PLAYING
            && server->clients[i].video_channel == channel) {
                // IMP_LOG_DBG(TAG, "Client %d is active and playing on channel %d", i, channel);
            count++;
        }
    }

    return count;
}

/* Calculate monotonic RTP timestamp per channel with real-time timing */
static uint32_t calculate_monotonic_rtp_timestamp(int channel)
{
    /* Static variables for each channel */
    static uint64_t last_frame_time[4] = {0, 0, 0, 0};
    static uint32_t base_timestamp[4] = {0, 0, 0, 0};
    static bool initialized[4] = {false, false, false, false};

    /* Get current time */
    uint64_t current_time = get_monotonic_time_us();

    /* Initialize on first call for this channel */
    if (!initialized[channel]) {
        base_timestamp[channel] = (uint32_t)(rand() % 100000);
        last_frame_time[channel] = current_time;
        initialized[channel] = true;
        IMP_LOG_INFO(TAG, "Real-time RTP timestamp initialized for channel %d: base=%u",
                     channel, base_timestamp[channel]);
        return base_timestamp[channel];
    }

    /* Calculate timestamp based on actual elapsed time */
    uint64_t elapsed_us = current_time - last_frame_time[channel];
    uint32_t timestamp_increment = (uint32_t)((elapsed_us * 90000) / 1000000); /* 90kHz clock */

    last_frame_time[channel] = current_time;
    base_timestamp[channel] += timestamp_increment;

    return base_timestamp[channel];
}

/* Send frame to a specific client (for cached GOP frames) */
static int send_frame_to_client(rtsp_server_t* server,
                                rtsp_client_t* client,
                                const void* frame_data,
                                unsigned int frame_size,
                                int channel)
{
    if (!server || !client || !frame_data || frame_size == 0) {
        IMP_LOG_ERR(TAG, "Invalid parameters for sending frame to client");
        return 0;
    }

    /* Only send to clients that are playing */
    if (client->state != RTSP_CLIENT_STATE_PLAYING) {
        return 0;
    }

    /* Simple frame sending - no GOP caching logic here since this is for cached frames */
    const uint8_t* data = (const uint8_t*) frame_data;

    /* Parse NAL units */
    size_t nal_offsets[32];
    size_t nal_sizes[32];
    int nal_count = 0;

    /* Find NAL unit boundaries - handle both 3-byte and 4-byte start codes */
    for (size_t offset = 0; offset < frame_size - 3;) {
        /* Check for 4-byte start code first: 0x00 0x00 0x00 0x01 */
        if (offset < frame_size - 4 &&
            data[offset] == 0x00 && data[offset + 1] == 0x00 &&
            data[offset + 2] == 0x00 && data[offset + 3] == 0x01) {

            /* Record size of previous NAL if exists */
            if (nal_count > 0) {
                nal_sizes[nal_count - 1] = offset - nal_offsets[nal_count - 1];
            }

            /* Skip the 4-byte start code and record this NAL */
            offset += 4;
            nal_offsets[nal_count] = offset;
            nal_count++;

        /* Check for 3-byte start code: 0x00 0x00 0x01 */
        } else if (data[offset] == 0x00 && data[offset + 1] == 0x00 && data[offset + 2] == 0x01) {
            /* Record size of previous NAL if exists */
            if (nal_count > 0) {
                nal_sizes[nal_count - 1] = offset - nal_offsets[nal_count - 1];
            }

            /* Skip the 3-byte start code and record this NAL */
            offset += 3;
            nal_offsets[nal_count] = offset;
            nal_count++;
        } else {
            offset++;
        }
    }

    /* Record size of last NAL */
    if (nal_count > 0) {
        nal_sizes[nal_count - 1] = frame_size - nal_offsets[nal_count - 1];
    }

    /* Use monotonic frame-based timestamp for smooth playback */
    uint32_t rtp_timestamp = calculate_monotonic_rtp_timestamp(channel);

    /* Send NAL units to client */
    for (int i = 0; i < nal_count; i++) {
        if (nal_offsets[i] < frame_size && nal_sizes[i] > 0) {
            send_rtp_packet(client,
                            data + nal_offsets[i],
                            nal_sizes[i],
                            rtp_timestamp,
                            i == nal_count - 1);
        }
    }

    return nal_count;
}

int rtsp_server_send_frame(rtsp_server_t* server,
                           int channel,
                           const void* frame_data,
                           unsigned int frame_size,
                           const struct timeval* timestamp)
{
    if (!server || !server->running || !frame_data || frame_size == 0) {
        IMP_LOG_ERR(TAG, "Invalid parameters for sending frame");
        return 0;
    }

    /* Frame rate debugging - track frames sent to RTSP */
    static unsigned long last_rtsp_report_time = 0;
    static int rtsp_frame_count = 0;
    rtsp_frame_count++;

    unsigned long current_time = get_monotonic_time_us();
    if (last_rtsp_report_time == 0) {
        last_rtsp_report_time = current_time;
    } else if (current_time - last_rtsp_report_time >= 5000000) { /* 5 seconds */
        double elapsed_seconds = (current_time - last_rtsp_report_time) / 1000000.0;
        double rtsp_fps = rtsp_frame_count / elapsed_seconds;
        IMP_LOG_INFO(TAG, "RTSP frames sent rate: %.2f fps (%d frames in %.2f seconds)",
                    rtsp_fps, rtsp_frame_count, elapsed_seconds);
        last_rtsp_report_time = current_time;
        rtsp_frame_count = 0;
    }

    /* Use monotonic frame-based timestamp for smooth playback */
    uint32_t rtp_timestamp = calculate_monotonic_rtp_timestamp(channel);

    /* Get current time for debugging */
    // time_t now = time(NULL);
    // struct tm* tm_info = localtime(&now);
    // char time_str[32];
    // strftime(time_str, sizeof(time_str), "%H:%M:%S", tm_info);

    /* Find the stream configuration for this channel */
    video_stream_config_t* stream_config = NULL;
    for (int i = 0; i < server->stream_count; i++) {
        if (server->streams[i].channel == channel) {
            stream_config = &server->streams[i];
            break;
        }
    }

    /* Use encoder's timestamp consistently */
    uint32_t current_timestamp = rtp_timestamp;

    int clients_sent = 0;

    /* Find NAL units in the frame */
    const uint8_t* data = (const uint8_t*) frame_data;
    size_t nal_offsets[100]; /* Store offsets of NAL units */
    size_t nal_sizes[100];   /* Store sizes of NAL units */
    int nal_count = 0;

    /* Find NAL units (look for 0x00000001 or 0x000001) */
    size_t offset = 0;
    while (offset < frame_size && nal_count < 100) {
        /* Look for start code */
        if (offset + 4 <= frame_size && data[offset] == 0 && data[offset + 1] == 0
            && data[offset + 2] == 0 && data[offset + 3] == 1) {
            /* Found 4-byte start code, record previous NAL if any */
            if (nal_count > 0) {
                nal_sizes[nal_count - 1] = offset - nal_offsets[nal_count - 1];
            }

            /* Skip the start code and record this NAL */
            offset += 4;
            nal_offsets[nal_count] = offset;
            nal_count++;
        } else if (offset + 3 <= frame_size && data[offset] == 0 && data[offset + 1] == 0
                   && data[offset + 2] == 1) {
            /* Found 3-byte start code, record previous NAL if any */
            if (nal_count > 0) {
                nal_sizes[nal_count - 1] = offset - nal_offsets[nal_count - 1];
            }

            /* Skip the start code and record this NAL */
            offset += 3;
            nal_offsets[nal_count] = offset;
            nal_count++;
        } else {
            offset++;
        }
    }

    /* Record size of last NAL */
    if (nal_count > 0) {
        nal_sizes[nal_count - 1] = frame_size - nal_offsets[nal_count - 1];
    }

    /* Check if this is an IDR frame and manage GOP cache */
    bool is_idr_frame = false;
    /* Use SDK approach to determine codec type */
    hal_enc_attr_t a;
    bool is_hevc = false;
    if (channel < FS_CHN_NUM && hal_enc_get_attr(channel, &a) == 0) {
        is_hevc = (a.payload == HAL_PT_H265);
    }

    /* Check for IDR frame based on SDK codec type - check ALL NAL units */
    for (int i = 0; i < nal_count; i++) {
        if (nal_offsets[i] < frame_size && nal_sizes[i] > 0) {
            const uint8_t* nal_data = data + nal_offsets[i];
            size_t nal_size = nal_sizes[i];

            if (is_hevc) {
                /* H.265 IDR detection */
                if (nal_size >= 2) {
                    uint8_t nal_type = (nal_data[0] >> 1) & 0x3F;
                    /* H.265 key frames: IDR_W_RADL (19), IDR_N_LP (20), CRA_NUT (21) */
                    if (nal_type >= 19 && nal_type <= 21) {
                        is_idr_frame = true;
                        break;
                    }
                }
            } else {
                /* H.264 IDR detection */
                if (nal_size >= 1) {
                    uint8_t nal_type = nal_data[0] & 0x1F;
                    if (nal_type == 5) { /* IDR frame */
                        is_idr_frame = true;
                        break;
                    }
                }
            }
        }
    }

    /* GOP caching disabled - let IMP library handle parameter sets natively */
    if (is_idr_frame) {
        /* Log timing for IDR frame reception */
        unsigned long timestamp_us = (unsigned long)get_monotonic_time_us();
        // IMP_LOG_INFO(TAG, "IDR frame received from encoder for channel %d at %lu.%06lu (GOP cache disabled)",
        //             channel, timestamp_us / 1000000, timestamp_us % 1000000);
    }

    /* Add debugging for NAL types when clients are waiting for IDR */
    bool clients_waiting_for_idr = false;
    for (int i = 0; i < MAX_RTSP_CLIENTS; i++) {
        rtsp_client_t* client = &server->clients[i];
        if (client->active && client->state == RTSP_CLIENT_STATE_PLAYING &&
            client->video_channel == channel && client->needs_idr) {
            clients_waiting_for_idr = true;
            break;
        }
    }

    /* Debug NAL types when clients are waiting for IDR frames */
    if (clients_waiting_for_idr && nal_count > 0) {
        hal_enc_attr_t a;
        bool is_hevc = false;
        if (channel < FS_CHN_NUM && hal_enc_get_attr(channel, &a) == 0) {
            is_hevc = (a.payload == HAL_PT_H265);
        }

        for (int i = 0; i < nal_count && i < 3; i++) { /* Log first 3 NAL units */
            if (nal_offsets[i] < frame_size && nal_sizes[i] > 0) {
                const uint8_t* nal_data = data + nal_offsets[i];
                if (is_hevc && nal_sizes[i] >= 2) {
                    uint8_t nal_type = (nal_data[0] >> 1) & 0x3F;
                    // IMP_LOG_DBG(TAG, "H.265 NAL unit %d: type=%d, size=%zu", i, nal_type, nal_sizes[i]);
                } else if (!is_hevc && nal_sizes[i] >= 1) {
                    uint8_t nal_type = nal_data[0] & 0x1F;
                    // IMP_LOG_DBG(TAG, "H.264 NAL unit %d: type=%d, size=%zu", i, nal_type, nal_sizes[i]);
                }
            }
        }
    }

    /* Check if this frame contains parameter sets (SPS/PPS) that new clients need */
    bool has_parameter_sets = false;
    for (int i = 0; i < nal_count; i++) {
        if (nal_offsets[i] < frame_size && nal_sizes[i] > 0) {
            const uint8_t* nal_data = data + nal_offsets[i];
            size_t nal_size = nal_sizes[i];

            if (is_hevc) {
                /* H.265 parameter sets */
                if (nal_size >= 2) {
                    uint8_t nal_type = (nal_data[0] >> 1) & 0x3F;
                    /* VPS (32), SPS (33), PPS (34) */
                    if (nal_type >= 32 && nal_type <= 34) {
                        has_parameter_sets = true;
                        break;
                    }
                }
            } else {
                /* H.264 parameter sets */
                if (nal_size >= 1) {
                    uint8_t nal_type = nal_data[0] & 0x1F;
                    /* SPS (7), PPS (8) */
                    if (nal_type == 7 || nal_type == 8) {
                        has_parameter_sets = true;
                        break;
                    }
                }
            }
        }
    }

    /* Send frame to clients, honoring needs_idr flag to prevent SPS/PPS race condition */
    unsigned long current_time_us = get_monotonic_time_us();
    for (int i = 0; i < MAX_RTSP_CLIENTS; i++) {
        rtsp_client_t* client = &server->clients[i];

        if (client->active && client->state == RTSP_CLIENT_STATE_PLAYING && client->video_channel == channel) {

            if (client->needs_idr) {
                /* Check timeout - if client has been waiting more than 5 seconds, allow any frame */
                unsigned long wait_time_us = current_time_us - client->idr_wait_start_us;
                if (wait_time_us > 5000000) { /* 5 seconds in microseconds */
                    IMP_LOG_WARN(TAG, "Client %d timeout waiting for IDR frame (%lu.%06lu seconds), allowing any frame",
                                i, wait_time_us / 1000000, wait_time_us % 1000000);
                    client->needs_idr = false;
                    send_frame_to_client(server, client, frame_data, frame_size, channel);
                } else if (is_idr_frame) {
                    /* This is an IDR frame, send it! */
                    // IMP_LOG_INFO(TAG, "Sending initial IDR frame to new client %d after %lu.%06lu seconds wait",
                    //             i, wait_time_us / 1000000, wait_time_us % 1000000);
                    send_frame_to_client(server, client, frame_data, frame_size, channel);

                    /* The client has its keyframe, so we can clear the flag */
                    client->needs_idr = false;
                } else if (has_parameter_sets) {
                    /* This frame contains SPS/PPS - send it as new clients need parameter sets */
                    // IMP_LOG_INFO(TAG, "Sending parameter sets (SPS/PPS) to new client %d", i);
                    send_frame_to_client(server, client, frame_data, frame_size, channel);
                // } else {
                //     /* This is a P-frame or other non-essential frame. Skip/drop it for this new client */
                //     if (wait_time_us % 1000000 < 100000) { /* Log every ~1 second to avoid spam */
                //         IMP_LOG_DBG(TAG, "Dropping non-essential frame for new client %d (waiting %lu.%06lu seconds for keyframe)",
                //                    i, wait_time_us / 1000000, wait_time_us % 1000000);
                //     }
                }
            } else {
                /* This client is already streaming, send any frame */
                send_frame_to_client(server, client, frame_data, frame_size, channel);
            }
        }
    }

    return 0; /* Skip all the GOP cache code below */

    /* This GOP cache code should never execute when caching is disabled */
    if (0 && channel < MAX_VIDEO_STREAMS) {
        pthread_mutex_lock(&server->gop_cache[channel].mutex);

        /* If this is an IDR frame, start new GOP but preserve parameter sets */
        if (is_idr_frame) {
            /* Log timing for IDR frame reception */
            unsigned long timestamp_us = (unsigned long)get_monotonic_time_us();
            // IMP_LOG_INFO(TAG, "IDR frame received from encoder for channel %d at %lu.%06lu",
            //             channel, timestamp_us / 1000000, timestamp_us % 1000000);

            /* Preserve SPS/PPS parameter sets from previous GOP */
            gop_frame_t preserved_params[MAX_GOP_FRAMES];
            int preserved_count = 0;
            size_t preserved_size = 0;

            /* Extract parameter sets from current cache */
            for (int i = 0; i < server->gop_cache[channel].frame_count; i++) {
                gop_frame_t* frame = &server->gop_cache[channel].frames[i];
                const uint8_t* data = (const uint8_t*)frame->data;
                bool has_params = false;

                /* Check if frame contains SPS or PPS */
                for (size_t offset = 0; offset < frame->size - 4; offset++) {
                    if (data[offset] == 0x00 && data[offset + 1] == 0x00 &&
                        data[offset + 2] == 0x00 && data[offset + 3] == 0x01) {
                        uint8_t nal_type = data[offset + 4] & 0x1F;
                        if (nal_type == 7 || nal_type == 8) {
                            has_params = true;
                            break;
                        }
                    } else if (data[offset] == 0x00 && data[offset + 1] == 0x00 &&
                               data[offset + 2] == 0x01) {
                        uint8_t nal_type = data[offset + 3] & 0x1F;
                        if (nal_type == 7 || nal_type == 8) {
                            has_params = true;
                            break;
                        }
                    }
                }

                if (has_params && preserved_count < MAX_GOP_FRAMES) {
                    memcpy(&preserved_params[preserved_count], frame, sizeof(gop_frame_t));
                    preserved_size += frame->size;
                    preserved_count++;
                    // IMP_LOG_INFO(TAG, "Preserved parameter set frame for new GOP");
                }
            }

            /* Start new GOP */
            server->gop_cache[channel].frame_count = 0;
            server->gop_cache[channel].total_size = 0;

            /* Restore preserved parameter sets */
            for (int i = 0; i < preserved_count; i++) {
                memcpy(&server->gop_cache[channel].frames[server->gop_cache[channel].frame_count],
                       &preserved_params[i], sizeof(gop_frame_t));
                server->gop_cache[channel].frame_count++;
                server->gop_cache[channel].total_size += preserved_params[i].size;
            }

            // IMP_LOG_INFO(TAG, "New GOP started with %d preserved parameter sets", preserved_count);
        }

        /* Add frame to GOP cache if there's space */
        if (server->gop_cache[channel].frame_count < MAX_GOP_FRAMES
            && frame_size <= MAX_FRAME_SIZE) {
            gop_frame_t* gop_frame = &server->gop_cache[channel]
                                          .frames[server->gop_cache[channel].frame_count];

            /* Copy frame data */
            memcpy(gop_frame->data, frame_data, frame_size);
            gop_frame->size = frame_size;
            gop_frame->timestamp = *timestamp;
            gop_frame->is_idr = is_idr_frame;

            server->gop_cache[channel].frame_count++;
            server->gop_cache[channel].total_size += frame_size;

            /* Mark GOP as valid immediately when we have an IDR frame */
            if (is_idr_frame) {
                server->gop_cache[channel].valid = true;
            }
        }

        pthread_mutex_unlock(&server->gop_cache[channel].mutex);
    }

    /* For single-pack frames, check if clients need IDR and analyze NAL unit type */
    if (nal_count == 1) {
        /* Check if any client needs IDR frames */
        bool clients_need_idr = false;
        for (int i = 0; i < server->client_count; i++) {
            rtsp_client_t* client = &server->clients[i];
            if (client->state == RTSP_CLIENT_STATE_PLAYING && client->needs_idr) {
                clients_need_idr = true;
                break;
            }
        }

        if (clients_need_idr) {
            /* Analyze the single NAL unit to see if it's important for H.265 */
            extern struct chn_conf chn[FS_CHN_NUM];
            bool is_hevc_local = is_hevc;

            /* Extract NAL unit data (skip start codes) */
            const uint8_t* nal_data = (const uint8_t*) data;
            size_t nal_size = frame_size;

            /* Skip start codes */
            if (frame_size >= 4 && nal_data[0] == 0x00 && nal_data[1] == 0x00 && nal_data[2] == 0x00 && nal_data[3] == 0x01) {
                nal_data += 4;
                nal_size -= 4;
            } else if (frame_size >= 3 && nal_data[0] == 0x00 && nal_data[1] == 0x00 && nal_data[2] == 0x01) {
                nal_data += 3;
                nal_size -= 3;
            }

            bool is_important_nal = false;
            if (is_hevc_local && nal_size >= 2) {
                /* H.265 NAL unit analysis */
                uint8_t nal_type = (nal_data[0] >> 1) & 0x3F;
                /* VPS (32), SPS (33), PPS (34), IDR frames (19-21) */
                is_important_nal = (nal_type >= 19 && nal_type <= 21) || (nal_type >= 32 && nal_type <= 34);
            } else if (!is_hevc_local && nal_size >= 1) {
                /* H.264 NAL unit analysis */
                uint8_t nal_type = nal_data[0] & 0x1F;
                /* SPS (7), PPS (8), IDR (5) */
                is_important_nal = (nal_type == 5) || (nal_type == 7) || (nal_type == 8);
            }

            if (!is_important_nal) {
                // IMP_LOG_INFO(TAG,
                //              "Skipping single-pack frame (size=%u) - waiting for IDR frame",
                //              frame_size);
                return 0; /* Skip this frame and wait for important NAL units */
            }
        }
    }

    /* Send NAL units to clients */
    for (int i = 0; i < MAX_RTSP_CLIENTS; i++) {
        rtsp_client_t* client = &server->clients[i];

        if (client->active && client->state == RTSP_CLIENT_STATE_PLAYING
            && client->video_channel == channel) {
            /* Frame-to-client logging disabled for performance */
            /* Clear IDR flag if this frame contains IDR or parameter sets */
            bool should_clear_idr = false;

            if (nal_count > 1) {
                /* Multi-pack frame - likely contains parameter sets and IDR */
                should_clear_idr = true;
            } else if (nal_count == 1) {
                /* Single-pack frame - check if it's an IDR frame */
                extern struct chn_conf chn[FS_CHN_NUM];
                hal_enc_attr_t a;
                bool is_hevc_client = false;
                if (client->video_channel < FS_CHN_NUM && hal_enc_get_attr(client->video_channel, &a) == 0) {
                    is_hevc_client = (a.payload == HAL_PT_H265);
                }

                /* Extract NAL unit data (skip start codes) */
                const uint8_t* nal_data = (const uint8_t*) data;
                size_t nal_size = frame_size;

                /* Skip start codes */
                if (frame_size >= 4 && nal_data[0] == 0x00 && nal_data[1] == 0x00 && nal_data[2] == 0x00 && nal_data[3] == 0x01) {
                    nal_data += 4;
                    nal_size -= 4;
                } else if (frame_size >= 3 && nal_data[0] == 0x00 && nal_data[1] == 0x00 && nal_data[2] == 0x01) {
                    nal_data += 3;
                    nal_size -= 3;
                }

                if (is_hevc_client && nal_size >= 2) {
                    /* H.265 IDR frame detection */
                    uint8_t nal_type = (nal_data[0] >> 1) & 0x3F;
                    /* IDR frames: IDR_W_RADL (19), IDR_N_LP (20), CRA_NUT (21) */
                    if (nal_type >= 19 && nal_type <= 21) {
                        should_clear_idr = true;
                    }
                } else if (!is_hevc_client && nal_size >= 1) {
                    /* H.264 IDR frame detection */
                    uint8_t nal_type = nal_data[0] & 0x1F;
                    if (nal_type == 5) { /* IDR frame */
                        should_clear_idr = true;
                    }
                }
            }

            if (should_clear_idr) {
                client->needs_idr = false;
            }

            /* Use the calculated real-time timestamp */
            uint32_t rtp_timestamp = current_timestamp;

            bool frame_sent_successfully = true;

            /* Send each NAL unit with proper start codes */
            for (int nal = 0; nal < nal_count; nal++) {
                bool is_last_nal = (nal == nal_count - 1);
                const uint8_t* nal_data = data + nal_offsets[nal];
                size_t nal_size = nal_sizes[nal];

                /* Validate NAL unit size */
                if (nal_size == 0 || nal_offsets[nal] >= frame_size) {
                    IMP_LOG_ERR(TAG,
                                "WARNING: Invalid NAL unit %d: offset=%zu, size=%zu, frame_size=%u",
                                nal,
                                nal_offsets[nal],
                                nal_size,
                                frame_size);
                    continue;
                }

                /* Extract NAL type using SDK approach */
                extern struct chn_conf chn[FS_CHN_NUM];
                hal_enc_attr_t a2;
                bool is_hevc_client2 = false;
                if (client->video_channel < FS_CHN_NUM && hal_enc_get_attr(client->video_channel, &a2) == 0) {
                    is_hevc_client2 = (a2.payload == HAL_PT_H265);
                }

                uint8_t nal_type = 0;
                bool is_key_frame = false;
                bool is_parameter_set = false;

                if (is_hevc_client2) {
                    /* H.265 NAL unit header is 2 bytes */
                    if (nal_size >= 2) {
                        nal_type = (nal_data[0] >> 1) & 0x3F;
                        /* H.265 key frames: IDR_W_RADL (19), IDR_N_LP (20), CRA_NUT (21) */
                        is_key_frame = (nal_type >= 19 && nal_type <= 21);
                        /* H.265 parameter sets: VPS (32), SPS (33), PPS (34) */
                        is_parameter_set = (nal_type >= 32 && nal_type <= 34);
                    }
                } else {
                    /* H.264 NAL unit header is 1 byte */
                    if (nal_size >= 1) {
                        nal_type = nal_data[0] & 0x1F;
                        /* H.264 key frames: IDR (5) */
                        is_key_frame = (nal_type == 5);
                        /* H.264 parameter sets: SPS (7), PPS (8) */
                        is_parameter_set = (nal_type == 7 || nal_type == 8);
                    }
                }

                /* Log important NAL units to track parameter sets */
                // if (is_key_frame || is_parameter_set) {
                //     IMP_LOG_INFO(TAG,
                //                 "Sending %s NAL type %d (%s), size %zu to client",
                //                 enc_type == IMP_ENC_TYPE_HEVC ? "H.265" : "H.264",
                //                 nal_type,
                //                 is_parameter_set ? (nal_type == 7 ? "SPS" : nal_type == 8 ? "PPS" : "PARAM") : "IDR",
                //                 nal_size);
                // }

                /* Debug large NAL units before sending */
                // if (nal_size > 1000) {
                //     IMP_LOG_INFO(TAG, "Sending large NAL unit: type=%d, size=%zu", nal_type, nal_size);
                // }

                /* Check if client is still active before sending */
                if (!client->active) {
                    IMP_LOG_DBG(TAG, "Client %d became inactive, skipping RTP packet", i);
                    frame_sent_successfully = false;
                    break;
                }

                /* Send raw NAL unit WITHOUT start code (RFC 3984/7798 requirement) */
                if (send_rtp_packet(client,
                                    nal_data,
                                    nal_size,
                                    rtp_timestamp,
                                    is_last_nal)
                    <= 0) {
                    IMP_LOG_ERR(TAG, "send_rtp_packet failed for client %d, marking inactive", i);
                    /* Mark client as inactive instead of immediate cleanup */
                    client->active = false;
                    frame_sent_successfully = false;
                    break;
                }
            }

            if (frame_sent_successfully) {
                clients_sent++;
                server->total_bytes_sent += frame_size;
                server->total_packets_sent += nal_count;
            }
        }
    }

    /* Debug: Track client frame delivery rate */
    static unsigned long last_client_report_time = 0;
    static int client_frames_sent = 0;
    client_frames_sent += clients_sent;

    if (last_client_report_time == 0) {
        last_client_report_time = current_time;
    } else if (current_time - last_client_report_time >= 5000000) { /* 5 seconds */
        double elapsed_seconds = (current_time - last_client_report_time) / 1000000.0;
        double client_fps = client_frames_sent / elapsed_seconds;
        IMP_LOG_INFO(TAG, "Client frame delivery rate: %.2f fps (%d frames to clients in %.2f seconds)",
                    client_fps, client_frames_sent, elapsed_seconds);
        last_client_report_time = current_time;
        client_frames_sent = 0;
    }

    return clients_sent;
}

void rtsp_server_set_default_config(rtsp_server_config_t* config)
{
    if (!config)
        return;

    memset(config, 0, sizeof(rtsp_server_config_t));

    config->port = DEFAULT_RTSP_PORT;

    /* Authentication defaults */
    config->auth.enabled = false;
    config->auth.localhost_bypass = true;
    strcpy(config->auth.username, "admin");
    strcpy(config->auth.password, "admin");

    strncpy(config->server_name, "Thingino RTSP Server", sizeof(config->server_name) - 1);
    config->max_clients = MAX_RTSP_CLIENTS;
    config->session_timeout = 60; /* 60 seconds */

    /* RTSPS defaults */
    config->tls_enabled = false;
    config->tls_port = 322;  /* Standard RTSPS port */
    config->cert_file[0] = '\0';
    config->key_file[0] = '\0';
    config->tls_verify_client = false;
}

/* Server thread implementation */
static void* server_thread_func(void* arg)
{
    rtsp_server_t* server = (rtsp_server_t*) arg;
    fd_set read_fds;
    struct timeval timeout;
    int max_fd;

    // IMP_LOG_DBG(TAG, "RTSP server thread started");

    while (!server->should_stop) {
        FD_ZERO(&read_fds);
        max_fd = 0;

        /* Add listen socket to fd set */
        if (server->listen_socket >= 0) {
            FD_SET(server->listen_socket, &read_fds);
            max_fd = server->listen_socket;
        }

        /* Add TLS listen socket to fd set */
        if (server->config.tls_enabled && server->tls_listen_socket >= 0) {
            FD_SET(server->tls_listen_socket, &read_fds);
            if (server->tls_listen_socket > max_fd) {
                max_fd = server->tls_listen_socket;
            }
        }

        /* Add client sockets to fd set */
        for (int i = 0; i < MAX_RTSP_CLIENTS; i++) {
            if (server->clients[i].active && server->clients[i].socket_fd >= 0) {
                FD_SET(server->clients[i].socket_fd, &read_fds);
                if (server->clients[i].socket_fd > max_fd) {
                    max_fd = server->clients[i].socket_fd;
                }
            }
        }

        /* Set timeout for select */
        timeout.tv_sec = 1;
        timeout.tv_usec = 0;

        int activity = select(max_fd + 1, &read_fds, NULL, NULL, &timeout);

        if (activity < 0) {
            if (errno != EINTR) {
                IMP_LOG_ERR(TAG, "Select error: %s (errno=%d)", strerror(errno), errno);
                IMP_LOG_ERR(TAG, "Select context: max_fd=%d, listen_socket=%d, tls_listen_socket=%d",
                           max_fd, server->listen_socket, server->tls_listen_socket);

                /* Log active client sockets */
                for (int i = 0; i < MAX_RTSP_CLIENTS; i++) {
                    if (server->clients[i].active && server->clients[i].socket_fd >= 0) {
                        IMP_LOG_ERR(TAG, "Active client %d: socket_fd=%d, use_tls=%s, state=%d",
                                   i, server->clients[i].socket_fd,
                                   server->clients[i].use_tls ? "yes" : "no",
                                   server->clients[i].state);
                    }
                }

                /* Check if any file descriptors are invalid */
                if (server->listen_socket >= 0) {
                    int flags = fcntl(server->listen_socket, F_GETFL);
                    if (flags == -1) {
                        IMP_LOG_ERR(TAG, "Listen socket fd=%d is invalid: %s", server->listen_socket, strerror(errno));
                    }
                }

                if (server->config.tls_enabled && server->tls_listen_socket >= 0) {
                    int flags = fcntl(server->tls_listen_socket, F_GETFL);
                    if (flags == -1) {
                        IMP_LOG_ERR(TAG, "TLS listen socket fd=%d is invalid: %s", server->tls_listen_socket, strerror(errno));
                    }
                }

                for (int i = 0; i < MAX_RTSP_CLIENTS; i++) {
                    if (server->clients[i].active && server->clients[i].socket_fd >= 0) {
                        int flags = fcntl(server->clients[i].socket_fd, F_GETFL);
                        if (flags == -1) {
                            IMP_LOG_ERR(TAG, "Client %d socket fd=%d is invalid: %s",
                                       i, server->clients[i].socket_fd, strerror(errno));
                            /* Mark client as inactive to prevent further issues */
                            server->clients[i].active = false;
                        }
                    }
                }
            }
            continue;
        }

        if (activity == 0) {
            /* Timeout - check for inactive clients */
            unsigned long now_us = (unsigned long)get_monotonic_time_us();

            for (int i = 0; i < MAX_RTSP_CLIENTS; i++) {
                if (server->clients[i].active) {
                    /* Check if client has been inactive for too long */
                    unsigned long inactive_time_us = now_us - (unsigned long)server->clients[i].last_activity_us;
                    if (inactive_time_us > (unsigned long)server->config.session_timeout * 1000000UL) {
                        IMP_LOG_INFO(TAG, "Client %d timed out", i);
                        cleanup_client(&server->clients[i]);
                        server->client_count--;
                    }
                }
            }
            continue;
        }

        IMP_LOG_DBG(TAG, "Checking for new connections");
        /* Check for new connections */
        if (server->listen_socket >= 0 && FD_ISSET(server->listen_socket, &read_fds)) {
            IMP_LOG_DBG(TAG, "New connection detected");
            struct sockaddr_in client_addr;
            socklen_t client_len = sizeof(client_addr);
            int client_fd = accept(server->listen_socket,
                                   (struct sockaddr*) &client_addr,
                                   &client_len);

            if (client_fd < 0) {
                IMP_LOG_ERR(TAG, "Accept failed: %s", strerror(errno));
            } else {
                IMP_LOG_INFO(TAG,
                             "New connection from %s:%d",
                             inet_ntoa(client_addr.sin_addr),
                             ntohs(client_addr.sin_port));
                IMP_LOG_INFO(TAG, "*** DEBUG: About to process new connection ***");

                /* Check if we have room for a new client */
                if (server->client_count >= server->config.max_clients) {
                    IMP_LOG_ERR(TAG, "Too many clients, rejecting connection");
                    close(client_fd);
                } else {
                    IMP_LOG_INFO(TAG, "Accepting connection, client count: %d", server->client_count);
                    /* Find a free client slot */
                    rtsp_client_t* client = find_free_client_slot(server);
                    if (client) {
                        /* Initialize client */
                        client->socket_fd = client_fd;
                        client->rtp_socket_fd = -1;
                        client->active = true;
                        client->state = RTSP_CLIENT_STATE_INIT;
                        client->transport_mode = RTSP_TRANSPORT_UDP;
                        client->needs_idr = false;
                        client->idr_wait_start_us = 0;
                        memcpy(&client->client_addr, &client_addr, sizeof(client_addr));
                        client->last_activity_us = get_monotonic_time_us();
                        generate_session_id(client->session_id, sizeof(client->session_id));

                        /* Initialize TLS context */
                        client->use_tls = false;
                        client->ssl_context = NULL;
                        client->ssl_config = NULL;
                        client->entropy_context = NULL;
                        client->ctr_drbg_context = NULL;

                        /* Initialize RTP parameters */
                        client->rtp_seq = (uint16_t) rand();
                        client->rtp_timestamp = (uint32_t) rand();
                        client->rtp_ssrc = (uint32_t) rand();

                        server->client_count++;
                        server->total_connections++;
                        IMP_LOG_INFO(TAG, "*** DEBUG: Client initialized, waiting for data ***");
                    } else {
                        /* This shouldn't happen, but just in case */
                        IMP_LOG_ERR(TAG, "No free client slots, rejecting connection");
                        close(client_fd);
                    }
                }
            }
        }

        IMP_LOG_DBG(TAG, "Checking for TLS connections");
        /* Check for new TLS connections */
        if (server->config.tls_enabled && server->tls_listen_socket >= 0 &&
            FD_ISSET(server->tls_listen_socket, &read_fds)) {
                IMP_LOG_DBG(TAG, "TLS connection detected");
            struct sockaddr_in client_addr;
            socklen_t client_len = sizeof(client_addr);
            int client_fd = accept(server->tls_listen_socket,
                                   (struct sockaddr*) &client_addr,
                                   &client_len);

            if (client_fd < 0) {
                IMP_LOG_ERR(TAG, "TLS accept failed: %s", strerror(errno));
            } else {
                IMP_LOG_INFO(TAG,
                             "New TLS connection from %s:%d",
                             inet_ntoa(client_addr.sin_addr),
                             ntohs(client_addr.sin_port));
                IMP_LOG_INFO(TAG, "*** DEBUG: About to process new TLS connection ***");

                /* Check if we have room for a new client */
                if (server->client_count >= server->config.max_clients) {
                    IMP_LOG_ERR(TAG, "Too many clients, rejecting TLS connection");
                    close(client_fd);
                } else {
                    IMP_LOG_INFO(TAG, "Accepting TLS connection, client count: %d", server->client_count);
                    /* Find a free client slot */
                    rtsp_client_t* client = find_free_client_slot(server);
                    if (client) {
                        /* Initialize client */
                        client->socket_fd = client_fd;
                        client->rtp_socket_fd = -1;
                        client->active = true;
                        client->state = RTSP_CLIENT_STATE_INIT;
                        client->transport_mode = RTSP_TRANSPORT_UDP;
                        client->needs_idr = false;
                        client->idr_wait_start_us = 0;
                        memcpy(&client->client_addr, &client_addr, sizeof(client_addr));
                        client->last_activity_us = get_monotonic_time_us();
                        generate_session_id(client->session_id, sizeof(client->session_id));

                        /* Initialize TLS context */
                        client->use_tls = false;
                        client->ssl_context = NULL;
                        client->ssl_config = NULL;
                        client->entropy_context = NULL;
                        client->ctr_drbg_context = NULL;

                        /* Perform TLS handshake */
                        if (rtsp_client_tls_accept(server, client, client_fd) < 0) {
                            IMP_LOG_ERR(TAG, "TLS handshake failed for client");
                            close(client_fd);
                        } else {
                            /* Initialize RTP parameters */
                            client->rtp_seq = (uint16_t) rand();
                            client->rtp_timestamp = (uint32_t) rand();
                            client->rtp_ssrc = (uint32_t) rand();

                            server->client_count++;
                            server->total_connections++;
                            IMP_LOG_INFO(TAG, "TLS client connected successfully");
                            IMP_LOG_INFO(TAG, "*** DEBUG: TLS client initialized, waiting for data ***");
                        }
                    } else {
                        /* This shouldn't happen, but just in case */
                        IMP_LOG_ERR(TAG, "No free client slots, rejecting TLS connection");
                        close(client_fd);
                    }
                }
            }
        }

        /* Check for client activity */
        for (int i = 0; i < MAX_RTSP_CLIENTS; i++) {
            if (server->clients[i].active && server->clients[i].socket_fd >= 0
                && FD_ISSET(server->clients[i].socket_fd, &read_fds)) {
                IMP_LOG_INFO(TAG, "*** CLIENT %d SOCKET READY FOR READING ***", i);

                /* Check if this client has recent RTP send failures indicating broken connection */
                bool connection_broken = false;
                /* We can detect this by checking socket state, but for now we'll rely on the recv() result */

                /* For TCP transport clients, temporarily mark as not playing to prevent RTP interference */
                bool was_playing = (server->clients[i].state == RTSP_CLIENT_STATE_PLAYING);
                rtsp_client_state_t original_state = server->clients[i].state;
                if (server->clients[i].transport_mode == RTSP_TRANSPORT_TCP && was_playing) {
                    IMP_LOG_INFO(TAG, "Temporarily pausing RTP for TCP client %d to handle control message", i);
                    server->clients[i].state = RTSP_CLIENT_STATE_READY;

                    /* State change to READY immediately stops RTP sending */
                    IMP_LOG_INFO(TAG, "RTP paused for TCP client %d", i);
                }

                /* Handle client request */
                int result = handle_client_connection(server, i);

                /* Restore playing state if client is still active and request was successful */
                if (server->clients[i].active && result >= 0 && was_playing &&
                    server->clients[i].state == RTSP_CLIENT_STATE_READY) {
                    server->clients[i].state = original_state;
                    IMP_LOG_INFO(TAG, "Resumed RTP for TCP client %d after handling control message", i);
                }

                if (result < 0) {
                    /* Client disconnected or error */
                    IMP_LOG_DBG(TAG, "Client %d disconnected", i);
                    /* Mark client as inactive first to stop other threads from using it */
                    server->clients[i].active = false;
                    /* Small delay to let other threads finish their operations */
                    usleep(10000); /* 10ms delay */
                    cleanup_client(&server->clients[i]);
                    server->client_count--;
                }
            }
        }
    }

    // IMP_LOG_DBG(TAG, "RTSP server thread exiting");
    return NULL;
}

/* RTP thread implementation */
static void* rtp_thread_func(void* arg)
{
    rtsp_server_t* server = (rtsp_server_t*) arg;

    if (!server) {
        IMP_LOG_ERR(TAG, "RTP thread: Invalid server pointer");
        return NULL;
    }

    // IMP_LOG_DBG(TAG, "RTP thread started");

    /* Give system time to stabilize before starting frame processing */
    sleep(2);
    // IMP_LOG_DBG(TAG, "RTP thread: Starting frame processing loop");

    while (!server->should_stop) {
        /* Use 16ms polling (60fps) - optimized for 30fps streaming with headroom */
        usleep(16000); /* 16ms - allows up to 60fps with good efficiency */

        /* Check if we have any active RTSP clients */
        int active_rtsp_clients = 0;
        for (int i = 0; i < MAX_RTSP_CLIENTS; i++) {
            if (server->clients[i].active && server->clients[i].state == RTSP_CLIENT_STATE_PLAYING) {
                active_rtsp_clients++;
            }
        }

        /* Also check for active RTMP clients */
        int active_rtmp_clients = 0;
#ifdef ENABLE_RTMP_CLIENT
        extern int rtmp_client_get_active_connection_count(void);
        active_rtmp_clients = rtmp_client_get_active_connection_count();
#endif

        /* Call frame callback if we have any active clients (RTSP or RTMP) */
        if (active_rtsp_clients > 0 || active_rtmp_clients > 0) {
            /* Call frame callback - let SDK decide when frames are ready */
            if (server->frame_callback) {
                server->frame_callback(server, server->user_data);
            }
        }
    }

    // IMP_LOG_DBG(TAG, "RTP thread exiting");
    return NULL;
}

/* Handle client connection */
static int handle_client_connection(rtsp_server_t* server, int client_index)
{
    rtsp_client_t* client = &server->clients[client_index];
    char buffer[MAX_RTSP_BUFFER_SIZE];
    int bytes_received;

    IMP_LOG_INFO(TAG, "handle_client_connection called for client %d", client_index);

    /* Receive data from client (TLS-aware) */
    bytes_received = rtsp_client_tls_read(client, buffer, sizeof(buffer) - 1);
    IMP_LOG_INFO(TAG, "recv() returned %d bytes", bytes_received);
    if (bytes_received <= 0) {
        if (bytes_received == 0) {
            /* Client closed connection */
            IMP_LOG_INFO(TAG, "Client closed connection (recv returned 0)");
            return -1;
        } else {
            /* Error */
            IMP_LOG_ERR(TAG, "recv() failed: %s", strerror(errno));
            return -1;
        }
    }

    /* Log first few bytes to detect non-RTSP data */
    if (bytes_received > 0) {
        IMP_LOG_INFO(TAG, "Raw data received - first 4 bytes: 0x%02x 0x%02x 0x%02x 0x%02x",
                    (unsigned char)buffer[0],
                    bytes_received > 1 ? (unsigned char)buffer[1] : 0,
                    bytes_received > 2 ? (unsigned char)buffer[2] : 0,
                    bytes_received > 3 ? (unsigned char)buffer[3] : 0);

        /* Special handling for TCP transport: look for TEARDOWN in the data stream */
        if (client->transport_mode == RTSP_TRANSPORT_TCP && bytes_received >= 8) {
            /* Search for "TEARDOWN" pattern in the received data */
            for (int j = 0; j <= bytes_received - 8; j++) {
                if (strncmp(&buffer[j], "TEARDOWN", 8) == 0) {
                    IMP_LOG_INFO(TAG, "*** TEARDOWN DETECTED in TCP stream at offset %d ***", j);
                    /* If TEARDOWN is not at the beginning, we have interleaved data */
                    if (j > 0) {
                        IMP_LOG_WARN(TAG, "TEARDOWN found with %d bytes of preceding data (likely RTP)", j);
                        /* Move TEARDOWN to beginning of buffer */
                        memmove(buffer, &buffer[j], bytes_received - j);
                        bytes_received = bytes_received - j;
                        buffer[bytes_received] = '\0';
                        IMP_LOG_INFO(TAG, "Extracted TEARDOWN from interleaved stream: %s", buffer);
                    }
                    break;
                }
            }
        }
    }

    /* Check if this is an RTP packet (TCP interleaved mode) */
    if (bytes_received >= 1 && buffer[0] == '$') {
        if (bytes_received >= 4) {
            /* This is an RTP packet in TCP interleaved format */
            unsigned char channel = buffer[1];
            unsigned short length = (buffer[2] << 8) | buffer[3];

        //     IMP_LOG_INFO(TAG,
        //                  "Received RTP packet - Channel: %d, Length: %d bytes (received %d bytes)",
        //                  channel,
        //                  length,
        //                  bytes_received);
        // } else {
        //     IMP_LOG_INFO(TAG, "Received partial RTP header (%d bytes)", bytes_received);
        }

        /* For now, just ignore RTP packets - we're not implementing RTP streaming yet */
        /* In a full implementation, we would process the RTP data here */
        return 0; /* Continue processing */
    }

    buffer[bytes_received] = '\0';
    client->last_activity_us = get_monotonic_time_us();

    IMP_LOG_INFO(TAG, "Received RTSP request (%d bytes): %s", bytes_received,
                 (bytes_received > 0 && buffer[0] != '\0') ? buffer : "[empty or invalid]");

    /* Parse RTSP request first to get CSeq for proper response handling */
    rtsp_method_t method;
    char url[MAX_RTSP_URL_LEN];
    int cseq = 0; /* Initialize to 0 in case parsing fails */

    if (parse_rtsp_request(buffer, &method, url, &cseq) < 0) {
        IMP_LOG_ERR(TAG, "Failed to parse RTSP request, closing connection");
        /* For malformed requests (like responses being sent as requests), close the connection */
        return -1;
    }

    client->cseq = cseq;

    /* Get client information for authentication */
    client_info_t client_info;
    if (auth_get_client_info(client->socket_fd, &client_info) < 0) {
        /* If we can't get client info but this is a TEARDOWN, allow it to proceed */
        if (method == RTSP_METHOD_TEARDOWN) {
            IMP_LOG_WARN(TAG, "Failed to get client info for TEARDOWN (broken connection), proceeding with cleanup");
            /* Set dummy client info for TEARDOWN processing */
            memset(&client_info, 0, sizeof(client_info));
            strcpy(client_info.ip_string, "unknown");
        } else {
            IMP_LOG_ERR(TAG, "Failed to get client information");
            return -1;
        }
    }

    /* Check authentication (skip for TEARDOWN on broken connections) */
    auth_result_t auth_result = AUTH_RESULT_SUCCESS;
    bool skip_auth = (method == RTSP_METHOD_TEARDOWN && strcmp(client_info.ip_string, "unknown") == 0);

    if (!skip_auth) {
        IMP_LOG_DBG(TAG, "Auth config: enabled=%s, username='%s', password='%s'",
                   server->config.auth.enabled ? "true" : "false",
                   server->config.auth.username, server->config.auth.password);
        auth_result = auth_check_rtsp_request(buffer, &server->config.auth, &client_info);
    } else {
        IMP_LOG_INFO(TAG, "Skipping authentication for TEARDOWN on broken connection");
    }

    if (auth_result == AUTH_RESULT_REQUIRED) {
        /* Send 401 Unauthorized with WWW-Authenticate header */
        IMP_LOG_INFO(TAG, "401 - Authentication required for RTSP client %s", client_info.ip_string);
        return send_rtsp_response(client, RTSP_STATUS_UNAUTHORIZED, "Unauthorized",
                                 "WWW-Authenticate: Basic realm=\"Thingino RTSP Server\"\r\n", NULL);
    } else if (auth_result == AUTH_RESULT_INVALID) {
        /* Send 401 Unauthorized for invalid credentials */
        IMP_LOG_WARN(TAG, "401 - Invalid credentials from RTSP client %s", client_info.ip_string);
        return send_rtsp_response(client, RTSP_STATUS_UNAUTHORIZED, "Unauthorized",
                                 "WWW-Authenticate: Basic realm=\"Thingino RTSP Server\"\r\n", NULL);
    } else if (auth_result == AUTH_RESULT_ERROR) {
        /* Authentication system error */
        IMP_LOG_ERR(TAG, "Authentication system error for RTSP client %s", client_info.ip_string);
        return send_rtsp_response(client, RTSP_STATUS_INTERNAL_ERROR, "Internal Server Error", NULL, NULL);
    }

    /* Authentication successful or not required */
    if (auth_is_required(&server->config.auth, &client_info)) {
        IMP_LOG_INFO(TAG, "Authenticated RTSP request from %s", client_info.ip_string);
    } else {
        if (!server->config.auth.enabled) {
            IMP_LOG_INFO(TAG, "RTSP authentication disabled in configuration");
        } else if (server->config.auth.localhost_bypass && client_info.is_localhost) {
            IMP_LOG_DBG(TAG, "Localhost bypass for RTSP client %s", client_info.ip_string);
        } else {
            IMP_LOG_DBG(TAG, "No authentication required for RTSP client %s", client_info.ip_string);
        }
    }

    /* Handle different RTSP methods */
    // IMP_LOG_INFO(
    //     TAG,
    //     "Handling RTSP method: %d (1=OPTIONS, 2=DESCRIBE, 3=SETUP, 4=PLAY, 5=PAUSE, 6=TEARDOWN)",
    //     method);

    switch (method) {
    case RTSP_METHOD_OPTIONS:
        return handle_options_request(server, client);

    case RTSP_METHOD_DESCRIBE:
        return handle_describe_request(server, client, url);

    case RTSP_METHOD_SETUP: {
        /* Extract transport header */
        char* transport_line = strstr(buffer, "Transport:");
        char transport[256] = "";
        if (transport_line) {
            sscanf(transport_line, "Transport: %255[^\r\n]", transport);
        }
        IMP_LOG_INFO(TAG, "*** SETUP REQUEST - Transport: %s ***", transport);
        return handle_setup_request(server, client, url, transport);
    }

    case RTSP_METHOD_PLAY:
        return handle_play_request(server, client);

    case RTSP_METHOD_PAUSE:
        return handle_pause_request(server, client);

    case RTSP_METHOD_TEARDOWN:
        IMP_LOG_INFO(TAG, "*** TEARDOWN REQUEST RECEIVED - Processing client termination request ***");
        return handle_teardown_request(server, client);

    default:
        IMP_LOG_ERR(TAG, "Unsupported RTSP method: %d from %s. Request was: %s", method, client_info.ip_string, buffer);
        return send_rtsp_response(client,
                                  RTSP_STATUS_METHOD_NOT_ALLOWED,
                                  "Method Not Allowed",
                                  NULL,
                                  NULL);
    }
}

/* Parse RTSP request */
static int parse_rtsp_request(const char* request, rtsp_method_t* method, char* url, int* cseq)
{
    char method_str[32];

    /* Parse request line */
    if (sscanf(request, "%31s %255s", method_str, url) != 2) {
        IMP_LOG_ERR(TAG, "Failed to parse RTSP request line: %s", request);
        return -1;
    }

    /* Check if this is actually an RTSP response being sent as a request */
    if (strncmp(method_str, "RTSP/", 5) == 0) {
        IMP_LOG_WARN(TAG, "Client sent RTSP response instead of request: %s", method_str);
        return -1;
    }

    /* Determine method */
    if (strcmp(method_str, "OPTIONS") == 0) {
        *method = RTSP_METHOD_OPTIONS;
    } else if (strcmp(method_str, "DESCRIBE") == 0) {
        *method = RTSP_METHOD_DESCRIBE;
    } else if (strcmp(method_str, "SETUP") == 0) {
        *method = RTSP_METHOD_SETUP;
    } else if (strcmp(method_str, "PLAY") == 0) {
        *method = RTSP_METHOD_PLAY;
    } else if (strcmp(method_str, "PAUSE") == 0) {
        *method = RTSP_METHOD_PAUSE;
    } else if (strcmp(method_str, "TEARDOWN") == 0) {
        *method = RTSP_METHOD_TEARDOWN;
        IMP_LOG_INFO(TAG, "*** TEARDOWN METHOD DETECTED IN REQUEST PARSING ***");
    } else {
        IMP_LOG_ERR(TAG, "Unknown RTSP method: '%s' in request: %s", method_str, request);
        *method = RTSP_METHOD_UNKNOWN;
    }

    /* Extract CSeq */
    const char* cseq_line = strstr(request, "CSeq:");
    if (cseq_line) {
        if (sscanf(cseq_line, "CSeq: %d", cseq) != 1) {
            IMP_LOG_ERR(TAG, "Failed to parse CSeq from line: '%.50s'", cseq_line);
            *cseq = 0;
        }
    } else {
        IMP_LOG_ERR(TAG, "No CSeq header found in RTSP request");
        *cseq = 0;
    }

    IMP_LOG_DBG(TAG, "Parsed RTSP: method='%s', url='%s', cseq=%d", method_str, url, *cseq);

    return 0;
}

/* Send RTSP response */
static int send_rtsp_response(rtsp_client_t* client,
                              int status_code,
                              const char* status_text,
                              const char* headers,
                              const char* body)
{
    char response[MAX_RTSP_BUFFER_SIZE];
    int len;

    /* Log CSeq value before building response */
    IMP_LOG_INFO(TAG, "Building RTSP response with CSeq: %d (status: %d %s)", client->cseq, status_code, status_text);

    /* Build response */
    len = snprintf(response,
                   sizeof(response),
                   "RTSP/1.0 %d %s\r\n"
                   "CSeq: %d\r\n"
                   "Server: Thingino RTSP Server\r\n",
                   status_code,
                   status_text,
                   client->cseq);

    /* Add session header if we have a session ID */
    if (client->session_id[0] != '\0') {
        len += snprintf(response + len,
                        sizeof(response) - len,
                        "Session: %s\r\n",
                        client->session_id);
    }

    /* Add custom headers */
    if (headers) {
        len += snprintf(response + len, sizeof(response) - len, "%s", headers);
    }

    /* Add content length and type if we have a body */
    if (body) {
        len += snprintf(response + len,
                        sizeof(response) - len,
                        "Content-Type: application/sdp\r\n"
                        "Content-Length: %d\r\n\r\n%s",
                        (int) strlen(body),
                        body);
    } else {
        len += snprintf(response + len, sizeof(response) - len, "\r\n");
    }

    /* Send response (TLS-aware) */
    int bytes_sent = rtsp_client_tls_write(client, response, len);
    if (bytes_sent < 0) {
        IMP_LOG_ERR(TAG, "Failed to send RTSP response: %s", strerror(errno));
        /* Don't mark client as inactive here - let the main loop handle cleanup */
        return -1;
    }

    IMP_LOG_INFO(TAG, "Sent RTSP response (%d bytes):\n%s", bytes_sent, response);
    return 0;
}

static int handle_options_request(rtsp_server_t* server, rtsp_client_t* client)
{
    const char* headers = "Public: OPTIONS, DESCRIBE, SETUP, PLAY, PAUSE, TEARDOWN\r\n";

    IMP_LOG_INFO(TAG, "Handling OPTIONS request with CSeq: %d", client->cseq);
    return send_rtsp_response(client, RTSP_STATUS_OK, "OK", headers, NULL);
}

static int handle_describe_request(rtsp_server_t* server,
                                   rtsp_client_t* client,
                                   const char* url)
{
    char sdp[MAX_SDP_SIZE];
    char stream_name[MAX_RTSP_URL_LEN];

    /* Check if client supports ONVIF backchannel */
    if (strstr(client->request_buffer, "Require: www.onvif.org/ver20/backchannel") != NULL) {
        client->supports_backchannel = true;
        IMP_LOG_INFO(TAG, "Client supports ONVIF audio backchannel");
    } else {
        client->supports_backchannel = false;
    }

    // IMP_LOG_INFO(TAG, "Handling DESCRIBE request for URL: %s", url);

    /* Extract stream name from URL using shared parser */
    if (extract_stream_name_from_url(server, url, stream_name, sizeof(stream_name)) < 0) {
        return send_rtsp_response(client, RTSP_STATUS_NOT_FOUND, "Not Found", NULL, NULL);
    }

    /* Find stream by name */
    int stream_index = -1;
    for (int i = 0; i < server->stream_count; i++) {
        if (strcmp(stream_name, server->streams[i].stream_name) == 0) {
            stream_index = i;
            break;
        }
    }

    if (stream_index < 0) {
        IMP_LOG_ERR(TAG, "Stream '%s' not found. Available streams:", stream_name);
        for (int i = 0; i < server->stream_count; i++) {
            IMP_LOG_INFO(TAG,
                         "  - %s (channel %d)",
                         server->streams[i].stream_name,
                         server->streams[i].channel);
        }
        return send_rtsp_response(client, RTSP_STATUS_NOT_FOUND, "Not Found", NULL, NULL);
    }

    /* Generate SDP */
    if (generate_sdp(server, stream_name, sdp, sizeof(sdp), client) < 0) {
        return send_rtsp_response(client,
                                  RTSP_STATUS_INTERNAL_ERROR,
                                  "Internal Server Error",
                                  NULL,
                                  NULL);
    }

    return send_rtsp_response(client, RTSP_STATUS_OK, "OK", NULL, sdp);
}

static int handle_setup_request(rtsp_server_t* server,
                                rtsp_client_t* client,
                                const char* url,
                                const char* transport)
{
    char headers[512];
    char stream_name[MAX_RTSP_URL_LEN];

    // IMP_LOG_INFO(TAG, "Handling SETUP request for URL: %s, Transport: %s", url, transport);

    /* Extract stream name from URL using shared parser */
    if (extract_stream_name_from_url(server, url, stream_name, sizeof(stream_name)) < 0) {
        return send_rtsp_response(client, RTSP_STATUS_NOT_FOUND, "Not Found", NULL, NULL);
    }

    /* Remove track suffix if present and extract track ID
     * Standard RTSP/ONVIF convention: track1=video, track2=audio */
    int track_id = 0; /* Default to video track */
    char* track_suffix = NULL;
    bool is_backchannel = false;

    /* Handle ONVIF audio backchannel tracks */
    if ((track_suffix = strstr(stream_name, "/G711_audiobackchannel")) != NULL) {
        track_id = 3; /* G.711 backchannel */
        is_backchannel = true;
        *track_suffix = '\0';
        IMP_LOG_INFO(TAG, "SETUP for G.711 audio backchannel");
    } else if ((track_suffix = strstr(stream_name, "/G726_audiobackchannel")) != NULL) {
        track_id = 4; /* G.726 backchannel */
        is_backchannel = true;
        *track_suffix = '\0';
        IMP_LOG_INFO(TAG, "SETUP for G.726 audio backchannel");
    } else if ((track_suffix = strstr(stream_name, "/audio")) != NULL) {
        track_id = 2; /* Regular audio track */
        *track_suffix = '\0';
    } else if ((track_suffix = strstr(stream_name, "/track1")) != NULL) {
        track_id = 1; /* Video track */
        *track_suffix = '\0';
    } else if ((track_suffix = strstr(stream_name, "/track2")) != NULL) {
        track_id = 2; /* Audio track */
        *track_suffix = '\0';
    } else if ((track_suffix = strstr(stream_name, "/trackID=")) != NULL) {
        /* Legacy trackID=X format for backward compatibility */
        track_id = atoi(track_suffix + 9);
        *track_suffix = '\0';
    }

    // IMP_LOG_INFO(TAG, "SETUP: stream_name='%s', track_id=%d", stream_name, track_id);

    // IMP_LOG_INFO(TAG, "Extracted stream name: '%s'", stream_name);

    /* Find stream by name */
    int stream_index = -1;
    for (int i = 0; i < server->stream_count; i++) {
        if (strcmp(stream_name, server->streams[i].stream_name) == 0) {
            stream_index = i;
            break;
        }
    }

    if (stream_index < 0) {
        IMP_LOG_ERR(TAG, "Stream '%s' not found in SETUP. Available streams:", stream_name);
        for (int i = 0; i < server->stream_count; i++) {
            IMP_LOG_INFO(TAG,
                         "  - %s (channel %d)",
                         server->streams[i].stream_name,
                         server->streams[i].channel);
        }
        return send_rtsp_response(client, RTSP_STATUS_NOT_FOUND, "Not Found", NULL, NULL);
    }

    /* Validate backchannel requests */
    if (is_backchannel) {
        /* Check if client supports ONVIF backchannel */
        if (strstr(client->request_buffer, "Require: www.onvif.org/ver20/backchannel") == NULL) {
            IMP_LOG_ERR(TAG, "Backchannel SETUP request missing required ONVIF header");
            return send_rtsp_response(client, RTSP_STATUS_BAD_REQUEST, "Bad Request", NULL, NULL);
        }
        IMP_LOG_INFO(TAG, "Valid ONVIF backchannel SETUP request");
    }

    /* Store stream info in client - for video tracks (track1 or trackID=0) */
    if (track_id == 1 || track_id == 0) {  /* track1=video or legacy trackID=0 */
        client->video_channel = server->streams[stream_index].channel;
        client->codec = server->streams[stream_index].codec;
        IMP_LOG_ERR(TAG, "SETUP: stream='%s' -> stream_index=%d, video_channel=%d, codec=%d",
                   stream_name, stream_index, client->video_channel, client->codec);
    } else {
        IMP_LOG_ERR(TAG, "SETUP: Audio track setup: track_id=%d (not setting video_channel)", track_id);
    }

    /* Parse transport header */
    bool forced_udp_for_tls = false;

    if (strstr(transport, "RTP/AVP/TCP")) {
        /* TLS clients can use TCP transport - we'll handle RTP over TLS with flow control */
        // if (client->use_tls) {
        //     IMP_LOG_INFO(TAG, "RTSPS client requested TCP transport - using TLS with flow control");
        // }

        /* TCP transport (only for non-TLS clients) */
        client->transport_mode = RTSP_TRANSPORT_TCP;

        /* Extract interleaved channels */
        int rtp_channel = 0, rtcp_channel = 1;
        if (sscanf(transport, "%*[^;];%*[^;];interleaved=%d-%d", &rtp_channel, &rtcp_channel) == 2) {
            client->rtp_port = rtp_channel;
            client->rtcp_port = rtcp_channel;
        }

        /* Build transport response */
        snprintf(headers,
                 sizeof(headers),
                 "Transport: RTP/AVP/TCP;unicast;interleaved=%d-%d\r\n",
                 client->rtp_port,
                 client->rtcp_port);
    } else {
setup_udp_transport:
        /* UDP transport */
        client->transport_mode = RTSP_TRANSPORT_UDP;

        /* Extract client ports */
        int rtp_port = 0, rtcp_port = 0;
        bool ports_from_transport = false;

        if (sscanf(transport, "%*[^;];%*[^;];client_port=%d-%d", &rtp_port, &rtcp_port) == 2) {
            ports_from_transport = true;
        } else {
            /* For RTSPS clients forced to UDP, or clients that didn't specify ports */
            if (client->use_tls) {
                // IMP_LOG_INFO(TAG, "RTSPS client didn't specify client_port - using default ports");
                rtp_port = 5004;  /* Default RTP port */
                rtcp_port = 5005; /* Default RTCP port */
            } else {
                return send_rtsp_response(client, RTSP_STATUS_BAD_REQUEST, "Bad Request", NULL, NULL);
            }
        }

        client->rtp_port = rtp_port;
        client->rtcp_port = rtcp_port;

        /* Set up RTP destination */
        memset(&client->rtp_addr, 0, sizeof(client->rtp_addr));
        client->rtp_addr.sin_family = AF_INET;
        client->rtp_addr.sin_addr = client->client_addr.sin_addr;
        client->rtp_addr.sin_port = htons(client->rtp_port);

        /* Set up RTCP destination */
        memset(&client->rtcp_addr, 0, sizeof(client->rtcp_addr));
        client->rtcp_addr.sin_family = AF_INET;
        client->rtcp_addr.sin_addr = client->client_addr.sin_addr;
        client->rtcp_addr.sin_port = htons(client->rtcp_port);

        /* Create RTP UDP socket for this client */
        client->rtp_socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (client->rtp_socket_fd < 0) {
            IMP_LOG_ERR(TAG, "Failed to create RTP UDP socket: %s", strerror(errno));
            return send_rtsp_response(client, RTSP_STATUS_INTERNAL_ERROR, "Internal Server Error", NULL, NULL);
        }

        /* Optimize UDP socket buffers for streaming */
        int send_buffer_size = 256 * 1024; /* 256KB send buffer */
        int recv_buffer_size = 64 * 1024;  /* 64KB receive buffer */

        if (setsockopt(client->rtp_socket_fd, SOL_SOCKET, SO_SNDBUF, &send_buffer_size, sizeof(send_buffer_size)) < 0) {
            IMP_LOG_WARN(TAG, "Failed to set RTP socket send buffer size: %s", strerror(errno));
        }

        if (setsockopt(client->rtp_socket_fd, SOL_SOCKET, SO_RCVBUF, &recv_buffer_size, sizeof(recv_buffer_size)) < 0) {
            IMP_LOG_WARN(TAG, "Failed to set RTP socket receive buffer size: %s", strerror(errno));
        }

        IMP_LOG_INFO(TAG, "Created RTP UDP socket (fd=%d) for client %s:%d with optimized buffers",
                     client->rtp_socket_fd, inet_ntoa(client->rtp_addr.sin_addr), client->rtp_port);

        /* Build transport response - no server_port needed for UDP */
        if (forced_udp_for_tls) {
            /* For RTSPS clients forced from TCP to UDP, provide UDP response with server ports */
            snprintf(headers,
                     sizeof(headers),
                     "Transport: RTP/AVP;unicast;client_port=%d-%d;server_port=%d-%d\r\n",
                     client->rtp_port,
                     client->rtcp_port,
                     49152,  /* Standard RTP server port */
                     49153); /* Standard RTCP server port */
        } else {
            /* Standard UDP response */
            snprintf(headers,
                     sizeof(headers),
                     "Transport: RTP/AVP;unicast;client_port=%d-%d\r\n",
                     client->rtp_port,
                     client->rtcp_port);
        }
    }

    /* Update client state */
    client->state = RTSP_CLIENT_STATE_READY;

    /* SETUP only establishes transport - no media data sent yet */
    // IMP_LOG_INFO(TAG,
    //              "SETUP completed for channel %d, client ready for PLAY",
    //              client->video_channel);

    // IMP_LOG_INFO(TAG, "Sending SETUP response with headers: %s", headers);

    int result = send_rtsp_response(client, RTSP_STATUS_OK, "OK", headers, NULL);
    // IMP_LOG_INFO(TAG, "SETUP response sent, result: %d", result);

    return result;
}

static int handle_play_request(rtsp_server_t* server, rtsp_client_t* client)
{
    // IMP_LOG_INFO(TAG, "Handling PLAY request");

    /* Check for ONVIF backchannel support in PLAY request */
    if (strstr(client->request_buffer, "Require: www.onvif.org/ver20/backchannel") != NULL) {
        IMP_LOG_INFO(TAG, "PLAY request includes ONVIF backchannel support");
        client->supports_backchannel = true;
    }

    /* Check if client is in READY state (from SETUP or PAUSE) or already PLAYING (resume) */
    if (client->state != RTSP_CLIENT_STATE_READY && client->state != RTSP_CLIENT_STATE_PLAYING) {
        IMP_LOG_ERR(TAG,
                    "PLAY request rejected: client state is %d (expected READY=2 or PLAYING=3)",
                    client->state);
        return send_rtsp_response(client, RTSP_STATUS_BAD_REQUEST, "Bad Request", NULL, NULL);
    }

    /* Update client state */
    client->state = RTSP_CLIENT_STATE_PLAYING;

    /* Always generate fresh RTP parameters for new PLAY session to avoid sequence number issues */
    client->rtp_seq = (uint16_t) rand();
    client->rtp_ssrc = (uint32_t) rand();
    // IMP_LOG_INFO(TAG, "Generated fresh RTP sequence number: %u, SSRC: %u", client->rtp_seq, client->rtp_ssrc);

    /* Send response first */
    const char* headers = "Range: npt=0.000-\r\n";
    int result = send_rtsp_response(client, RTSP_STATUS_OK, "OK", headers, NULL);

    /* Skip GOP cache - always start clients with fresh IDR frames from live stream */
    if (result == 0 && client->video_channel < MAX_VIDEO_STREAMS) {
        /* Client will get frames from live stream only */
        // IMP_LOG_INFO(TAG, "Client will join live stream (GOP cache disabled for clean startup)");
        client->needs_idr = true;
        client->idr_wait_start_us = get_monotonic_time_us(); /* Track when client started waiting */

        /* Request IDR frame for this channel to minimize startup delay */
        unsigned long timestamp_us = client->idr_wait_start_us;
        // IMP_LOG_INFO(TAG, "Requesting immediate IDR frame for channel %d to reduce startup delay at %lu.%06lu",
        //             client->video_channel, timestamp_us / 1000000, timestamp_us % 1000000);
        extern time_t g_last_immediate_idr;
        g_last_immediate_idr = time(NULL);
        extern void request_idr_frame(int channel);
        request_idr_frame(client->video_channel);
    }

    return result;
}

static int handle_pause_request(rtsp_server_t* server, rtsp_client_t* client)
{
    // IMP_LOG_INFO(TAG, "Handling PAUSE request for client");

    /* Check if client is in PLAYING state */
    if (client->state != RTSP_CLIENT_STATE_PLAYING) {
        return send_rtsp_response(client, RTSP_STATUS_BAD_REQUEST, "Bad Request", NULL, NULL);
    }

    /* Update client state to READY (paused) */
    client->state = RTSP_CLIENT_STATE_READY;

    // IMP_LOG_INFO(TAG, "Client paused - state changed to READY");

    /* Send response */
    return send_rtsp_response(client, RTSP_STATUS_OK, "OK", NULL, NULL);
}

static int handle_teardown_request(rtsp_server_t* server, rtsp_client_t* client)
{
    IMP_LOG_INFO(TAG, "Handling TEARDOWN request - client requesting session termination");

    /* Update client state */
    client->state = RTSP_CLIENT_STATE_INIT;

    /* Send response */
    int ret = send_rtsp_response(client, RTSP_STATUS_OK, "OK", NULL, NULL);

    if (ret == 0) {
        IMP_LOG_INFO(TAG, "TEARDOWN response sent successfully, cleaning up client");
        /* Allow time for response to be transmitted before cleanup */
        usleep(10000); /* 10ms delay */
    } else {
        IMP_LOG_WARN(TAG, "Failed to send TEARDOWN response, proceeding with cleanup");
    }

    /* Clean up client resources */
    cleanup_client(client);
    server->client_count--;

    IMP_LOG_INFO(TAG, "TEARDOWN completed, client cleaned up");
    return ret;
}

static void generate_session_id(char* session_id, size_t size)
{
    static const char charset[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";

    /* Seed random number generator */
    static bool seeded = false;
    if (!seeded) {
        srand((unsigned int) time(NULL));
        seeded = true;
    }

    /* Generate random session ID */
    for (size_t i = 0; i < size - 1; i++) {
        int index = rand() % (sizeof(charset) - 1);
        session_id[i] = charset[index];
    }

    session_id[size - 1] = '\0';
}

static int generate_sdp(rtsp_server_t* server,
                        const char* stream_name,
                        char* sdp_buffer,
                        size_t buffer_size,
                        rtsp_client_t* client)
{
    /* TODO: SDP Generation Enhancements
     * 1. Extract actual SPS/PPS from H.264/H.265 encoder for sprop-parameter-sets
     * 2. Add audio media description support (OPUS, AAC, etc.)
     * 3. Dynamic profile-level-id extraction from encoder settings
     * 4. Support for multiple video tracks (main + sub streams)
     * 5. Real-time bandwidth calculation from encoder bitrate
     * 6. Add more standard SDP attributes for better client compatibility
     * 7. Support for encryption parameters if SRTP is implemented
     */

    /* Find stream by name */
    int stream_index = -1;
    for (int i = 0; i < server->stream_count; i++) {
        if (strcmp(stream_name, server->streams[i].stream_name) == 0) {
            stream_index = i;
            break;
        }
    }

    if (stream_index < 0) {
        return -1;
    }

    video_stream_config_t* stream = &server->streams[stream_index];

    IMP_LOG_INFO(TAG, "SDP: Generating for stream '%s' (channel %d): %dx%d",
                stream_name, stream->channel, stream->width, stream->height);

    /* Generate a random session ID for the SDP */
    char session_id[16];
    snprintf(session_id, sizeof(session_id), "%ld", (long) time(NULL));

    /* Build SDP */
    int len = 0;

    /* SDP version */
    len += snprintf(sdp_buffer + len, buffer_size - len, "v=0\r\n");

    /* Origin - use actual server IP */
    len += snprintf(sdp_buffer + len,
                    buffer_size - len,
                    "o=- %s %s IN IP4 %s\r\n",
                    session_id,
                    session_id,
                    g_config->general.server_ip);

    /* Session name */
    len += snprintf(sdp_buffer + len, buffer_size - len, "s=thingino %s\r\n", stream->stream_name);

    /* Session information */
    len += snprintf(sdp_buffer + len, buffer_size - len, "i=%s\r\n", stream->stream_name);

    /* Time */
    len += snprintf(sdp_buffer + len, buffer_size - len, "t=0 0\r\n");

    /* Session attributes */
    len += snprintf(sdp_buffer + len, buffer_size - len, "a=tool:Thingino Streamer v1.0\r\n");
    len += snprintf(sdp_buffer + len, buffer_size - len, "a=type:broadcast\r\n");
    len += snprintf(sdp_buffer + len, buffer_size - len, "a=control:*\r\n");
    len += snprintf(sdp_buffer + len, buffer_size - len, "a=range:npt=now-\r\n");
    len += snprintf(sdp_buffer + len, buffer_size - len, "a=x-qt-text-nam:thingino %s\r\n", stream->stream_name);
    len += snprintf(sdp_buffer + len, buffer_size - len, "a=x-qt-text-inf:%s\r\n", stream->stream_name);

    /* Media description - use proper RTP port instead of 0 */
    len += snprintf(sdp_buffer + len,
                    buffer_size - len,
                    "m=video 0 RTP/AVP %d\r\n",
                    stream->codec == VIDEO_CODEC_H265 ? RTP_PAYLOAD_TYPE_H265
                                                      : RTP_PAYLOAD_TYPE_H264);

    /* Connection info for video */
    len += snprintf(sdp_buffer + len, buffer_size - len, "c=IN IP4 0.0.0.0\r\n");

    /* TODO: Replace rough bandwidth estimation with actual encoder bitrate
     * Current calculation is based on resolution/fps which is inaccurate
     * Should get target bitrate from encoder configuration */
    int video_bandwidth = (stream->width * stream->height * stream->fps) / 1000; // Rough estimate
    if (video_bandwidth < 1000) video_bandwidth = 1000;
    if (video_bandwidth > 10000) video_bandwidth = 10000;
    len += snprintf(sdp_buffer + len, buffer_size - len, "b=AS:%d\r\n", video_bandwidth);

    /* Media attributes */
    if (stream->codec == VIDEO_CODEC_H264) {
        /* TODO: Extract actual SPS/PPS NAL units from encoder and include in sprop-parameter-sets
         * Current hardcoded profile-level-id should be replaced with actual encoder settings
         * Example: sprop-parameter-sets=J2QAM60AzoB4AiflmoCAgPgAAAMACAAAAwGRgIACq5AACALL//gU,KO48sA==
         * This requires parsing the first I-frame or getting SPS/PPS from encoder configuration */
        len += snprintf(
            sdp_buffer + len,
            buffer_size - len,
            "a=rtpmap:%d H264/90000\r\n"
            "a=fmtp:%d profile-level-id=42e01f;packetization-mode=1\r\n",
            RTP_PAYLOAD_TYPE_H264,
            RTP_PAYLOAD_TYPE_H264);
    } else {
        /* TODO: Extract actual VPS/SPS/PPS from H.265 encoder instead of hardcoded values
         * Should get profile-space, profile-id, tier-flag, level-id from actual encoder settings */
        len += snprintf(
            sdp_buffer + len,
            buffer_size - len,
            "a=rtpmap:%d H265/90000\r\n"
            "a=fmtp:%d profile-space=0;profile-id=1;tier-flag=0;level-id=93\r\n",
            RTP_PAYLOAD_TYPE_H265,
            RTP_PAYLOAD_TYPE_H265);
    }

    /* Control attribute - use standard track1 for video */
    len += snprintf(sdp_buffer + len, buffer_size - len, "a=control:track1\r\n");

    /* Additional info */
    if (stream->stream_info && strlen(stream->stream_info) > 0) {
        len += snprintf(sdp_buffer + len, buffer_size - len, "a=x-desc:%s\r\n", stream->stream_info);
    }

    /* Resolution */
    len += snprintf(sdp_buffer + len,
                    buffer_size - len,
                    "a=x-dimensions:%d,%d\r\n",
                    stream->width,
                    stream->height);

    /* Framerate */
    len += snprintf(sdp_buffer + len, buffer_size - len, "a=framerate:%d\r\n", stream->fps);

    /* Add ONVIF audio backchannel streams if client supports it */
    if (client && client->supports_backchannel) {
        IMP_LOG_INFO(TAG, "Adding ONVIF audio backchannel streams to SDP");

        /* Add downstream audio (from device to client) - recvonly */
        len += snprintf(sdp_buffer + len, buffer_size - len, "m=audio 0 RTP/AVP 0\r\n");
        len += snprintf(sdp_buffer + len, buffer_size - len, "c=IN IP4 0.0.0.0\r\n");
        len += snprintf(sdp_buffer + len, buffer_size - len, "b=AS:64\r\n");
        len += snprintf(sdp_buffer + len, buffer_size - len, "a=rtpmap:0 PCMU/8000\r\n");
        len += snprintf(sdp_buffer + len, buffer_size - len, "a=control:rtsp://%s:%d/%s/audio\r\n",
                       g_config->general.server_ip, server->config.port, stream_name);
        len += snprintf(sdp_buffer + len, buffer_size - len, "a=recvonly\r\n");

        /* Add upstream audio backchannel (from client to device) - sendonly */
        len += snprintf(sdp_buffer + len, buffer_size - len, "m=audio 0 RTP/AVP 0\r\n");
        len += snprintf(sdp_buffer + len, buffer_size - len, "c=IN IP4 0.0.0.0\r\n");
        len += snprintf(sdp_buffer + len, buffer_size - len, "b=AS:64\r\n");
        len += snprintf(sdp_buffer + len, buffer_size - len, "a=rtpmap:0 PCMU/8000\r\n");
        len += snprintf(sdp_buffer + len, buffer_size - len, "a=control:rtsp://%s:%d/%s/G711_audiobackchannel\r\n",
                       g_config->general.server_ip, server->config.port, stream_name);
        len += snprintf(sdp_buffer + len, buffer_size - len, "a=sendonly\r\n");

        /* Add G.726 backchannel option */
        len += snprintf(sdp_buffer + len, buffer_size - len, "m=audio 98 RTP/AVP 98\r\n");
        len += snprintf(sdp_buffer + len, buffer_size - len, "c=IN IP4 0.0.0.0\r\n");
        len += snprintf(sdp_buffer + len, buffer_size - len, "b=AS:32\r\n");
        len += snprintf(sdp_buffer + len, buffer_size - len, "a=rtpmap:98 G726-16/8000\r\n");
        len += snprintf(sdp_buffer + len, buffer_size - len, "a=control:rtsp://%s:%d/%s/G726_audiobackchannel\r\n",
                       g_config->general.server_ip, server->config.port, stream_name);
        len += snprintf(sdp_buffer + len, buffer_size - len, "a=sendonly\r\n");
    }

    /* TODO: Add audio media description if audio is available
     * Should check if audio encoder is enabled and add:
     * m=audio 0 RTP/AVP 97
     * c=IN IP4 0.0.0.0
     * b=AS:40
     * a=rtpmap:97 OPUS/48000/2  (or other audio codec)
     * a=control:track2
     * This requires integration with audio encoder module */

    /* TODO: Add dynamic bandwidth calculation based on actual encoder bitrate settings
     * Current bandwidth estimation is rough - should get actual target bitrate from encoder */

    /* TODO: Add support for multiple video tracks (e.g., main + sub stream)
     * Each track should have its own media description with different payload types */

    return 0;
}

static int send_h265_fragmented_rtp(
    rtsp_client_t* client, const uint8_t* data, unsigned int size, uint32_t timestamp, bool marker)
{
    const int max_payload_size = MAX_RTP_PACKET_SIZE - RTP_HEADER_SIZE - 3; /* -3 for FU headers */
    const uint8_t* nal_data = data;
    unsigned int remaining = size;

    /* Skip NAL unit start codes first for size calculation */
    const uint8_t* nal_unit_data = nal_data;
    unsigned int nal_unit_size = remaining;

    if (size >= 4 && nal_data[0] == 0x00 && nal_data[1] == 0x00 && nal_data[2] == 0x00
        && nal_data[3] == 0x01) {
        nal_unit_data += 4;
        nal_unit_size -= 4;
    } else if (size >= 3 && nal_data[0] == 0x00 && nal_data[1] == 0x00 && nal_data[2] == 0x01) {
        nal_unit_data += 3;
        nal_unit_size -= 3;
    }

    /* If NAL unit fits in single packet, don't use fragmentation headers */
    if (nal_unit_size <= max_payload_size) {
        uint8_t packet[MAX_RTP_PACKET_SIZE];

        /* Build RTP header */
        packet[0] = 0x80; /* RTP version 2 */
        packet[1] = RTP_PAYLOAD_TYPE_H265;
        if (marker) {
            packet[1] |= 0x80; /* Marker bit */
        }

        /* Sequence number */
        packet[2] = (client->rtp_seq >> 8) & 0xFF;
        packet[3] = client->rtp_seq & 0xFF;
        client->rtp_seq++;

        /* Timestamp */
        packet[4] = (timestamp >> 24) & 0xFF;
        packet[5] = (timestamp >> 16) & 0xFF;
        packet[6] = (timestamp >> 8) & 0xFF;
        packet[7] = timestamp & 0xFF;

        /* SSRC */
        packet[8] = (client->rtp_ssrc >> 24) & 0xFF;
        packet[9] = (client->rtp_ssrc >> 16) & 0xFF;
        packet[10] = (client->rtp_ssrc >> 8) & 0xFF;
        packet[11] = client->rtp_ssrc & 0xFF;

        /* Copy NAL unit data without start codes */
        memcpy(packet + RTP_HEADER_SIZE, nal_unit_data, nal_unit_size);
        int packet_size = RTP_HEADER_SIZE + nal_unit_size;

        /* Send packet */
        if (client->transport_mode == RTSP_TRANSPORT_UDP) {
            if (sendto(client->rtp_socket_fd, packet, packet_size, 0,
                       (struct sockaddr*) &client->rtp_addr, sizeof(client->rtp_addr)) < 0) {
                IMP_LOG_ERR(TAG, "Failed to send H.265 single NAL RTP packet: %s", strerror(errno));
                return -1;
            }
        } else {
            /* TCP transport */
            uint8_t header[4] = {'$', client->rtp_port, (packet_size >> 8) & 0xFF, packet_size & 0xFF};
            if (send(client->socket_fd, header, 4, 0) < 4 ||
                send(client->socket_fd, packet, packet_size, 0) < packet_size) {
                IMP_LOG_ERR(TAG, "Failed to send H.265 single NAL RTP packet: %s", strerror(errno));
                return -1;
            }
        }

        return packet_size;
    }
    int fragments_sent = 0;

    /* Use the already processed NAL unit data */
    nal_data = nal_unit_data;
    remaining = nal_unit_size;

    if (remaining < 2) {
        return 0; /* H.265 NAL header is 2 bytes */
    }

    /* Extract H.265 NAL unit header (2 bytes) - RFC 7798 Section 1.1.4 */
    uint16_t nal_header = (nal_data[0] << 8) | nal_data[1];
    uint8_t nal_type = (nal_header >> 9) & 0x3F;
    uint8_t nuh_layer_id = (nal_header >> 3) & 0x3F;
    uint8_t nuh_temporal_id = nal_header & 0x07;

    /* Skip the original NAL header (2 bytes for H.265) */
    nal_data += 2;
    remaining -= 2;

    /* RFC 7798 Section 4.4.3: H.265 Fragmentation Unit format */
    /* PayloadHdr (2 bytes) = FU indicator (2 bytes) */
    /* FU header (1 byte) */

    int fragment_num = 0;
    while (remaining > 0) {
        uint8_t packet[MAX_RTP_PACKET_SIZE];
        int payload_size = (remaining > max_payload_size) ? max_payload_size : remaining;
        bool is_last_fragment = (remaining <= max_payload_size);

        /* Build RTP header */
        packet[0] = 0x80; /* RTP version 2 */
        packet[1] = RTP_PAYLOAD_TYPE_H265;
        if (is_last_fragment && marker) {
            packet[1] |= 0x80; /* Marker bit only on last fragment */
        }

        /* Sequence number */
        packet[2] = (client->rtp_seq >> 8) & 0xFF;
        packet[3] = client->rtp_seq & 0xFF;
        client->rtp_seq++;

        /* Timestamp */
        packet[4] = (timestamp >> 24) & 0xFF;
        packet[5] = (timestamp >> 16) & 0xFF;
        packet[6] = (timestamp >> 8) & 0xFF;
        packet[7] = timestamp & 0xFF;

        /* SSRC */
        packet[8] = (client->rtp_ssrc >> 24) & 0xFF;
        packet[9] = (client->rtp_ssrc >> 16) & 0xFF;
        packet[10] = (client->rtp_ssrc >> 8) & 0xFF;
        packet[11] = client->rtp_ssrc & 0xFF;

        /* RFC 7798: PayloadHdr (2 bytes) - F=0, Type=49, LayerId=original, TID=original */
        /* Format: |F|   Type    |  LayerId  | TID | */
        /*         |0|1|2|3|4|5|6|7|0|1|2|3|4|5|6|7| */
        uint16_t payload_hdr = (0 << 15) |             /* F = 0 */
                               (49 << 9) |             /* Type = 49 (FU) */
                               (nuh_layer_id << 3) |   /* LayerId = original */
                               (nuh_temporal_id);      /* TID = original */
        packet[12] = (payload_hdr >> 8) & 0xFF;
        packet[13] = payload_hdr & 0xFF;

        /* FU header (1 byte) - RFC 7798 Section 4.4.3 */
        uint8_t fu_header = nal_type; /* Original NAL unit type */
        if (fragment_num == 0) {
            fu_header |= 0x80; /* S bit - Start of fragmented NAL unit */
        }
        if (is_last_fragment) {
            fu_header |= 0x40; /* E bit - End of fragmented NAL unit */
        }
        packet[14] = fu_header;

        /* Copy payload data */
        memcpy(packet + RTP_HEADER_SIZE + 3, nal_data, payload_size);

        int packet_size = RTP_HEADER_SIZE + 3 + payload_size;

        /* Send packet */
        if (client->transport_mode == RTSP_TRANSPORT_UDP) {
            if (sendto(client->rtp_socket_fd,
                       packet,
                       packet_size,
                       0,
                       (struct sockaddr*) &client->rtp_addr,
                       sizeof(client->rtp_addr))
                < 0) {
                IMP_LOG_ERR(TAG, "Failed to send H.265 FU RTP packet: %s", strerror(errno));
                return -1;
            }
        } else {
            /* TCP transport */
            uint8_t header[4];
            header[0] = '$';
            header[1] = client->rtp_port;
            header[2] = (packet_size >> 8) & 0xFF;
            header[3] = packet_size & 0xFF;

            if (send(client->socket_fd, header, 4, 0) < 4) {
                return -1;
            }
            if (send(client->socket_fd, packet, packet_size, 0) < packet_size) {
                return -1;
            }
        }

        nal_data += payload_size;
        remaining -= payload_size;
        fragments_sent++;
        fragment_num++;
    }

    return fragments_sent;
}

static int send_h264_fragmented_rtp(
    rtsp_client_t* client, const uint8_t* data, unsigned int size, uint32_t timestamp, bool marker)
{
    /* Increase safety limit for IDR frames which can be large */
    const unsigned int MAX_SAFE_FRAME_SIZE = 300 * 1024; /* 300KB limit for IDR frames */
    if (size > MAX_SAFE_FRAME_SIZE) {
        IMP_LOG_ERR(TAG, "Frame too large for fragmentation: %u bytes, dropping", size);
        return -1;
    }

    const int max_payload_size = MAX_RTP_PACKET_SIZE - RTP_HEADER_SIZE - 2;
    const uint8_t* nal_data = data;
    unsigned int remaining = size;
    int fragments_sent = 0;
    int rtp_socket = -1;

    /* Skip NAL unit start codes (0x00 0x00 0x00 0x01 or 0x00 0x00 0x01) */
    if (size >= 4 && nal_data[0] == 0x00 && nal_data[1] == 0x00 && nal_data[2] == 0x00
        && nal_data[3] == 0x01) {
        nal_data += 4;
        remaining -= 4;
    } else if (size >= 3 && nal_data[0] == 0x00 && nal_data[1] == 0x00 && nal_data[2] == 0x01) {
        nal_data += 3;
        remaining -= 3;
    }

    if (remaining == 0) {
        return 0;
    }

    uint8_t nal_type = nal_data[0] & 0x1F;
    uint8_t nal_nri = nal_data[0] & 0x60;

    /* If NAL unit fits in single packet, don't use fragmentation headers */
    if (remaining <= max_payload_size) {
        uint8_t packet[MAX_RTP_PACKET_SIZE];

        /* Build RTP header */
        packet[0] = 0x80; /* RTP version 2 */
        packet[1] = RTP_PAYLOAD_TYPE_H264;
        if (marker) {
            packet[1] |= 0x80; /* Marker bit */
        }

        /* Sequence number */
        packet[2] = (client->rtp_seq >> 8) & 0xFF;
        packet[3] = client->rtp_seq & 0xFF;
        client->rtp_seq++;

        /* Timestamp */
        packet[4] = (timestamp >> 24) & 0xFF;
        packet[5] = (timestamp >> 16) & 0xFF;
        packet[6] = (timestamp >> 8) & 0xFF;
        packet[7] = timestamp & 0xFF;

        /* SSRC */
        packet[8] = (client->rtp_ssrc >> 24) & 0xFF;
        packet[9] = (client->rtp_ssrc >> 16) & 0xFF;
        packet[10] = (client->rtp_ssrc >> 8) & 0xFF;
        packet[11] = client->rtp_ssrc & 0xFF;

        /* Copy NAL unit data directly (no fragmentation headers) */
        memcpy(packet + RTP_HEADER_SIZE, nal_data, remaining);
        int packet_size = RTP_HEADER_SIZE + remaining;

        /* Send packet */
        if (client->transport_mode == RTSP_TRANSPORT_UDP) {
            if (sendto(client->rtp_socket_fd, packet, packet_size, 0,
                       (struct sockaddr*) &client->rtp_addr, sizeof(client->rtp_addr)) < 0) {
                IMP_LOG_ERR(TAG, "Failed to send H.264 single NAL RTP packet: %s", strerror(errno));
                return -1;
            }
        } else {
            /* TCP transport - combine header and packet for single send() */
            uint8_t tcp_packet[MAX_RTP_PACKET_SIZE + 4];
            tcp_packet[0] = '$';
            tcp_packet[1] = client->rtp_port;
            tcp_packet[2] = (packet_size >> 8) & 0xFF;
            tcp_packet[3] = packet_size & 0xFF;

            /* Copy RTP packet after TCP header */
            memcpy(tcp_packet + 4, packet, packet_size);

            int total_size = packet_size + 4;
            ssize_t sent = rtsp_client_tls_write(client, (const char*)tcp_packet, total_size);
            if (sent < total_size) {
                IMP_LOG_ERR(TAG, "Failed to send H.264 single NAL RTP packet: %zd/%d bytes, error: %s", sent, total_size, strerror(errno));
                return -1;
            }

            /* Add small delay for TLS clients to prevent overwhelming the connection */
            if (client->use_tls) {
                usleep(25); /* Reduced to 0.025ms delay for TLS flow control */
            }
        }

        return packet_size;
    }

    /* Skip the original NAL header */
    nal_data++;
    remaining--;

    /* Use UDP socket created during SETUP */
    if (client->transport_mode == RTSP_TRANSPORT_UDP) {
        rtp_socket = client->rtp_socket_fd;
    }

    while (remaining > 0) {
        uint8_t packet[MAX_RTP_PACKET_SIZE];
        int packet_size;
        bool is_first = (fragments_sent == 0);
        bool is_last = (remaining <= max_payload_size);
        int payload_size = (remaining > max_payload_size) ? max_payload_size : remaining;

        /* Build RTP header */
        packet[0] = 0x80; /* RTP version 2 */
        packet[1] = RTP_PAYLOAD_TYPE_H264;
        if (is_last && marker) {
            packet[1] |= 0x80; /* Marker bit only on last fragment */
        }

        /* Sequence number - increment for EVERY fragment */
        packet[2] = (client->rtp_seq >> 8) & 0xFF;
        packet[3] = client->rtp_seq & 0xFF;
        client->rtp_seq++; /* Increment sequence number for every RTP packet */

        /* Timestamp */
        packet[4] = (timestamp >> 24) & 0xFF;
        packet[5] = (timestamp >> 16) & 0xFF;
        packet[6] = (timestamp >> 8) & 0xFF;
        packet[7] = timestamp & 0xFF;

        /* SSRC */
        packet[8] = (client->rtp_ssrc >> 24) & 0xFF;
        packet[9] = (client->rtp_ssrc >> 16) & 0xFF;
        packet[10] = (client->rtp_ssrc >> 8) & 0xFF;
        packet[11] = client->rtp_ssrc & 0xFF;

        /* FU indicator */
        packet[12] = nal_nri | 28; /* NAL type 28 = FU-A */

        /* FU header */
        packet[13] = (is_first ? 0x80 : 0x00) | (is_last ? 0x40 : 0x00) | nal_type;

        /* Copy payload */
        memcpy(packet + RTP_HEADER_SIZE + 2, nal_data, payload_size);
        packet_size = RTP_HEADER_SIZE + 2 + payload_size;

        /* Send fragment */
        if (client->transport_mode == RTSP_TRANSPORT_UDP) {
            if (sendto(rtp_socket,
                       packet,
                       packet_size,
                       0,
                       (struct sockaddr*) &client->rtp_addr,
                       sizeof(client->rtp_addr))
                < 0) {
                IMP_LOG_ERR(TAG, "Failed to send RTP fragment: %s", strerror(errno));
                return -1;
            }
        } else {
            /* TCP transport - combine header and packet for single send() */
            uint8_t tcp_packet[MAX_RTP_PACKET_SIZE + 4];
            tcp_packet[0] = '$';
            tcp_packet[1] = client->rtp_port;
            tcp_packet[2] = (packet_size >> 8) & 0xFF;
            tcp_packet[3] = packet_size & 0xFF;

            /* Copy RTP packet after TCP header */
            memcpy(tcp_packet + 4, packet, packet_size);

            int total_size = packet_size + 4;
            ssize_t sent = rtsp_client_tls_write(client, (const char*)tcp_packet, total_size);
            if (sent < total_size) {
                IMP_LOG_ERR(TAG, "Failed to send RTP fragment: %zd/%d bytes, error: %s", sent, total_size, strerror(errno));
                return -1;
            }

            /* Add small delay for TLS clients to prevent overwhelming the connection */
            if (client->use_tls) {
                usleep(25); /* Reduced to 0.025ms delay for TLS flow control */
            }
        }

        nal_data += payload_size;
        remaining -= payload_size;
        fragments_sent++;
    }

    return fragments_sent;
}

/* FIXME: Network optimization needed for RTP packet delivery
 * Issue: Packet loss increases significantly during active network usage
 * Symptoms: "RTP: missed X packets" and "max delay reached" in client logs
 * - Idle network: occasional 1 packet loss
 * - Active network: 1-11 packets lost per burst, frequent occurrences
 *
 * Optimization opportunities:
 * 1. Adaptive bitrate - reduce quality when congestion detected
 * 2. Jitter buffer tuning - increase buffer sizes for network variations
 * 3. Packet pacing - spread RTP packets more evenly over time
 * 4. UDP socket buffer optimization - increase OS-level send/receive buffers
 * 5. Frame rate adaptation - temporarily reduce FPS during congestion
 * 6. GOP size optimization - smaller GOPs for better error recovery
 * 7. RTP packet size tuning - optimize for network MTU
 *
 * Root cause: Network bandwidth competition between RTSP streaming and other traffic
 */
static int send_rtp_packet(
    rtsp_client_t* client, const void* data, unsigned int size, uint32_t timestamp, bool marker)
{
    uint8_t packet[MAX_RTP_PACKET_SIZE];
    int packet_size;

    /* Check if client is in PLAYING state */
    if (client->state != RTSP_CLIENT_STATE_PLAYING) {
        return 0;
    }

    /* Check if we need to fragment the packet */
    int max_single_packet_size = MAX_RTP_PACKET_SIZE - RTP_HEADER_SIZE - 4;
    // IMP_LOG_DBG(TAG, "send_rtp_packet: size=%u, max_single=%d, codec=%d, needs_fragment=%s",
    //             size, max_single_packet_size, client->codec,
    //             (size > max_single_packet_size) ? "YES" : "NO");

    if (size > max_single_packet_size) { /* -4 for potential NAL headers */
        if (client->codec == VIDEO_CODEC_H264) {
            // IMP_LOG_DBG(TAG, "Calling H.264 fragmentation for %u bytes", size);
            return send_h264_fragmented_rtp(client, (const uint8_t*) data, size, timestamp, marker);
        } else if (client->codec == VIDEO_CODEC_H265) {
            // IMP_LOG_INFO(TAG, "Calling H.265 fragmentation for %u bytes", size);
            return send_h265_fragmented_rtp(client, (const uint8_t*) data, size, timestamp, marker);
        } else {
            IMP_LOG_ERR(TAG, "WARNING: RTP packet too large (%d bytes), unsupported codec", size);
            return 0;
        }
    }

    /* Build RTP header */
    packet[0] = 0x80; /* RTP version 2 */
    packet[1] = (client->codec == VIDEO_CODEC_H265 ? RTP_PAYLOAD_TYPE_H265 : RTP_PAYLOAD_TYPE_H264);
    if (marker) {
        packet[1] |= 0x80; /* Marker bit */
    }

    /* Sequence number */
    packet[2] = (client->rtp_seq >> 8) & 0xFF;
    packet[3] = client->rtp_seq & 0xFF;
    client->rtp_seq++; /* Increment sequence number for every RTP packet */

    /* Timestamp */
    packet[4] = (timestamp >> 24) & 0xFF;
    packet[5] = (timestamp >> 16) & 0xFF;
    packet[6] = (timestamp >> 8) & 0xFF;
    packet[7] = timestamp & 0xFF;

    /* SSRC (use client's SSRC) */
    packet[8] = (client->rtp_ssrc >> 24) & 0xFF;
    packet[9] = (client->rtp_ssrc >> 16) & 0xFF;
    packet[10] = (client->rtp_ssrc >> 8) & 0xFF;
    packet[11] = client->rtp_ssrc & 0xFF;

    /* Process payload data based on codec */
    if (client->codec == VIDEO_CODEC_H265) {
        /* For H.265, we need to ensure proper NAL unit format */
        const uint8_t* nal_data = (const uint8_t*) data;
        unsigned int nal_size = size;

        /* Skip start codes if present */
        if (size >= 4 && nal_data[0] == 0x00 && nal_data[1] == 0x00 && nal_data[2] == 0x00
            && nal_data[3] == 0x01) {
            nal_data += 4;
            nal_size -= 4;
        } else if (size >= 3 && nal_data[0] == 0x00 && nal_data[1] == 0x00 && nal_data[2] == 0x01) {
            nal_data += 3;
            nal_size -= 3;
        }

        if (nal_size >= 2) {
            /* H.265 NAL unit - copy as-is without modifying temporal ID */
            memcpy(packet + RTP_HEADER_SIZE, nal_data, nal_size);
            packet_size = RTP_HEADER_SIZE + nal_size;
        } else {
            /* Invalid H.265 NAL unit */
            return 0;
        }
    } else {
        /* H.264 or other codecs - copy as-is */
        memcpy(packet + RTP_HEADER_SIZE, data, size);
        packet_size = RTP_HEADER_SIZE + size;
    }

    /* Send packet */
    if (client->transport_mode == RTSP_TRANSPORT_UDP) {
        /* UDP transport - use socket created during SETUP */
        if (client->rtp_socket_fd < 0) {
            IMP_LOG_ERR(TAG, "RTP socket not initialized (fd=%d)", client->rtp_socket_fd);
            return -1;
        }

        ssize_t sent = sendto(client->rtp_socket_fd,
                             packet,
                             packet_size,
                             0,
                             (struct sockaddr*) &client->rtp_addr,
                             sizeof(client->rtp_addr));
        if (sent < 0) {
            IMP_LOG_ERR(TAG, "Failed to send RTP packet: %s", strerror(errno));
            /* Track send failures */
            static int send_failures = 0;
            send_failures++;
            if (send_failures % 100 == 1) {
                IMP_LOG_WARN(TAG, "RTP send failures: %d total", send_failures);
            }
            return -1;
        } else if (sent != packet_size) {
            IMP_LOG_WARN(TAG, "Partial RTP packet sent: %zd/%d bytes to %s:%d",
                        sent, packet_size, inet_ntoa(client->rtp_addr.sin_addr), ntohs(client->rtp_addr.sin_port));
            /* Track partial sends */
            static int partial_sends = 0;
            partial_sends++;
            if (partial_sends % 50 == 1) {
                IMP_LOG_WARN(TAG, "RTP partial sends: %d total", partial_sends);
            }
        }

        // IMP_LOG_INFO(TAG, "Sent RTP packet: %zd bytes to %s:%d (socket=%d)",
        //             sent, inet_ntoa(client->rtp_addr.sin_addr), ntohs(client->rtp_addr.sin_port), client->rtp_socket_fd);
    } else {
        /* TCP transport (interleaved) - use proper framing for both TLS and non-TLS */
        uint8_t header[4];
        header[0] = '$';              /* Dollar sign for interleaved data */
        header[1] = client->rtp_port; /* Channel ID */
        header[2] = (packet_size >> 8) & 0xFF;
        header[3] = packet_size & 0xFF;

        /* Extract RTP header info for debugging */
        uint16_t seq_num = (packet[2] << 8) | packet[3];
        uint32_t rtp_timestamp = (packet[4] << 24) | (packet[5] << 16) | (packet[6] << 8) | packet[7];
        bool marker_bit = (packet[1] & 0x80) != 0;

        // IMP_LOG_DBG(TAG, "Sending TCP RTP: channel=%d, size=%d, seq=%u, ts=%u, marker=%d, socket=%d, TLS=%s",
        //             client->rtp_port, packet_size, seq_num, rtp_timestamp, marker_bit, client->socket_fd,
        //             client->use_tls ? "yes" : "no");

        /* Validate that RTP packet doesn't start with RTSP-like data */
        if (packet_size >= 4 &&
            (memcmp(packet, "RTSP", 4) == 0 || memcmp(packet, "GET ", 4) == 0 ||
             memcmp(packet, "POST", 4) == 0 || memcmp(packet, "OPTI", 4) == 0)) {
            IMP_LOG_WARN(TAG, "RTP packet starts with HTTP/RTSP-like data, skipping to prevent client confusion");
            return 0; /* Skip this packet */
        }

        /* Combine header and packet for single send() to reduce TCP overhead */
        uint8_t tcp_packet[MAX_RTP_PACKET_SIZE + 4];
        memcpy(tcp_packet, header, 4);
        memcpy(tcp_packet + 4, packet, packet_size);

        int total_size = packet_size + 4;
        ssize_t sent = rtsp_client_tls_write(client, (const char*)tcp_packet, total_size);
        if (sent < total_size) {
            /* Log the specific error and what might be causing 501 responses */
            if (sent == -1) {
                IMP_LOG_ERR(TAG, "TCP RTP send failed completely: %s (this might cause client 501 responses)", strerror(errno));
            } else {
                IMP_LOG_ERR(TAG, "TCP RTP partial send: %zd/%d bytes, error: %s (this might cause client 501 responses)",
                           sent, total_size, strerror(errno));
            }
            return -1;
        }

        /* Add small delay for TLS clients to prevent overwhelming the connection */
        if (client->use_tls) {
            usleep(50); /* Reduced to 0.05ms delay for TLS flow control */
        }

        // IMP_LOG_DBG(TAG, "TCP RTP sent successfully: header=4 bytes, packet=%d bytes", packet_size);
    }

    return packet_size;
}

/* Extract stream name from URL, supporting both rtsp:// and rtsps:// schemes */
static int extract_stream_name_from_url(rtsp_server_t* server, const char* url, char* stream_name, size_t stream_name_size)
{
    /* Extract stream name from URL (support both rtsp:// and rtsps://) */
    if (sscanf(url, "rtsp://%*[^/]/%s", stream_name) != 1 &&
        sscanf(url, "rtsps://%*[^/]/%s", stream_name) != 1) {
        /* Try without hostname */
        if (sscanf(url, "/%s", stream_name) != 1) {
            /* Default to first stream */
            if (server->stream_count > 0) {
                strncpy(stream_name, server->streams[0].stream_name, stream_name_size - 1);
                stream_name[stream_name_size - 1] = '\0';
            } else {
                return -1; /* No streams available */
            }
        }
    }

    return 0; /* Success */
}

static rtsp_client_t* find_free_client_slot(rtsp_server_t* server)
{
    for (int i = 0; i < MAX_RTSP_CLIENTS; i++) {
        if (!server->clients[i].active) {
            return &server->clients[i];
        }
    }

    return NULL;
}

static void cleanup_client(rtsp_client_t* client)
{
    if (!client)
        return;

    /* Clean up TLS context first */
    rtsp_client_tls_cleanup(client);

    /* Close sockets */
    if (client->socket_fd >= 0) {
        close(client->socket_fd);
        client->socket_fd = -1;
    }

    if (client->rtp_socket_fd >= 0) {
        close(client->rtp_socket_fd);
        client->rtp_socket_fd = -1;
    }

    /* Destroy SSL mutex */
    pthread_mutex_destroy(&client->ssl_mutex);

    /* Reset client state */
    client->active = false;
    client->state = RTSP_CLIENT_STATE_INIT;
    client->session_id[0] = '\0';

    /* Reinitialize SSL mutex for potential reuse */
    pthread_mutex_init(&client->ssl_mutex, NULL);
}

void rtsp_server_get_stats(rtsp_server_t* server,
                                   unsigned long* connections,
                                   unsigned long* bytes_sent,
                                   unsigned long* packets_sent)
{
    if (!server)
        return;

    if (connections)
        *connections = server->total_connections;
    if (bytes_sent)
        *bytes_sent = server->total_bytes_sent;
    if (packets_sent)
        *packets_sent = server->total_packets_sent;
}

/* Initialize RTSP server with stream info */
int rtsp_server_init(rtsp_server_config_t* config, stream_info_t* inputs[], int num_inputs)
{
    /* This is a placeholder for integration with stream system */
    // IMP_LOG_INFO(TAG, "RTSP server init called with %d inputs", num_inputs);
    return 0;
}

video_codec_t string_to_video_codec(const char* format_str)
{
    if (!format_str || strlen(format_str) == 0) {
        return VIDEO_CODEC_H264; /* Default */
    }

    if (strcasecmp(format_str, "H264") == 0)
        return VIDEO_CODEC_H264;
    if (strcasecmp(format_str, "H265") == 0 || strcasecmp(format_str, "HEVC") == 0)
        return VIDEO_CODEC_H265;

    IMP_LOG_ERR(TAG, "Unknown video codec format '%s' - configuration error", format_str);
    abort(); /* Abort on invalid codec to prevent undefined behavior */
}

/* ========================================================================
 * TLS/SSL Implementation for RTSPS Support
 * ======================================================================== */

#ifdef RTSPS_BACKEND_OPENSSL

static int rtsp_server_tls_init(rtsp_server_t* server)
{
    if (!server || !server->config.tls_enabled) {
        return 0; /* TLS not enabled */
    }

    // IMP_LOG_INFO(TAG, "Initializing OpenSSL TLS for RTSPS server");

    /* Initialize OpenSSL */
    SSL_library_init();
    SSL_load_error_strings();
    OpenSSL_add_all_algorithms();

    /* Create SSL context */
    const SSL_METHOD* method = TLS_server_method();
    SSL_CTX* ctx = SSL_CTX_new(method);
    if (!ctx) {
        IMP_LOG_ERR(TAG, "Failed to create SSL context");
        return -1;
    }

    /* Configure SSL context */
    SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, NULL); /* Skip client verification for embedded use */
    SSL_CTX_set_options(ctx, SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3); /* Disable old protocols */

    /* Configure session timeout and buffer settings for sustained streaming */
    SSL_CTX_set_timeout(ctx, 3600); /* 1 hour session timeout instead of default */
    SSL_CTX_set_session_cache_mode(ctx, SSL_SESS_CACHE_OFF); /* Disable session caching for embedded use */
    SSL_CTX_set_read_ahead(ctx, 1); /* Enable read-ahead for better performance */

    /* Load certificate and private key if provided */
    if (strlen(server->config.cert_file) > 0) {
        if (SSL_CTX_use_certificate_file(ctx, server->config.cert_file, SSL_FILETYPE_PEM) <= 0) {
            IMP_LOG_ERR(TAG, "Failed to load certificate file: %s", server->config.cert_file);
            SSL_CTX_free(ctx);
            return -1;
        }
        IMP_LOG_INFO(TAG, "Loaded certificate file: %s", server->config.cert_file);
    }

    if (strlen(server->config.key_file) > 0) {
        if (SSL_CTX_use_PrivateKey_file(ctx, server->config.key_file, SSL_FILETYPE_PEM) <= 0) {
            IMP_LOG_ERR(TAG, "Failed to load private key file: %s", server->config.key_file);
            SSL_CTX_free(ctx);
            return -1;
        }
        IMP_LOG_INFO(TAG, "Loaded private key file: %s", server->config.key_file);

        /* Verify private key matches certificate */
        if (!SSL_CTX_check_private_key(ctx)) {
            IMP_LOG_ERR(TAG, "Private key does not match certificate");
            SSL_CTX_free(ctx);
            return -1;
        }
    }

    /* Store context */
    server->tls_context = ctx;

    // IMP_LOG_INFO(TAG, "OpenSSL TLS initialization completed for RTSPS server");
    return 0;
}

static void rtsp_server_tls_cleanup(rtsp_server_t* server)
{
    if (!server) {
        return;
    }

    if (server->tls_context) {
        SSL_CTX_free((SSL_CTX*)server->tls_context);
        server->tls_context = NULL;
    }
}

static int rtsp_client_tls_accept(rtsp_server_t* server, rtsp_client_t* client, int client_fd)
{
    if (!server || !client || !server->tls_context) {
        IMP_LOG_ERR(TAG, "Invalid parameters for TLS client accept");
        return -1;
    }

    SSL_CTX* ctx = (SSL_CTX*)server->tls_context;

    /* Create SSL connection */
    SSL* ssl = SSL_new(ctx);
    if (!ssl) {
        IMP_LOG_ERR(TAG, "Failed to create SSL connection for client");
        return -1;
    }

    /* Configure socket timeouts for sustained streaming */
    struct timeval timeout;
    timeout.tv_sec = 300;  /* 5 minutes timeout */
    timeout.tv_usec = 0;
    setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    /* Set socket */
    SSL_set_fd(ssl, client_fd);

    /* Perform TLS handshake */
    int ret = SSL_accept(ssl);
    if (ret <= 0) {
        int ssl_error = SSL_get_error(ssl, ret);
        IMP_LOG_ERR(TAG, "TLS handshake failed: %d (SSL error: %d)", ret, ssl_error);
        SSL_free(ssl);
        return -1;
    }

    /* Store SSL context in client */
    client->ssl_context = ssl;
    client->use_tls = true;

    // IMP_LOG_INFO(TAG, "TLS handshake completed for RTSPS client");
    return 0;
}

static void rtsp_client_tls_cleanup(rtsp_client_t* client)
{
    if (!client) {
        return;
    }

    if (client->ssl_context) {
        SSL* ssl = (SSL*)client->ssl_context;
        /* Additional safety check - ensure SSL context is valid before freeing */
        if (ssl != NULL && ssl != (SSL*)-1) {
            SSL_free(ssl);
        }
        client->ssl_context = NULL;
    }

    client->use_tls = false;
}

static int rtsp_client_tls_read(rtsp_client_t* client, char* buffer, size_t length)
{
    if (!client || !buffer || length == 0) {
        return -1;
    }

    if (!client->use_tls || !client->ssl_context) {
        return recv(client->socket_fd, buffer, length, 0);
    }

    SSL* ssl = (SSL*)client->ssl_context;
    int ret = SSL_read(ssl, buffer, length);
    if (ret <= 0) {
        int ssl_error = SSL_get_error(ssl, ret);
        if (ssl_error == SSL_ERROR_ZERO_RETURN) {
            IMP_LOG_DBG(TAG, "TLS connection closed by client");
            return 0;
        }
        IMP_LOG_ERR(TAG, "TLS read failed: %d (SSL error: %d)", ret, ssl_error);
        return -1;
    }

    return ret;
}

static int rtsp_client_tls_write(rtsp_client_t* client, const char* buffer, size_t length)
{
    if (!client || !buffer || length == 0) {
        return -1;
    }

    if (!client->use_tls || !client->ssl_context) {
        return send(client->socket_fd, buffer, length, 0);
    }

    /* Lock SSL mutex to prevent concurrent access */
    pthread_mutex_lock(&client->ssl_mutex);

    SSL* ssl = (SSL*)client->ssl_context;
    int final_result = -1;

    /* Retry loop for SSL_ERROR_WANT_READ/SSL_ERROR_WANT_WRITE */
    int retry_count = 0;
    const int max_retries = 3;

    while (retry_count < max_retries) {
        int ret = SSL_write(ssl, buffer, length);
        if (ret > 0) {
            final_result = ret; /* Success */
            break;
        }

        int ssl_error = SSL_get_error(ssl, ret);
        const char* error_str = "Unknown";

        switch (ssl_error) {
            case SSL_ERROR_WANT_READ:
                error_str = "Want read";
                /* SSL needs to read data before it can write - retry after short delay */
                pthread_mutex_unlock(&client->ssl_mutex);
                usleep(1000); /* 1ms delay */
                pthread_mutex_lock(&client->ssl_mutex);
                retry_count++;
                continue;

            case SSL_ERROR_WANT_WRITE:
                error_str = "Want write";
                /* SSL needs to write data before it can continue - retry after short delay */
                pthread_mutex_unlock(&client->ssl_mutex);
                usleep(1000); /* 1ms delay */
                pthread_mutex_lock(&client->ssl_mutex);
                retry_count++;
                continue;

            case SSL_ERROR_SYSCALL:
                error_str = "System call error";
                break;
            case SSL_ERROR_SSL:
                error_str = "SSL protocol error";
                break;
            case SSL_ERROR_ZERO_RETURN:
                error_str = "Connection closed";
                break;
        }

        IMP_LOG_ERR(TAG, "TLS write failed: %d (SSL error: %d - %s)", ret, ssl_error, error_str);

        /* If SSL context is corrupted, mark it as invalid immediately to prevent further use */
        if (ssl_error == SSL_ERROR_SSL || ssl_error == SSL_ERROR_SYSCALL) {
            IMP_LOG_WARN(TAG, "SSL context corrupted, marking as invalid");
            client->ssl_context = NULL; /* Mark as invalid to prevent cleanup issues */
            client->use_tls = false;
        }

        /* Don't mark client as inactive here - let the main loop handle cleanup */
        break;
    }

    if (final_result == -1 && retry_count >= max_retries) {
        IMP_LOG_WARN(TAG, "TLS write failed after %d retries", max_retries);
    }

    /* Unlock SSL mutex */
    pthread_mutex_unlock(&client->ssl_mutex);

    return final_result;
}

#elif defined(RTSPS_BACKEND_MBEDTLS)

/* mbedTLS implementation */

/* Generate a self-signed certificate at runtime if none exists */
static int generate_self_signed_certificate(const char* cert_file, const char* key_file)
{
    mbedtls_pk_context key;
    mbedtls_x509write_cert crt;
    mbedtls_mpi serial;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context ctr_drbg;

    const char* pers = "cert_gen";
    int ret = 0;
    FILE* f = NULL;
    unsigned char output_buf[4096];

    IMP_LOG_INFO(TAG, "Generating self-signed certificate for RTSPS");

    /* Create certificate directories if they don't exist */
    system("mkdir -p /etc/ssl/certs /etc/ssl/private");
    system("chmod 755 /etc/ssl/certs");
    system("chmod 700 /etc/ssl/private");

    /* Initialize contexts */
    mbedtls_pk_init(&key);
    mbedtls_x509write_crt_init(&crt);
    mbedtls_mpi_init(&serial);
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&ctr_drbg);

    /* Seed the random number generator */
    ret = mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy,
                               (const unsigned char*)pers, strlen(pers));
    if (ret != 0) {
        IMP_LOG_ERR(TAG, "mbedtls_ctr_drbg_seed failed: -0x%04x", -ret);
        goto cleanup;
    }

    /* Generate RSA key pair */
    IMP_LOG_INFO(TAG, "Generating RSA key pair (2048 bits)...");
    ret = mbedtls_pk_setup(&key, mbedtls_pk_info_from_type(MBEDTLS_PK_RSA));
    if (ret != 0) {
        IMP_LOG_ERR(TAG, "mbedtls_pk_setup failed: -0x%04x", -ret);
        goto cleanup;
    }

    ret = mbedtls_rsa_gen_key(mbedtls_pk_rsa(key), mbedtls_ctr_drbg_random, &ctr_drbg, 2048, 65537);
    if (ret != 0) {
        IMP_LOG_ERR(TAG, "mbedtls_rsa_gen_key failed: -0x%04x", -ret);
        goto cleanup;
    }

    /* Write private key to file */
    ret = mbedtls_pk_write_key_pem(&key, output_buf, sizeof(output_buf));
    if (ret != 0) {
        IMP_LOG_ERR(TAG, "mbedtls_pk_write_key_pem failed: -0x%04x", -ret);
        goto cleanup;
    }

    f = fopen(key_file, "w");
    if (f == NULL) {
        IMP_LOG_ERR(TAG, "Failed to create key file: %s", key_file);
        ret = -1;
        goto cleanup;
    }

    if (fwrite(output_buf, 1, strlen((char*)output_buf), f) != strlen((char*)output_buf)) {
        IMP_LOG_ERR(TAG, "Failed to write key file");
        ret = -1;
        goto cleanup;
    }

    fclose(f);
    f = NULL;

    /* Set certificate parameters */
    mbedtls_x509write_crt_set_subject_key(&crt, &key);
    mbedtls_x509write_crt_set_issuer_key(&crt, &key);

    ret = mbedtls_x509write_crt_set_subject_name(&crt, "CN=camera.local,O=Thingino,C=US");
    if (ret != 0) {
        IMP_LOG_ERR(TAG, "mbedtls_x509write_crt_set_subject_name failed: -0x%04x", -ret);
        goto cleanup;
    }

    ret = mbedtls_x509write_crt_set_issuer_name(&crt, "CN=camera.local,O=Thingino,C=US");
    if (ret != 0) {
        IMP_LOG_ERR(TAG, "mbedtls_x509write_crt_set_issuer_name failed: -0x%04x", -ret);
        goto cleanup;
    }

    ret = mbedtls_mpi_lset(&serial, 1);
    if (ret != 0) {
        IMP_LOG_ERR(TAG, "mbedtls_mpi_lset failed: -0x%04x", -ret);
        goto cleanup;
    }

    ret = mbedtls_x509write_crt_set_serial(&crt, &serial);
    if (ret != 0) {
        IMP_LOG_ERR(TAG, "mbedtls_x509write_crt_set_serial failed: -0x%04x", -ret);
        goto cleanup;
    }

    ret = mbedtls_x509write_crt_set_validity(&crt, "20240101000000", "20340101000000");
    if (ret != 0) {
        IMP_LOG_ERR(TAG, "mbedtls_x509write_crt_set_validity failed: -0x%04x", -ret);
        goto cleanup;
    }

    mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);

    /* Write certificate to buffer */
    ret = mbedtls_x509write_crt_pem(&crt, output_buf, sizeof(output_buf), mbedtls_ctr_drbg_random, &ctr_drbg);
    if (ret < 0) {
        IMP_LOG_ERR(TAG, "mbedtls_x509write_crt_pem failed: -0x%04x", -ret);
        goto cleanup;
    }

    /* Write certificate to file */
    f = fopen(cert_file, "w");
    if (f == NULL) {
        IMP_LOG_ERR(TAG, "Failed to create certificate file: %s", cert_file);
        ret = -1;
        goto cleanup;
    }

    if (fwrite(output_buf, 1, strlen((char*)output_buf), f) != strlen((char*)output_buf)) {
        IMP_LOG_ERR(TAG, "Failed to write certificate file");
        ret = -1;
        goto cleanup;
    }

    fclose(f);
    f = NULL;

    IMP_LOG_INFO(TAG, "Self-signed certificate generated successfully");
    IMP_LOG_INFO(TAG, "Certificate: %s", cert_file);
    IMP_LOG_INFO(TAG, "Private key: %s", key_file);

    ret = 0;

cleanup:
    if (f) fclose(f);
    mbedtls_pk_free(&key);
    mbedtls_x509write_crt_free(&crt);
    mbedtls_mpi_free(&serial);
    mbedtls_entropy_free(&entropy);
    mbedtls_ctr_drbg_free(&ctr_drbg);

    return ret;
}
static int rtsp_server_tls_init(rtsp_server_t* server)
{
    if (!server || !server->config.tls_enabled) {
        return 0; /* TLS not enabled */
    }

    IMP_LOG_INFO(TAG, "Initializing mbedTLS for RTSPS server");
    IMP_LOG_DBG(TAG, "Certificate file: %s", server->config.cert_file);
    IMP_LOG_DBG(TAG, "Private key file: %s", server->config.key_file);

    /* Allocate mbedTLS contexts */
    server->tls_context = malloc(sizeof(mbedtls_ssl_config));
    server->tls_entropy = malloc(sizeof(mbedtls_entropy_context));
    server->tls_ctr_drbg = malloc(sizeof(mbedtls_ctr_drbg_context));
    server->tls_cert_context = malloc(sizeof(mbedtls_x509_crt));
    server->tls_key_context = malloc(sizeof(mbedtls_pk_context));

    if (!server->tls_context || !server->tls_entropy || !server->tls_ctr_drbg ||
        !server->tls_cert_context || !server->tls_key_context) {
        IMP_LOG_ERR(TAG, "Failed to allocate mbedTLS contexts");
        rtsp_server_tls_cleanup(server);
        return -1;
    }

    mbedtls_ssl_config* conf = (mbedtls_ssl_config*)server->tls_context;
    mbedtls_entropy_context* entropy = (mbedtls_entropy_context*)server->tls_entropy;
    mbedtls_ctr_drbg_context* ctr_drbg = (mbedtls_ctr_drbg_context*)server->tls_ctr_drbg;
    mbedtls_x509_crt* cert = (mbedtls_x509_crt*)server->tls_cert_context;
    mbedtls_pk_context* key = (mbedtls_pk_context*)server->tls_key_context;

    /* Initialize contexts */
    mbedtls_ssl_config_init(conf);
    mbedtls_entropy_init(entropy);
    mbedtls_ctr_drbg_init(ctr_drbg);
    mbedtls_x509_crt_init(cert);
    mbedtls_pk_init(key);

    /* Seed the random number generator */
    const char* pers = "rtsp_server";
    int ret = mbedtls_ctr_drbg_seed(ctr_drbg, mbedtls_entropy_func, entropy,
                                   (const unsigned char*)pers, strlen(pers));
    if (ret != 0) {
        IMP_LOG_ERR(TAG, "mbedtls_ctr_drbg_seed failed: -0x%04x", -ret);
        rtsp_server_tls_cleanup(server);
        return -1;
    }

    /* Setup SSL configuration */
    ret = mbedtls_ssl_config_defaults(conf, MBEDTLS_SSL_IS_SERVER,
                                     MBEDTLS_SSL_TRANSPORT_STREAM,
                                     MBEDTLS_SSL_PRESET_DEFAULT);
    if (ret != 0) {
        IMP_LOG_ERR(TAG, "mbedtls_ssl_config_defaults failed: -0x%04x", -ret);
        rtsp_server_tls_cleanup(server);
        return -1;
    }

    /* Configure certificate verification */
    if (server->config.tls_verify_client) {
        mbedtls_ssl_conf_authmode(conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    } else {
        mbedtls_ssl_conf_authmode(conf, MBEDTLS_SSL_VERIFY_NONE);
    }

    mbedtls_ssl_conf_rng(conf, mbedtls_ctr_drbg_random, ctr_drbg);

    /* Configure supported elliptic curves for EC certificate compatibility */
    static const mbedtls_ecp_group_id curves[] = {
        MBEDTLS_ECP_DP_SECP256R1,    /* P-256 */
        MBEDTLS_ECP_DP_SECP384R1,    /* P-384 */
        MBEDTLS_ECP_DP_SECP521R1,    /* P-521 */
        MBEDTLS_ECP_DP_NONE
    };
    mbedtls_ssl_conf_curves(conf, curves);

    /* Configure signature algorithms to support ECDSA */
    static const int sig_hashes[] = {
        MBEDTLS_MD_SHA256,
        MBEDTLS_MD_SHA384,
        MBEDTLS_MD_SHA512,
        MBEDTLS_MD_SHA1,
        MBEDTLS_MD_NONE
    };
    mbedtls_ssl_conf_sig_hashes(conf, sig_hashes);

    /* Load certificate and key if provided */
    if (strlen(server->config.cert_file) > 0 && strlen(server->config.key_file) > 0) {
        IMP_LOG_INFO(TAG, "Loading certificate: %s", server->config.cert_file);
        IMP_LOG_INFO(TAG, "Loading private key: %s", server->config.key_file);

        ret = mbedtls_x509_crt_parse_file(cert, server->config.cert_file);
        if (ret != 0) {
            IMP_LOG_WARN(TAG, "Certificate file not found or invalid (%s): -0x%04x", server->config.cert_file, -ret);
            IMP_LOG_ERR(TAG, "Certificate loading failed - this may cause TLS handshake failures");

            /* Try to generate self-signed certificate as fallback */
            IMP_LOG_INFO(TAG, "Attempting to generate self-signed certificate...");
            if (generate_self_signed_certificate(server->config.cert_file, server->config.key_file) == 0) {
                /* Try loading the generated certificate */
                ret = mbedtls_x509_crt_parse_file(cert, server->config.cert_file);
                if (ret != 0) {
                    IMP_LOG_WARN(TAG, "Failed to load generated certificate: -0x%04x", -ret);
                } else {
                    ret = mbedtls_pk_parse_keyfile(key, server->config.key_file, NULL, mbedtls_ctr_drbg_random, ctr_drbg);
                    if (ret != 0) {
                        IMP_LOG_WARN(TAG, "Failed to load generated private key: -0x%04x", -ret);
                    } else {
                        ret = mbedtls_ssl_conf_own_cert(conf, cert, key);
                        if (ret != 0) {
                            IMP_LOG_WARN(TAG, "Failed to configure generated certificate: -0x%04x", -ret);
                        } else {
                            IMP_LOG_INFO(TAG, "Generated certificate loaded successfully");
                        }
                    }
                }
            } else {
                IMP_LOG_WARN(TAG, "Failed to generate self-signed certificate");
            }
        } else {
            IMP_LOG_INFO(TAG, "Certificate file loaded successfully");
            ret = mbedtls_pk_parse_keyfile(key, server->config.key_file, NULL, mbedtls_ctr_drbg_random, ctr_drbg);
            if (ret != 0) {
                IMP_LOG_WARN(TAG, "Private key file not found or invalid (%s): -0x%04x", server->config.key_file, -ret);
                IMP_LOG_ERR(TAG, "Private key loading failed - this may cause TLS handshake failures");
            } else {
                ret = mbedtls_ssl_conf_own_cert(conf, cert, key);
                if (ret != 0) {
                    IMP_LOG_WARN(TAG, "Failed to configure certificate: -0x%04x", -ret);
                } else {
                    IMP_LOG_INFO(TAG, "Certificate and key configured successfully");
                }
            }
        }
    } else {
        IMP_LOG_WARN(TAG, "No certificate/key files specified for RTSPS - TLS will work but clients may show warnings");
    }

    IMP_LOG_INFO(TAG, "mbedTLS server initialization completed");
    return 0;
}

static void rtsp_server_tls_cleanup(rtsp_server_t* server)
{
    if (!server) {
        return;
    }

    if (server->tls_context) {
        mbedtls_ssl_config_free((mbedtls_ssl_config*)server->tls_context);
        free(server->tls_context);
        server->tls_context = NULL;
    }

    if (server->tls_entropy) {
        mbedtls_entropy_free((mbedtls_entropy_context*)server->tls_entropy);
        free(server->tls_entropy);
        server->tls_entropy = NULL;
    }

    if (server->tls_ctr_drbg) {
        mbedtls_ctr_drbg_free((mbedtls_ctr_drbg_context*)server->tls_ctr_drbg);
        free(server->tls_ctr_drbg);
        server->tls_ctr_drbg = NULL;
    }

    if (server->tls_cert_context) {
        mbedtls_x509_crt_free((mbedtls_x509_crt*)server->tls_cert_context);
        free(server->tls_cert_context);
        server->tls_cert_context = NULL;
    }

    if (server->tls_key_context) {
        mbedtls_pk_free((mbedtls_pk_context*)server->tls_key_context);
        free(server->tls_key_context);
        server->tls_key_context = NULL;
    }
}

static int rtsp_client_tls_accept(rtsp_server_t* server, rtsp_client_t* client, int client_fd)
{
    if (!server || !client || client_fd < 0) {
        IMP_LOG_ERR(TAG, "Invalid parameters for TLS client accept");
        return -1;
    }

    if (!server->tls_context) {
        IMP_LOG_ERR(TAG, "Server TLS not initialized");
        return -1;
    }

    IMP_LOG_DBG(TAG, "TLS accept: server=%p, client=%p, client_fd=%d", server, client, client_fd);

    /* Allocate client SSL context */
    client->ssl_context = malloc(sizeof(mbedtls_ssl_context));
    if (!client->ssl_context) {
        IMP_LOG_ERR(TAG, "Failed to allocate client SSL context");
        return -1;
    }

    IMP_LOG_DBG(TAG, "TLS accept: allocated client SSL context at %p", client->ssl_context);

    mbedtls_ssl_context* ssl = (mbedtls_ssl_context*)client->ssl_context;
    mbedtls_ssl_config* conf = (mbedtls_ssl_config*)server->tls_context;

    IMP_LOG_DBG(TAG, "TLS accept: ssl=%p, conf=%p", ssl, conf);

    /* Initialize client SSL context */
    IMP_LOG_DBG(TAG, "TLS accept: calling mbedtls_ssl_init");
    mbedtls_ssl_init(ssl);
    IMP_LOG_DBG(TAG, "TLS accept: mbedtls_ssl_init completed");

    /* Setup SSL context with server configuration */
    IMP_LOG_DBG(TAG, "TLS accept: calling mbedtls_ssl_setup");
    int ret = mbedtls_ssl_setup(ssl, conf);
    if (ret != 0) {
        IMP_LOG_ERR(TAG, "mbedtls_ssl_setup failed: -0x%04x", -ret);
        mbedtls_ssl_free(ssl);
        free(client->ssl_context);
        client->ssl_context = NULL;
        return -1;
    }
    IMP_LOG_DBG(TAG, "TLS accept: mbedtls_ssl_setup completed");

    /* Set BIO callbacks */
    IMP_LOG_DBG(TAG, "TLS accept: calling mbedtls_ssl_set_bio with client_fd=%d", client_fd);
    mbedtls_ssl_set_bio(ssl, &client_fd, mbedtls_net_send, mbedtls_net_recv, NULL);
    IMP_LOG_DBG(TAG, "TLS accept: mbedtls_ssl_set_bio completed");

    /* Perform TLS handshake */
    IMP_LOG_DBG(TAG, "TLS accept: starting TLS handshake");
    while ((ret = mbedtls_ssl_handshake(ssl)) != 0) {
        IMP_LOG_DBG(TAG, "TLS accept: handshake returned %d (-0x%04x)", ret, -ret);
        if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
            char error_buf[100];
            mbedtls_strerror(ret, error_buf, sizeof(error_buf));
            IMP_LOG_ERR(TAG, "mbedTLS handshake failed: -0x%04x (%s) for client fd=%d", -ret, error_buf, client_fd);

            /* Log additional context */
            IMP_LOG_ERR(TAG, "TLS handshake context: client_fd=%d, ssl_context=%p", client_fd, ssl);

            mbedtls_ssl_free(ssl);
            free(client->ssl_context);
            client->ssl_context = NULL;
            return -1;
        }
    }

    client->use_tls = true;
    IMP_LOG_INFO(TAG, "TLS handshake completed for RTSPS client");
    return 0;
}

static void rtsp_client_tls_cleanup(rtsp_client_t* client)
{
    if (!client || !client->ssl_context) {
        return;
    }

    mbedtls_ssl_free((mbedtls_ssl_context*)client->ssl_context);
    free(client->ssl_context);
    client->ssl_context = NULL;
    client->use_tls = false;
}

static int rtsp_client_tls_read(rtsp_client_t* client, char* buffer, size_t length)
{
    if (!client || !buffer || length == 0) {
        return -1;
    }

    if (!client->use_tls || !client->ssl_context) {
        return recv(client->socket_fd, buffer, length, 0);
    }

    mbedtls_ssl_context* ssl = (mbedtls_ssl_context*)client->ssl_context;
    int ret = mbedtls_ssl_read(ssl, (unsigned char*)buffer, length);

    if (ret < 0) {
        if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
            errno = EAGAIN;
            return -1;
        }
        if (ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
            IMP_LOG_DBG(TAG, "TLS connection closed by client");
            return 0;
        }
        IMP_LOG_ERR(TAG, "TLS read failed: -0x%04x", -ret);
        return -1;
    }

    return ret;
}

static int rtsp_client_tls_write(rtsp_client_t* client, const char* buffer, size_t length)
{
    if (!client || !buffer || length == 0) {
        return -1;
    }

    if (!client->use_tls || !client->ssl_context) {
        return send(client->socket_fd, buffer, length, 0);
    }

    mbedtls_ssl_context* ssl = (mbedtls_ssl_context*)client->ssl_context;
    size_t bytes_written = 0;

    while (bytes_written < length) {
        int ret = mbedtls_ssl_write(ssl, (const unsigned char*)(buffer + bytes_written),
                                   length - bytes_written);
        if (ret < 0) {
            if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
                continue;
            }
            IMP_LOG_ERR(TAG, "TLS write failed: -0x%04x", -ret);
            return -1;
        }
        bytes_written += ret;
    }

    return bytes_written;
}

#else

/* No TLS backend selected */
static int rtsp_server_tls_init(rtsp_server_t* server)
{
    if (server && server->config.tls_enabled) {
        IMP_LOG_ERR(TAG, "TLS support not compiled - cannot enable RTSPS");
        return -1;
    }
    return 0; /* TLS not enabled */
}

static void rtsp_server_tls_cleanup(rtsp_server_t* server)
{
    /* Nothing to cleanup */
}

static int rtsp_client_tls_accept(rtsp_server_t* server, rtsp_client_t* client, int client_fd)
{
    IMP_LOG_ERR(TAG, "TLS support not compiled - cannot accept RTSPS client");
    return -1;
}

static void rtsp_client_tls_cleanup(rtsp_client_t* client)
{
    /* Nothing to cleanup */
}

static int rtsp_client_tls_read(rtsp_client_t* client, char* buffer, size_t length)
{
    return recv(client->socket_fd, buffer, length, 0);
}

static int rtsp_client_tls_write(rtsp_client_t* client, const char* buffer, size_t length)
{
    return send(client->socket_fd, buffer, length, 0);
}

#endif
