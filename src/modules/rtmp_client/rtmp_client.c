/*
 * rtmp_client.c - RTMP Client Module Implementation
 * Modular RTMP client for Thingino Streamer
 * Supports multiple concurrent RTMP connections with automatic reconnection
 * Also supports RTMPS (RTMP over TLS) for secure connections
 * Copyright (C) 2025 Paul Philippov, Thingino Project
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <imp/imp_common.h>
#include <imp/imp_encoder.h>
#include <json-c/json.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>

/* TLS includes for RTMPS support */
#ifdef RTMPS_BACKEND_OPENSSL
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/rand.h>
#elif defined(RTMPS_BACKEND_MBEDTLS)
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/error.h>
#endif

#include "rtmp_client.h"
#include "../../common.h"
#include "../../config.h"
#include "../../frame_manager.h"
#include "../rtsp/rtsp_server.h"

/* External declarations */
extern struct chn_conf chn[FS_CHN_NUM];

#define TAG "RTMP_CLIENT"

/* Global module state */
static struct {
    bool initialized;
    bool running;
    rtmp_client_config_t config;
    struct rtsp_server* rtsp_server;  /* Reference to RTSP server */
} g_rtmp_client_module_state = {0};

/* Global RTMP client instance */
static rtmp_client_t* g_rtmp_client = NULL;

/* Forward declarations */
static void* rtmp_client_manager_thread(void* arg);
static void* rtmp_client_connection_thread(void* arg);
static int rtmp_client_create(rtmp_client_config_t* config);
static void rtmp_client_destroy(void);
static int rtmp_client_parse_url(const char* url, char* host, int* port, char* app, char* stream, bool* use_tls);

/* Helper functions for socket I/O */
static int rtmp_client_write_bytes(int socket_fd, const uint8_t* buffer, size_t length);
static int rtmp_client_read_bytes(int socket_fd, uint8_t* buffer, size_t length);

/* TLS-aware I/O functions */
#if defined(RTMPS_BACKEND_OPENSSL) || defined(RTMPS_BACKEND_MBEDTLS)
static int rtmp_client_tls_write_bytes(rtmp_client_connection_t* conn, const uint8_t* buffer, size_t length);
static int rtmp_client_tls_read_bytes(rtmp_client_connection_t* conn, uint8_t* buffer, size_t length);
static int rtmp_client_tls_setup(rtmp_client_connection_t* conn, const char* hostname);
static int rtmp_client_tls_handshake(rtmp_client_connection_t* conn);
static void rtmp_client_tls_cleanup(rtmp_client_connection_t* conn);
#endif

/* Connection-aware I/O wrappers */
static int rtmp_client_connection_write_bytes(rtmp_client_connection_t* conn, const uint8_t* buffer, size_t length);
static int rtmp_client_connection_read_bytes(rtmp_client_connection_t* conn, uint8_t* buffer, size_t length);

/* TLS-aware I/O functions */
static int rtmp_client_tls_write_bytes(rtmp_client_connection_t* conn, const uint8_t* buffer, size_t length);
static int rtmp_client_tls_read_bytes(rtmp_client_connection_t* conn, uint8_t* buffer, size_t length);
static int rtmp_client_tls_setup(rtmp_client_connection_t* conn, const char* hostname);
static void rtmp_client_tls_cleanup(rtmp_client_connection_t* conn);

/* Module lifecycle functions */
int rtmp_client_module_init(void* config)
{
    IMP_LOG_INFO(TAG, "Initializing RTMP client module");

    if (g_rtmp_client_module_state.initialized) {
        IMP_LOG_WARN(TAG, "RTMP client module already initialized");
        return 0;
    }

    if (!config) {
        IMP_LOG_ERR(TAG, "Invalid configuration provided");
        return -1;
    }

    /* Copy configuration */
    memcpy(&g_rtmp_client_module_state.config, config, sizeof(rtmp_client_config_t));

    /* Validate configuration */
    if (rtmp_client_module_config_validate(&g_rtmp_client_module_state.config) != 0) {
        IMP_LOG_ERR(TAG, "Invalid RTMP client module configuration");
        return -1;
    }

    g_rtmp_client_module_state.initialized = true;
    IMP_LOG_INFO(TAG, "RTMP client module initialized successfully");
    return 0;
}

int rtmp_client_module_start(void)
{
    IMP_LOG_INFO(TAG, "Starting RTMP client module");

    if (!g_rtmp_client_module_state.initialized) {
        IMP_LOG_ERR(TAG, "RTMP client module not initialized");
        return -1;
    }

    if (g_rtmp_client_module_state.running) {
        IMP_LOG_WARN(TAG, "RTMP client module already running");
        return 0;
    }

    if (!g_rtmp_client_module_state.config.enabled) {
        IMP_LOG_INFO(TAG, "RTMP client module disabled in configuration");
        return 0;
    }

    /* Create RTMP client */
    if (rtmp_client_create(&g_rtmp_client_module_state.config) != 0) {
        IMP_LOG_ERR(TAG, "Failed to create RTMP client");
        return -1;
    }

    g_rtmp_client_module_state.running = true;
    IMP_LOG_INFO(TAG, "RTMP client module started successfully with %d targets",
                 g_rtmp_client_module_state.config.target_count);
    return 0;
}

int rtmp_client_module_stop(void)
{
    IMP_LOG_INFO(TAG, "Stopping RTMP client module");

    if (!g_rtmp_client_module_state.running) {
        IMP_LOG_WARN(TAG, "RTMP client module not running");
        return 0;
    }

    /* Destroy RTMP client */
    rtmp_client_destroy();

    g_rtmp_client_module_state.running = false;
    IMP_LOG_INFO(TAG, "RTMP client module stopped successfully");
    return 0;
}

int rtmp_client_module_cleanup(void)
{
    IMP_LOG_INFO(TAG, "Cleaning up RTMP client module");

    if (g_rtmp_client_module_state.running) {
        rtmp_client_module_stop();
    }

    if (!g_rtmp_client_module_state.initialized) {
        return 0;
    }

    /* Free target configurations */
    if (g_rtmp_client_module_state.config.targets) {
        free(g_rtmp_client_module_state.config.targets);
        g_rtmp_client_module_state.config.targets = NULL;
    }

    /* Reset state */
    memset(&g_rtmp_client_module_state, 0, sizeof(g_rtmp_client_module_state));

    IMP_LOG_INFO(TAG, "RTMP client module cleaned up successfully");
    return 0;
}

int rtmp_client_module_get_config_size(void)
{
    return sizeof(rtmp_client_config_t);
}

/* Configuration functions */
int rtmp_client_module_config_parse(json_object* json, void* config)
{
    if (!json || !config) {
        IMP_LOG_ERR(TAG, "Invalid parameters for config parsing");
        return -1;
    }

    rtmp_client_config_t* client_config = (rtmp_client_config_t*)config;

    /* JSON root is the rtmp_client config directly (no wrapper) */
    json_object* rtmp_client_obj = json;

    /* Parse enabled flag */
    json_object* enabled_obj;
    if (json_object_object_get_ex(rtmp_client_obj, "enabled", &enabled_obj)) {
        client_config->enabled = json_object_get_boolean(enabled_obj);
    }

    /* Parse streams array */
    json_object* streams_obj;
    if (json_object_object_get_ex(rtmp_client_obj, "streams", &streams_obj)) {
        if (json_object_is_type(streams_obj, json_type_array)) {
            int stream_count = json_object_array_length(streams_obj);
            if (stream_count > 0) {
                client_config->targets = malloc(stream_count * sizeof(rtmp_stream_target_t));
                if (!client_config->targets) {
                    IMP_LOG_ERR(TAG, "Failed to allocate memory for stream targets");
                    return -1;
                }

                client_config->target_count = 0;
                for (int i = 0; i < stream_count; i++) {
                    json_object* stream_obj = json_object_array_get_idx(streams_obj, i);
                    if (stream_obj) {
                        rtmp_stream_target_t* target = &client_config->targets[client_config->target_count];
                        memset(target, 0, sizeof(rtmp_stream_target_t));

                        /* Parse stream target properties */
                        json_object* name_obj;
                        if (json_object_object_get_ex(stream_obj, "name", &name_obj)) {
                            strncpy(target->name, json_object_get_string(name_obj), sizeof(target->name) - 1);
                        }

                        json_object* enabled_obj;
                        if (json_object_object_get_ex(stream_obj, "enabled", &enabled_obj)) {
                            target->enabled = json_object_get_boolean(enabled_obj);
                        }

                        json_object* url_obj;
                        if (json_object_object_get_ex(stream_obj, "url", &url_obj)) {
                            strncpy(target->url, json_object_get_string(url_obj), sizeof(target->url) - 1);
                        }

                        json_object* stream_key_obj;
                        if (json_object_object_get_ex(stream_obj, "stream_key", &stream_key_obj)) {
                            strncpy(target->stream_key, json_object_get_string(stream_key_obj), sizeof(target->stream_key) - 1);
                        }

                        json_object* retry_interval_obj;
                        if (json_object_object_get_ex(stream_obj, "retry_interval", &retry_interval_obj)) {
                            target->retry_interval = json_object_get_int(retry_interval_obj);
                        }

                        json_object* max_retries_obj;
                        if (json_object_object_get_ex(stream_obj, "max_retries", &max_retries_obj)) {
                            target->max_retries = json_object_get_int(max_retries_obj);
                        }

                        client_config->target_count++;
                    }
                }
            }
        }
    }

    /* Parse video settings */
    json_object* video_obj;
    if (json_object_object_get_ex(rtmp_client_obj, "video", &video_obj)) {
        json_object* channel_obj;
        if (json_object_object_get_ex(video_obj, "channel", &channel_obj)) {
            client_config->video.channel = json_object_get_int(channel_obj);
            IMP_LOG_INFO(TAG, "RTMP video channel configured: %d", client_config->video.channel);
        }

        json_object* bitrate_limit_obj;
        if (json_object_object_get_ex(video_obj, "bitrate_limit", &bitrate_limit_obj)) {
            client_config->video.bitrate_limit = json_object_get_int(bitrate_limit_obj);
        }

        json_object* fps_limit_obj;
        if (json_object_object_get_ex(video_obj, "fps_limit", &fps_limit_obj)) {
            client_config->video.fps_limit = json_object_get_int(fps_limit_obj);
        }
    }

    /* Parse connection settings */
    json_object* connection_obj;
    if (json_object_object_get_ex(rtmp_client_obj, "connection", &connection_obj)) {
        json_object* timeout_obj;
        if (json_object_object_get_ex(connection_obj, "timeout", &timeout_obj)) {
            client_config->connection.timeout = json_object_get_int(timeout_obj);
        }

        json_object* chunk_size_obj;
        if (json_object_object_get_ex(connection_obj, "chunk_size", &chunk_size_obj)) {
            client_config->connection.chunk_size = json_object_get_int(chunk_size_obj);
        }
    }

    IMP_LOG_DBG(TAG, "RTMP client config parsed: enabled=%s, targets=%d",
                client_config->enabled ? "true" : "false",
                client_config->target_count);

    return 0;
}

int rtmp_client_module_config_validate(void* config)
{
    if (!config) {
        IMP_LOG_ERR(TAG, "Invalid configuration pointer");
        return -1;
    }

    rtmp_client_config_t* client_config = (rtmp_client_config_t*)config;

    /* Validate video channel - must be explicitly configured */
    if (client_config->video.channel < 0 || client_config->video.channel >= FS_CHN_NUM) {
        IMP_LOG_ERR(TAG, "Video channel not configured or invalid: %d (must be 0-%d)",
                   client_config->video.channel, FS_CHN_NUM - 1);
        return -1;
    }

    /* Validate video bitrate limit - must be configured */
    if (client_config->video.bitrate_limit <= 0) {
        IMP_LOG_ERR(TAG, "Video bitrate_limit not configured or invalid: %d (must be > 0)",
                   client_config->video.bitrate_limit);
        return -1;
    }

    /* Validate video fps limit - must be configured */
    if (client_config->video.fps_limit <= 0) {
        IMP_LOG_ERR(TAG, "Video fps_limit not configured or invalid: %d (must be > 0)",
                   client_config->video.fps_limit);
        return -1;
    }

    /* Validate connection settings */
    if (client_config->connection.timeout < 5 || client_config->connection.timeout > 300) {
        IMP_LOG_ERR(TAG, "Invalid connection timeout: %d", client_config->connection.timeout);
        return -1;
    }

    if (client_config->connection.chunk_size < 128 || client_config->connection.chunk_size > 65536) {
        IMP_LOG_ERR(TAG, "Invalid chunk size: %d (valid range: 128-65536)", client_config->connection.chunk_size);
        return -1;
    }

    /* Validate stream targets */
    for (int i = 0; i < client_config->target_count; i++) {
        rtmp_stream_target_t* target = &client_config->targets[i];

        if (strlen(target->name) == 0) {
            IMP_LOG_ERR(TAG, "Stream target %d has empty name", i);
            return -1;
        }

        if (strlen(target->url) == 0) {
            IMP_LOG_ERR(TAG, "Stream target %s has empty URL", target->name);
            return -1;
        }

        if (target->retry_interval < 5 || target->retry_interval > 300) {
            IMP_LOG_ERR(TAG, "Invalid retry interval for target %s: %d", target->name, target->retry_interval);
            return -1;
        }

        if (target->max_retries < 0 || target->max_retries > 100) {
            IMP_LOG_ERR(TAG, "Invalid max retries for target %s: %d", target->name, target->max_retries);
            return -1;
        }
    }

    return 0;
}

int rtmp_client_module_set_defaults(void* config)
{
    if (!config) {
        IMP_LOG_ERR(TAG, "Invalid configuration pointer");
        return -1;
    }

    rtmp_client_config_t* client_config = (rtmp_client_config_t*)config;
    memset(client_config, 0, sizeof(rtmp_client_config_t));

    /* Initialize to invalid values - all settings must come from config */
    client_config->enabled = false;
    client_config->targets = NULL;
    client_config->target_count = 0;

    /* Video settings - NO DEFAULTS, must be configured */
    client_config->video.channel = -1;        /* Invalid - must be set in config */
    client_config->video.bitrate_limit = 0;   /* Invalid - must be set in config */
    client_config->video.fps_limit = 0;       /* Invalid - must be set in config */

    /* Connection settings - minimal required defaults only */
    client_config->connection.timeout = 30;
    client_config->connection.chunk_size = RTMP_DEFAULT_CHUNK_SIZE;
    client_config->connection.keepalive_interval = 60;

    return 0;
}

/* RTSP server integration */
int rtmp_client_module_set_rtsp_server(struct rtsp_server* server)
{
    g_rtmp_client_module_state.rtsp_server = server;
    IMP_LOG_INFO(TAG, "RTSP server reference set for RTMP client module");
    return 0;
}

/* RTMP client access */
rtmp_client_t* rtmp_client_module_get_client(void)
{
    return g_rtmp_client;
}

/* Module registration function */
int register_rtmp_client_module(void)
{
    return module_register(&rtmp_client_module_info);
}

/* Module registration - following the established pattern */
module_info_t rtmp_client_module_info = {
    .name = RTMP_CLIENT_MODULE_NAME,
    .version = RTMP_CLIENT_MODULE_VERSION,
    .description = "Real Time Messaging Protocol (RTMP) client for live streaming to platforms",
    .state = MODULE_STATE_UNREGISTERED,
    .module_data = &g_rtmp_client_module_state,

    /* Lifecycle callbacks */
    .init = rtmp_client_module_init,
    .start = rtmp_client_module_start,
    .stop = rtmp_client_module_stop,
    .cleanup = rtmp_client_module_cleanup,

    /* Configuration */
    .config_size = sizeof(rtmp_client_config_t),
    .config_parse = rtmp_client_module_config_parse,
    .config_validate = rtmp_client_module_config_validate,

    /* RTSP integration - RTMP client receives frames via RTSP frame callback */
    .rtsp_setup = NULL,
    .rtsp_frame_callback = rtmp_client_module_rtsp_frame_callback,
    .rtsp_cleanup = NULL,

    /* Statistics - not implemented yet */
    .get_stats = NULL
};

/* RTMP client implementation */
static int rtmp_client_create(rtmp_client_config_t* config)
{
    if (g_rtmp_client) {
        IMP_LOG_WARN(TAG, "RTMP client already exists");
        return 0;
    }

    /* Allocate client structure */
    g_rtmp_client = malloc(sizeof(rtmp_client_t));
    if (!g_rtmp_client) {
        IMP_LOG_ERR(TAG, "Failed to allocate RTMP client");
        return -1;
    }

    memset(g_rtmp_client, 0, sizeof(rtmp_client_t));

    /* Copy configuration */
    memcpy(&g_rtmp_client->config, config, sizeof(rtmp_client_config_t));

    /* Initialize mutex */
    if (pthread_mutex_init(&g_rtmp_client->connections_mutex, NULL) != 0) {
        IMP_LOG_ERR(TAG, "Failed to initialize connections mutex");
        free(g_rtmp_client);
        g_rtmp_client = NULL;
        return -1;
    }

    /* Start manager thread */
    g_rtmp_client->running = true;
    if (pthread_create(&g_rtmp_client->manager_thread, NULL, rtmp_client_manager_thread, g_rtmp_client) != 0) {
        IMP_LOG_ERR(TAG, "Failed to create RTMP client manager thread");
        pthread_mutex_destroy(&g_rtmp_client->connections_mutex);
        free(g_rtmp_client);
        g_rtmp_client = NULL;
        return -1;
    }

    IMP_LOG_INFO(TAG, "RTMP client created successfully");
    return 0;
}

static void rtmp_client_destroy(void)
{
    if (!g_rtmp_client) {
        return;
    }

    IMP_LOG_INFO(TAG, "Destroying RTMP client");

    /* Stop manager thread */
    g_rtmp_client->running = false;
    pthread_join(g_rtmp_client->manager_thread, NULL);

    /* Stop all connections */
    pthread_mutex_lock(&g_rtmp_client->connections_mutex);
    rtmp_client_connection_t* conn = g_rtmp_client->connections;
    while (conn) {
        rtmp_client_connection_t* next = conn->next;
        rtmp_client_connection_stop(conn);
        rtmp_client_connection_destroy(conn);
        conn = next;
    }
    g_rtmp_client->connections = NULL;
    pthread_mutex_unlock(&g_rtmp_client->connections_mutex);

    /* Cleanup */
    pthread_mutex_destroy(&g_rtmp_client->connections_mutex);
    free(g_rtmp_client);
    g_rtmp_client = NULL;

    IMP_LOG_INFO(TAG, "RTMP client destroyed");
}

/* RTMP client manager thread */
static void* rtmp_client_manager_thread(void* arg)
{
    rtmp_client_t* client = (rtmp_client_t*)arg;

    IMP_LOG_INFO(TAG, "RTMP client manager thread started");

    while (client->running) {
        /* Check and manage connections */
        pthread_mutex_lock(&client->connections_mutex);

        /* Create connections for enabled targets that don't have active connections */
        for (int i = 0; i < client->config.target_count; i++) {
            rtmp_stream_target_t* target = &client->config.targets[i];

            if (!target->enabled) {
                continue;
            }

            /* Check if connection already exists */
            bool connection_exists = false;
            rtmp_client_connection_t* conn = client->connections;
            while (conn) {
                if (strcmp(conn->name, target->name) == 0) {
                    connection_exists = true;
                    break;
                }
                conn = conn->next;
            }

            /* Create new connection if needed */
            if (!connection_exists) {
                rtmp_client_connection_t* new_conn;
                if (rtmp_client_connection_create(target, &new_conn) == 0) {
                    /* Add to connection list */
                    new_conn->next = client->connections;
                    client->connections = new_conn;

                    /* Start connection */
                    rtmp_client_connection_start(new_conn);

                    IMP_LOG_INFO(TAG, "Created RTMP connection for target: %s", target->name);
                }
            }
        }

        pthread_mutex_unlock(&client->connections_mutex);

        /* Sleep for a while before next check */
        sleep(5);
    }

    IMP_LOG_INFO(TAG, "RTMP client manager thread finished");
    return NULL;
}

/* RTMP client connection implementation */
int rtmp_client_connection_create(const rtmp_stream_target_t* target, rtmp_client_connection_t** conn)
{
    if (!target || !conn) {
        IMP_LOG_ERR(TAG, "Invalid parameters for connection creation");
        return -1;
    }

    IMP_LOG_INFO(TAG, "Creating RTMP connection for target: %s", target->name);

    /* Allocate connection structure */
    *conn = malloc(sizeof(rtmp_client_connection_t));
    if (!*conn) {
        IMP_LOG_ERR(TAG, "Failed to allocate connection structure");
        return -1;
    }

    memset(*conn, 0, sizeof(rtmp_client_connection_t));

    /* Copy target configuration */
    strncpy((*conn)->name, target->name, sizeof((*conn)->name) - 1);
    strncpy((*conn)->url, target->url, sizeof((*conn)->url) - 1);
    strncpy((*conn)->stream_key, target->stream_key, sizeof((*conn)->stream_key) - 1);

    /* Initialize connection state */
    (*conn)->socket_fd = -1;
    (*conn)->state = RTMP_CLIENT_STATE_DISCONNECTED;
    (*conn)->thread_running = false;
    (*conn)->chunk_size_in = RTMP_DEFAULT_CHUNK_SIZE;
    (*conn)->chunk_size_out = RTMP_DEFAULT_CHUNK_SIZE;
    (*conn)->window_ack_size = 2500000; /* 2.5MB - Adobe RTMP spec default */
    (*conn)->stream_id = 0;
    (*conn)->retry_count = 0;
    (*conn)->last_retry_time = 0;

    /* Initialize acknowledgement tracking */
    (*conn)->bytes_received = 0;
    (*conn)->last_ack_sent = 0;

    /* Initialize TLS contexts to NULL */
    (*conn)->ssl_context = NULL;
    (*conn)->ssl_config = NULL;
    (*conn)->entropy_context = NULL;
    (*conn)->ctr_drbg_context = NULL;

    IMP_LOG_INFO(TAG, "RTMP connection created for target: %s", target->name);
    return 0;
}

void rtmp_client_connection_destroy(rtmp_client_connection_t* conn)
{
    if (!conn) {
        return;
    }

    IMP_LOG_INFO(TAG, "Destroying RTMP connection for target: %s", conn->name);

    /* Stop connection if running */
    if (conn->thread_running) {
        rtmp_client_connection_stop(conn);
    }

    /* Close socket if open */
    if (conn->socket_fd >= 0) {

        close(conn->socket_fd);
        conn->socket_fd = -1;
    }

    /* Clean up TLS contexts */
#if defined(RTMPS_BACKEND_OPENSSL) || defined(RTMPS_BACKEND_MBEDTLS)
    rtmp_client_tls_cleanup(conn);
#endif

    /* Clean up cached SPS/PPS data */
    if (conn->cached_sps) {
        free(conn->cached_sps);
        conn->cached_sps = NULL;
    }
    if (conn->cached_pps) {
        free(conn->cached_pps);
        conn->cached_pps = NULL;
    }

    /* Free connection structure */
    free(conn);
}

int rtmp_client_connection_start(rtmp_client_connection_t* conn)
{
    if (!conn) {
        IMP_LOG_ERR(TAG, "Invalid connection for start");
        return -1;
    }

    if (conn->thread_running) {
        IMP_LOG_WARN(TAG, "Connection already running for target: %s", conn->name);
        return 0;
    }

    IMP_LOG_INFO(TAG, "Starting RTMP connection for target: %s", conn->name);

    /* Start connection thread */
    conn->thread_running = true;
    if (pthread_create(&conn->thread, NULL, rtmp_client_connection_thread, conn) != 0) {
        IMP_LOG_ERR(TAG, "Failed to create connection thread for target: %s", conn->name);
        conn->thread_running = false;
        return -1;
    }

    IMP_LOG_INFO(TAG, "RTMP connection thread started for target: %s", conn->name);
    return 0;
}

int rtmp_client_connection_stop(rtmp_client_connection_t* conn)
{
    if (!conn) {
        return 0;
    }

    if (!conn->thread_running) {
        return 0;
    }

    IMP_LOG_INFO(TAG, "Stopping RTMP connection for target: %s", conn->name);

    /* Signal thread to stop */
    conn->thread_running = false;

    /* Close socket to break any blocking operations */
    if (conn->socket_fd >= 0) {
        close(conn->socket_fd);
        conn->socket_fd = -1;
    }

    /* Wait for thread to finish */
    pthread_join(conn->thread, NULL);

    IMP_LOG_INFO(TAG, "RTMP connection stopped for target: %s", conn->name);
    return 0;
}

/* RTSP frame callback for RTMP client module - receives frame data from RTSP module */
int rtmp_client_module_rtsp_frame_callback(struct rtsp_server* server, int channel, const uint8_t* frame_data, uint32_t frame_size, const struct timeval* timestamp)
{
    if (!g_rtmp_client_module_state.running || !g_rtmp_client) {
        IMP_LOG_INFO(TAG, "RTMP client module not running");
        return 0;
    }

    if (!frame_data || frame_size == 0 || !timestamp) {
        IMP_LOG_INFO(TAG, "Invalid frame data for RTMP client");
        return 0;
    }

    /* Check if we have any active RTMP connections */
    int active_connections = 0;
    int total_connections = 0;
    pthread_mutex_lock(&g_rtmp_client->connections_mutex);

    rtmp_client_connection_t* conn = g_rtmp_client->connections;
    while (conn) {
        total_connections++;
        if (conn->state == RTMP_CLIENT_STATE_PUBLISHING && conn->thread_running) {
            active_connections++;
        }
        conn = conn->next;
    }

    pthread_mutex_unlock(&g_rtmp_client->connections_mutex);

    /* Debug logging every 100 calls to avoid spam */
    static int callback_count = 0;
    callback_count++;
    if (callback_count % 100 == 0) {
        IMP_LOG_DBG(TAG, "RTMP Client frame callback: channel=%d, total_connections=%d, active_connections=%d",
                   channel, total_connections, active_connections);
    }

    /* Only process frames if we have active connections */
    if (active_connections == 0) {
        return 0;
    }

    /* Only process the configured video channel */
    if (channel != g_rtmp_client_module_state.config.video.channel) {
        return 0;
    }

    /* Only process enabled channels that are not JPEG */
    if (!chn[channel].enable || chn[channel].payloadType == IMP_ENC_PROFILE_JPEG) {
        return 0;
    }

    /* Send frame to RTMP connections using the frame data passed from RTSP module */
    int frames_sent = rtmp_client_send_frame(g_rtmp_client, channel, frame_data, frame_size, timestamp);

    if (frames_sent > 0) {
        /* Frame sent successfully */
    } else {
        /* Debug why no frames were sent */
        static int no_frame_count = 0;
        no_frame_count++;
        if (no_frame_count % 50 == 0) {
            IMP_LOG_WARN(TAG, "RTMP Client: No frames sent for channel %d (count: %d)", channel, no_frame_count);
        }
    }

    return frames_sent;
}

/* RTMP client frame distribution */
int rtmp_client_send_frame(rtmp_client_t* client, int channel, const uint8_t* frame_data, uint32_t frame_size, const struct timeval* timestamp)
{
    if (!client || !frame_data || frame_size == 0) {
        IMP_LOG_ERR(TAG, "Invalid parameters for sending frame");
        return 0;
    }

    /* Convert timestamp to milliseconds */
    uint32_t rtmp_timestamp = (timestamp->tv_sec * 1000) + (timestamp->tv_usec / 1000);

    /* Bitrate monitoring and limiting for YouTube compatibility */
    static uint32_t total_bytes_sent = 0;
    static uint32_t last_bitrate_check = 0;
    static int frames_dropped_for_bitrate = 0;

    total_bytes_sent += frame_size;

    if (last_bitrate_check == 0) {
        last_bitrate_check = rtmp_timestamp;
    } else if (rtmp_timestamp - last_bitrate_check >= 1000) { /* Check every second */
        uint32_t bitrate_kbps = (total_bytes_sent * 8) / 1024; /* Convert to kbps */
        IMP_LOG_INFO(TAG, "RTMP bitrate: %u kbps, frames dropped: %d", bitrate_kbps, frames_dropped_for_bitrate);

        /* Reset counters */
        total_bytes_sent = 0;
        last_bitrate_check = rtmp_timestamp;
        frames_dropped_for_bitrate = 0;
    }

    int frames_sent = 0;

    /* Send frame to all publishing connections */
    pthread_mutex_lock(&client->connections_mutex);

    rtmp_client_connection_t* conn = client->connections;
    while (conn) {
        if (conn->state == RTMP_CLIENT_STATE_PUBLISHING && conn->thread_running) {
            /* Send video frame to this connection */
            if (rtmp_client_send_video_frame(conn, frame_data, frame_size, rtmp_timestamp) == 0) {
                frames_sent++;
                conn->frames_sent++;
                conn->bytes_sent += frame_size;
                conn->last_frame_time = time(NULL);
            }
        }
        conn = conn->next;
    }

    pthread_mutex_unlock(&client->connections_mutex);

    if (frames_sent > 0) {
        /* Frame distributed */
    }

    return frames_sent;
}

/* Forward declarations */
static int rtmp_client_connection_read_available(rtmp_client_connection_t* conn, uint8_t* buffer, size_t max_length);
static int rtmp_client_send_silent_audio_frame(rtmp_client_connection_t* conn, uint32_t timestamp);
static int send_aac_config(rtmp_client_connection_t* conn);
static int send_avc_sequence_header_from_frame(rtmp_client_connection_t* conn, const uint8_t* frame_data, uint32_t frame_size);
static void cache_sps_pps_from_frame(const uint8_t* frame_data, uint32_t frame_size, uint8_t** sps_data, uint32_t* sps_size, uint8_t** pps_data, uint32_t* pps_size);

static int send_cached_avc_sequence_header(rtmp_client_connection_t* conn, const uint8_t* sps_data, uint32_t sps_size, const uint8_t* pps_data, uint32_t pps_size);
static int rtmp_send_video_packet(rtmp_client_connection_t* conn, const uint8_t* data, uint32_t size, uint32_t timestamp, bool is_sequence_header);

/* Send RTMP message with proper chunking */
static int rtmp_client_send_chunked_message(rtmp_client_connection_t* conn, uint8_t chunk_stream_id,
                                           uint8_t message_type, uint32_t timestamp,
                                           const uint8_t* data, uint32_t data_size)
{
    if (!conn || !data || data_size == 0) {
        IMP_LOG_ERR(TAG, "Invalid parameters for sending chunked message");
        return -1;
    }

    uint32_t stream_id = (uint32_t)conn->stream_id;
    uint32_t chunk_size = conn->chunk_size_out;
    uint32_t bytes_sent = 0;
    bool first_chunk = true;
    int chunk_count = 0;

    /* All per-frame logging disabled for performance */

    /* Video payload analysis disabled for performance */

    while (bytes_sent < data_size) {
        uint32_t chunk_data_size = (data_size - bytes_sent > chunk_size) ? chunk_size : (data_size - bytes_sent);
        chunk_count++;

        /* Chunk logging disabled for performance */

        /* Create basic header with proper chunk stream ID encoding */
        uint8_t basic_header;
        uint8_t fmt = first_chunk ? 0 : 3; /* Type 0 for first chunk, Type 3 for continuation */

        /* RTMP spec: chunk stream ID encoding */
        if (chunk_stream_id < 2) {
            IMP_LOG_ERR(TAG, "Invalid chunk stream ID: %u (must be >= 2)", chunk_stream_id);
            return -1;
        } else if (chunk_stream_id <= 63) {
            /* 1-byte basic header: fmt(2) + cs_id(6) */
            basic_header = (fmt << 6) | chunk_stream_id;
        } else {
            IMP_LOG_ERR(TAG, "Chunk stream ID %u > 63 not supported yet", chunk_stream_id);
            return -1;
        }

        /* Send basic header */
        if (rtmp_client_connection_write_bytes(conn, &basic_header, 1) != 1) {
            IMP_LOG_ERR(TAG, "Failed to send basic header for chunk %d, target: %s", chunk_count, conn->name);

            /* Try to read any error response immediately */
            uint8_t error_buffer[512];
            int error_bytes = rtmp_client_connection_read_available(conn, error_buffer, sizeof(error_buffer));
            if (error_bytes > 0) {
                IMP_LOG_ERR(TAG, "%s response after basic header failure (%d bytes):", conn->name, error_bytes);
                char hex_str[128] = {0};
                int print_bytes = (error_bytes > 40) ? 40 : error_bytes;
                for (int i = 0; i < print_bytes; i++) {
                    snprintf(hex_str + (i * 3), 4, "%02x ", error_buffer[i]);
                }
                IMP_LOG_ERR(TAG, "Response hex: %s", hex_str);
            } else {
                IMP_LOG_ERR(TAG, "No response data available from %s", conn->name);
            }

            return -1;
        }

        /* Send message header only for first chunk (Type 0) */
        if (first_chunk) {
            uint8_t message_header[11];

            /* Timestamp (3 bytes, big-endian) */
            message_header[0] = (timestamp >> 16) & 0xFF;
            message_header[1] = (timestamp >> 8) & 0xFF;
            message_header[2] = timestamp & 0xFF;

            /* Message length (3 bytes, big-endian) */
            message_header[3] = (data_size >> 16) & 0xFF;
            message_header[4] = (data_size >> 8) & 0xFF;
            message_header[5] = data_size & 0xFF;

            /* Message type ID */
            message_header[6] = message_type;

            /* Message stream ID (4 bytes, little-endian) */
            message_header[7] = stream_id & 0xFF;
            message_header[8] = (stream_id >> 8) & 0xFF;
            message_header[9] = (stream_id >> 16) & 0xFF;
            message_header[10] = (stream_id >> 24) & 0xFF;

            if (rtmp_client_connection_write_bytes(conn, message_header, 11) != 11) {
                IMP_LOG_ERR(TAG, "Failed to send message header for target: %s", conn->name);
                return -1;
            }
            first_chunk = false;
        }

        /* Send chunk data */
        if (rtmp_client_connection_write_bytes(conn, data + bytes_sent, chunk_data_size) != (int)chunk_data_size) {
            IMP_LOG_ERR(TAG, "Failed to send chunk data for chunk %d (size=%u), target: %s",
                       chunk_count, chunk_data_size, conn->name);

            /* Try to read any error response from server */
            uint8_t error_buffer[512];
            int error_bytes = rtmp_client_connection_read_available(conn, error_buffer, sizeof(error_buffer));
            if (error_bytes > 0) {
                IMP_LOG_ERR(TAG, "Server sent %d bytes after chunk failure", error_bytes);
                char hex_str[128] = {0};
                int print_bytes = (error_bytes > 40) ? 40 : error_bytes;
                for (int i = 0; i < print_bytes; i++) {
                    snprintf(hex_str + (i * 3), 4, "%02x ", error_buffer[i]);
                }
                IMP_LOG_ERR(TAG, "Server response: %s", hex_str);
            }

            return -1;
        }

        bytes_sent += chunk_data_size;
    }

    /* Message sent successfully */
    return 0;
}

/* RTMP client video frame sending */
int rtmp_client_send_video_frame(rtmp_client_connection_t* conn, const uint8_t* frame_data, uint32_t frame_size, uint32_t timestamp)
{
    if (!conn || !frame_data || frame_size == 0) {
        IMP_LOG_ERR(TAG, "Invalid parameters for sending video frame");
        return -1;
    }

    if (conn->state != RTMP_CLIENT_STATE_PUBLISHING) {
        IMP_LOG_DBG(TAG, "Not publishing for target: %s, skipping frame", conn->name);
        return 0; /* Not publishing, skip */
    }

    /* Check if connection is still valid */
    if (conn->socket_fd < 0) {
        IMP_LOG_DBG(TAG, "Connection invalid for target: %s, skipping frame", conn->name);
        return 0;
    }

    /* Create RTMP video message */
    /* Video message format:
     * Byte 0: Frame type (4 bits) + Codec ID (4 bits)
     * Byte 1+: Video data
     */

    uint8_t* video_message = malloc(frame_size + 16); /* Extra space for headers */
    if (!video_message) {
        IMP_LOG_ERR(TAG, "Failed to allocate video message buffer");
        return -1;
    }

    size_t message_pos = 0;

    /* Determine frame type based on NAL unit type */
    uint8_t frame_type = 2; /* Inter frame (default) */
    uint8_t codec_id = 7;   /* Always use AVC (H.264) for RTMP compatibility */

    /* TODO: YouTube requirements not fully met:
     * - Needs Progressive Scan, 2 B-Frames, 1 Reference Frame
     * - Needs CABAC entropy coding (Main/High profile)
     * - Prefers H.265 for HDR content
     * Current implementation sends baseline H.264 which may cause disconnections
     */

    /* Check for keyframe (IDR) and extract SPS/PPS if needed */
    bool is_keyframe = false;

    /* Frame header logging disabled for performance */

    /* Scan entire frame for ALL NAL units, not just the first one */
    bool found_sps = false, found_pps = false, found_idr = false;
    for (size_t i = 0; i < frame_size - 4; i++) {
        if (frame_data[i] == 0x00 && frame_data[i+1] == 0x00 &&
            frame_data[i+2] == 0x00 && frame_data[i+3] == 0x01) {
            uint8_t nal_type = frame_data[i+4] & 0x1F;
            /* NAL unit logging disabled for performance */

            if (nal_type == 7) found_sps = true;
            else if (nal_type == 8) found_pps = true;
            else if (nal_type == 5) found_idr = true;
        }
    }

    if (found_sps || found_pps || found_idr) {
        /* Set keyframe flag if we have SPS+PPS+IDR (complete keyframe) */
        if (found_sps && found_pps && found_idr) {
            is_keyframe = true;
            frame_type = 1; /* Key frame */
        } else if (found_idr) {
            is_keyframe = true;
            frame_type = 1; /* Key frame */
        }
    }

    /* Set P-frame type if not already a keyframe */
    if (!is_keyframe && frame_size >= 5 && frame_data[0] == 0x00 && frame_data[1] == 0x00 &&
        frame_data[2] == 0x00 && frame_data[3] == 0x01) {
        uint8_t nal_type = frame_data[4] & 0x1F;
        if (nal_type == 1) {
            frame_type = 2; /* Inter frame */
        }
    }

    /* Send AVC sequence header once per connection using frame manager SPS/PPS */
    /* Use per-connection state instead of global static variables */

    if (is_keyframe) {
        conn->frames_without_keyframe = 0;

        /* Get stream parameters from frame manager for keyframe handling */
        stream_params_t stream_params = frame_manager_get_stream_params(0);

        if (!conn->avc_header_sent) {
            /* Prioritize frame manager SPS/PPS over extracting from current frame */
            if (stream_params.available) {
                /* Use stream parameters from frame manager (preferred method) */
                IMP_LOG_INFO(TAG, "Using frame manager SPS/PPS for AVC sequence header (SPS: %u bytes, PPS: %u bytes)",
                           stream_params.sps_size, stream_params.pps_size);
                if (send_cached_avc_sequence_header(conn, stream_params.sps_data, stream_params.sps_size,
                                                   stream_params.pps_data, stream_params.pps_size) == 0) {
                    conn->avc_header_sent = true;
                    IMP_LOG_INFO(TAG, "AVC sequence header sent using frame manager SPS/PPS");
                }
            } else {
                /* Fallback: try to extract from current frame */
                IMP_LOG_INFO(TAG, "Frame manager SPS/PPS not available, attempting to extract from keyframe for %s", conn->name);
                if (send_avc_sequence_header_from_frame(conn, frame_data, frame_size) == 0) {
                    conn->avc_header_sent = true;
                    IMP_LOG_INFO(TAG, "AVC sequence header sent from keyframe for %s", conn->name);
                } else {
                    /* Check timeout - if connection has been waiting more than 5 seconds, allow any frame */
                    extern uint64_t get_monotonic_time_us(void);
                    unsigned long current_time_us = (unsigned long)get_monotonic_time_us();
                    unsigned long wait_time_us = current_time_us - conn->avc_wait_start_us;

                    if (wait_time_us > 5000000) { /* 5 seconds in microseconds */
                        IMP_LOG_WARN(TAG, "RTMP connection %s timeout waiting for SPS/PPS (%lu.%06lu seconds), allowing keyframe without AVC header",
                                    conn->name, wait_time_us / 1000000, wait_time_us % 1000000);
                        conn->avc_header_sent = true; /* Mark as sent to prevent further waiting */
                        /* Continue with sending the frame below */
                    } else {
                        /* Wait for real SPS/PPS data - don't send anything until we have it */
                        if (wait_time_us % 1000000 < 100000) { /* Log every ~1 second to avoid spam */
                            IMP_LOG_WARN(TAG, "No SPS/PPS data available for %s - skipping keyframe (waiting %lu.%06lu seconds)",
                                        conn->name, wait_time_us / 1000000, wait_time_us % 1000000);
                        }
                        conn->frames_without_keyframe = 0; /* Reset counter since we found a keyframe, just no SPS/PPS yet */
                        free(video_message);
                        return 0; /* Skip this frame until we have real parameters */
                    }
                }
            }

            /* Cache SPS/PPS for this connection if we have frame manager data */
            if (stream_params.available && conn->avc_header_sent) {
                if (conn->cached_sps) free(conn->cached_sps);
                if (conn->cached_pps) free(conn->cached_pps);
                conn->cached_sps = malloc(stream_params.sps_size);
                conn->cached_pps = malloc(stream_params.pps_size);
                if (conn->cached_sps && conn->cached_pps) {
                    memcpy(conn->cached_sps, stream_params.sps_data, stream_params.sps_size);
                    memcpy(conn->cached_pps, stream_params.pps_data, stream_params.pps_size);
                    conn->cached_sps_size = stream_params.sps_size;
                    conn->cached_pps_size = stream_params.pps_size;
                }
            }
        } else {
            /* AVC header already sent */
            IMP_LOG_DBG(TAG, "AVC sequence header already sent");
        }
    } else {
        conn->frames_without_keyframe++;
        if (conn->frames_without_keyframe > 30) {
            IMP_LOG_WARN(TAG, "No keyframe found after %d frames - requesting IDR", conn->frames_without_keyframe);

            /* Request IDR frame to force keyframe generation */
            extern int IMP_Encoder_RequestIDR(int encChn);
            /* Use channel 0 as default for IDR requests when no keyframes are detected */
            int ret = IMP_Encoder_RequestIDR(0);
            if (ret < 0) {
                IMP_LOG_WARN(TAG, "Failed to request IDR frame for channel 0: %d", ret);
            } else {
                IMP_LOG_INFO(TAG, "Requested IDR frame for channel 0 to generate keyframes");
            }
        }

        /* Send AVC sequence header if we have stream parameters but haven't sent it yet */
        if (!conn->avc_header_sent) {
            stream_params_t stream_params = frame_manager_get_stream_params(0);
            if (stream_params.available) {
                IMP_LOG_INFO(TAG, "Sending AVC sequence header using frame manager SPS/PPS (non-keyframe)");
                if (send_cached_avc_sequence_header(conn, stream_params.sps_data, stream_params.sps_size,
                                                   stream_params.pps_data, stream_params.pps_size) == 0) {
                    conn->avc_header_sent = true;
                    IMP_LOG_INFO(TAG, "AVC sequence header sent using frame manager SPS/PPS");

                    /* Cache SPS/PPS for this connection (update if new values arrive) */
                    if (conn->cached_sps) free(conn->cached_sps);
                    if (conn->cached_pps) free(conn->cached_pps);
                    conn->cached_sps = malloc(stream_params.sps_size);
                    conn->cached_pps = malloc(stream_params.pps_size);
                    if (conn->cached_sps && conn->cached_pps) {
                        memcpy(conn->cached_sps, stream_params.sps_data, stream_params.sps_size);
                        memcpy(conn->cached_pps, stream_params.pps_data, stream_params.pps_size);
                        conn->cached_sps_size = stream_params.sps_size;
                        conn->cached_pps_size = stream_params.pps_size;
                    }
                } else {
                    IMP_LOG_WARN(TAG, "Failed to send AVC sequence header using frame manager SPS/PPS");
                    free(video_message);
                    return 0;
                }
            } else {
                /* Check timeout - if connection has been waiting more than 5 seconds, allow any frame */
                extern uint64_t get_monotonic_time_us(void);
                unsigned long current_time_us = (unsigned long)get_monotonic_time_us();
                unsigned long wait_time_us = current_time_us - conn->avc_wait_start_us;

                if (wait_time_us > 5000000) { /* 5 seconds in microseconds */
                    IMP_LOG_WARN(TAG, "RTMP connection %s timeout waiting for frame manager SPS/PPS (%lu.%06lu seconds), allowing frame without AVC header",
                                conn->name, wait_time_us / 1000000, wait_time_us % 1000000);
                    conn->avc_header_sent = true; /* Mark as sent to prevent further waiting */
                    /* Continue with sending the frame below */
                } else {
                    if (wait_time_us % 1000000 < 100000) { /* Log every ~1 second to avoid spam */
                        IMP_LOG_DBG(TAG, "Skipping frame - waiting for stream parameters from frame manager (%lu.%06lu seconds)",
                                   wait_time_us / 1000000, wait_time_us % 1000000);
                    }
                    free(video_message);
                    return 0;
                }
            }
        }
    }

    /* Video tag header - always use H.264 format for RTMP */
    video_message[message_pos++] = (frame_type << 4) | codec_id;

    /* Add AVC packet type and composition time */
    video_message[message_pos++] = 1; /* AVC NALU */
    video_message[message_pos++] = 0; /* Composition time (3 bytes) */
    video_message[message_pos++] = 0;
    video_message[message_pos++] = 0;

    /* Remove artificial frame size limit - RTMP chunking handles large frames correctly */

    /* Convert Annex B format to AVCC format */
    size_t src_pos = 0;
    while (src_pos < frame_size) {
        /* Find start code (00 00 00 01) */
        if (src_pos + 4 <= frame_size &&
            frame_data[src_pos] == 0x00 && frame_data[src_pos + 1] == 0x00 &&
            frame_data[src_pos + 2] == 0x00 && frame_data[src_pos + 3] == 0x01) {

            /* Find next start code or end of frame */
            size_t nal_start = src_pos + 4;
            size_t nal_end = frame_size;

            for (size_t i = nal_start + 3; i < frame_size; i++) {
                if (frame_data[i-3] == 0x00 && frame_data[i-2] == 0x00 &&
                    frame_data[i-1] == 0x00 && frame_data[i] == 0x01) {
                    nal_end = i - 3;
                    break;
                }
            }

            uint32_t nal_size = nal_end - nal_start;
            uint8_t nal_type = frame_data[nal_start] & 0x1F;

            /* Debug logging for first few frames */
            static int debug_frame_count = 0;
            if (debug_frame_count < 5) {
                IMP_LOG_INFO(TAG, "Frame %d: Found NAL type %d, size %u bytes (start=%zu, end=%zu)",
                           debug_frame_count, nal_type, nal_size, nal_start, nal_end);
            }

            /* Skip SPS/PPS in regular frames - they should only be in AVC sequence header */
            if (nal_type == 7 || nal_type == 8) {
                if (debug_frame_count < 5) {
                    IMP_LOG_INFO(TAG, "Frame %d: Skipping NAL type %d (SPS/PPS) in regular frame", debug_frame_count, nal_type);
                }
                src_pos = nal_end;
                continue;
            }

            /* Include all other NAL types: SEI (6), IDR (5), slices (1), etc. */
            if (debug_frame_count < 5) {
                IMP_LOG_INFO(TAG, "Frame %d: Including NAL type %d in AVCC format", debug_frame_count, nal_type);
            }

            /* Write NAL size as 4-byte big-endian length */
            if (message_pos + 4 + nal_size > frame_size + 16) {
                IMP_LOG_ERR(TAG, "Video buffer overflow: need %zu, have %zu", message_pos + 4 + nal_size, frame_size + 16);
                free(video_message);
                return -1;
            }

            video_message[message_pos] = (nal_size >> 24) & 0xFF;
            video_message[message_pos + 1] = (nal_size >> 16) & 0xFF;
            video_message[message_pos + 2] = (nal_size >> 8) & 0xFF;
            video_message[message_pos + 3] = nal_size & 0xFF;
            message_pos += 4;

            /* Copy NAL unit data */
            memcpy(video_message + message_pos, frame_data + nal_start, nal_size);
            message_pos += nal_size;

            if (debug_frame_count < 5) {
                debug_frame_count++;
            }

            src_pos = nal_end;
        } else {
            /* Skip byte if no start code found */
            src_pos++;
        }
    }

    /* Send video message using proper chunking with chunk stream ID 6 */
    int result = rtmp_client_send_chunked_message(conn, 6, RTMP_MSG_VIDEO, timestamp, video_message, message_pos);

    if (result == 0) {
        /* Video frame sent successfully */
        static int successful_frames = 0;
        successful_frames++;

        if (successful_frames % 100 == 0) {
            IMP_LOG_INFO(TAG, "Successfully sent %d video frames to %s", successful_frames, conn->name);
        }

        /* Send silent audio frame periodically for RTMP compatibility */
        static uint32_t last_audio_timestamp = 0;
        if (timestamp - last_audio_timestamp >= 40) { /* Send audio every 40ms */
            rtmp_client_send_silent_audio_frame(conn, timestamp);
            last_audio_timestamp = timestamp;
            /* Audio frame sent */
        }
    } else {
        IMP_LOG_WARN(TAG, "Failed to send video frame to %s (result=%d, frame_size=%u, timestamp=%u)",
                    conn->name, result, frame_size, timestamp);
        conn->state = RTMP_CLIENT_STATE_DISCONNECTED;
    }

    free(video_message);
    return result;
}

/* Send silent AAC audio frame for RTMP compatibility */
static int rtmp_client_send_silent_audio_frame(rtmp_client_connection_t* conn, uint32_t timestamp)
{
    if (!conn || conn->state != RTMP_CLIENT_STATE_PUBLISHING || conn->socket_fd < 0) {
        return 0;
    }

    /* YouTube-compliant AAC silent frame (AAC-LC, 44.1kHz, stereo, 128kbps) */
    static const uint8_t silent_aac_frame[] = {
        0xAF, 0x01,  /* Audio tag: AAC, 44.1kHz, 16-bit, stereo + AAC raw data */
        0x12, 0x10,  /* AAC frame header for 44.1kHz stereo */
        0x56, 0xE5, 0x00,  /* AAC silent frame data for 44.1kHz */
        0x00, 0x00, 0x00, 0x00  /* Additional padding for 128kbps bitrate */
    };

    /* Send audio message using chunking with chunk stream ID 4 */
    int result = rtmp_client_send_chunked_message(conn, 4, RTMP_MSG_AUDIO, timestamp,
                                                 silent_aac_frame, sizeof(silent_aac_frame));

    if (result == 0) {
        /* Audio frame sent */
    }

    return result;
}

/* RTMP client audio frame sending */
int rtmp_client_send_audio_frame(rtmp_client_connection_t* conn, const uint8_t* frame_data, uint32_t frame_size, uint32_t timestamp)
{
    if (!conn || !frame_data || frame_size == 0) {
        return -1;
    }

    if (conn->state != RTMP_CLIENT_STATE_PUBLISHING) {
        return 0; /* Not publishing, skip */
    }

    /* Check if connection is still valid */
    if (conn->socket_fd < 0) {
        IMP_LOG_DBG(TAG, "Connection invalid for target: %s, skipping audio frame", conn->name);
        return 0;
    }

    /* Create RTMP audio message */
    uint8_t* audio_message = malloc(frame_size + 16); /* Extra space for headers */
    if (!audio_message) {
        IMP_LOG_ERR(TAG, "Failed to allocate audio message buffer");
        return -1;
    }

    size_t message_pos = 0;

    /* Audio tag header for AAC */
    uint8_t sound_format = 10;  /* AAC */
    uint8_t sound_rate = 3;     /* 44kHz (or closest) */
    uint8_t sound_size = 1;     /* 16-bit */
    uint8_t sound_type = 1;     /* Stereo (or mono) */

    audio_message[message_pos++] = (sound_format << 4) | (sound_rate << 2) | (sound_size << 1) | sound_type;

    /* AAC packet type */
    audio_message[message_pos++] = 1; /* AAC raw data */

    /* Copy audio data */
    memcpy(audio_message + message_pos, frame_data, frame_size);
    message_pos += frame_size;

    /* Send audio message using proper chunking with chunk stream ID 4 */
    int result = rtmp_client_send_chunked_message(conn, 4, RTMP_MSG_AUDIO, timestamp, audio_message, message_pos);

    if (result == 0) {
        IMP_LOG_DBG(TAG, "Sent audio frame: %u bytes, timestamp=%u to target: %s",
                   frame_size, timestamp, conn->name);
    } else {
        IMP_LOG_DBG(TAG, "Failed to send audio frame for target: %s", conn->name);
        conn->state = RTMP_CLIENT_STATE_DISCONNECTED;
    }

    free(audio_message);
    return result;
}

/* Send RTMP acknowledgement message */
static int rtmp_client_send_acknowledgement(rtmp_client_connection_t* conn, uint32_t bytes_received)
{
    if (!conn || conn->state < RTMP_CLIENT_STATE_CONNECTED) {
        return -1;
    }

    /* Create acknowledgement message (4 bytes) */
    uint8_t ack_data[4];
    ack_data[0] = (bytes_received >> 24) & 0xFF;
    ack_data[1] = (bytes_received >> 16) & 0xFF;
    ack_data[2] = (bytes_received >> 8) & 0xFF;
    ack_data[3] = bytes_received & 0xFF;

    /* Send acknowledgement using chunk stream ID 2 (control messages) */
    int result = rtmp_client_send_chunked_message(conn, 2, RTMP_MSG_ACKNOWLEDGEMENT, 0, ack_data, 4);

    if (result == 0) {
        conn->last_ack_sent = bytes_received;
        IMP_LOG_DBG(TAG, "Sent acknowledgement: %u bytes received to target: %s", bytes_received, conn->name);
    } else {
        IMP_LOG_ERR(TAG, "Failed to send acknowledgement for target: %s", conn->name);
    }

    return result;
}

/* Check if acknowledgement needs to be sent and send it */
static int rtmp_client_check_and_send_ack(rtmp_client_connection_t* conn)
{
    if (!conn || conn->window_ack_size == 0) {
        return 0;
    }

    /* Check if we need to send an acknowledgement */
    uint32_t bytes_since_last_ack = conn->bytes_received - conn->last_ack_sent;

    IMP_LOG_DBG(TAG, "ACK check: bytes_since_last=%u, window_size=%u, need_ack=%s",
               bytes_since_last_ack, conn->window_ack_size,
               (bytes_since_last_ack >= conn->window_ack_size) ? "YES" : "NO");

    if (bytes_since_last_ack >= conn->window_ack_size) {
        IMP_LOG_INFO(TAG, "Sending acknowledgement: %u bytes received", conn->bytes_received);
        return rtmp_client_send_acknowledgement(conn, conn->bytes_received);
    }

    return 0;
}

/* RTMP client protocol functions */

/* Connection-aware I/O wrappers that handle both TLS and plain sockets */
static int rtmp_client_connection_write_bytes(rtmp_client_connection_t* conn, const uint8_t* buffer, size_t length)
{
#if defined(RTMPS_BACKEND_OPENSSL) || defined(RTMPS_BACKEND_MBEDTLS)
    if (conn->use_tls) {
        return rtmp_client_tls_write_bytes(conn, buffer, length);
    }
#endif
    return rtmp_client_write_bytes(conn->socket_fd, buffer, length);
}

static int rtmp_client_connection_read_bytes(rtmp_client_connection_t* conn, uint8_t* buffer, size_t length)
{
    int bytes_read;

#if defined(RTMPS_BACKEND_OPENSSL) || defined(RTMPS_BACKEND_MBEDTLS)
    if (conn->use_tls) {
        bytes_read = rtmp_client_tls_read_bytes(conn, buffer, length);
    } else {
        bytes_read = rtmp_client_read_bytes(conn->socket_fd, buffer, length);
    }
#else
    bytes_read = rtmp_client_read_bytes(conn->socket_fd, buffer, length);
#endif

    /* Track bytes received for acknowledgements */
    if (bytes_read > 0) {
        conn->bytes_received += bytes_read;

        IMP_LOG_DBG(TAG, "Received %d bytes, total: %u, window: %u, last_ack: %u",
                   bytes_read, conn->bytes_received, conn->window_ack_size, conn->last_ack_sent);

        /* Check if we need to send an acknowledgement */
        rtmp_client_check_and_send_ack(conn);
    }

    return bytes_read;
}

/* Non-blocking read for RTMP responses - reads available data without blocking */
static int rtmp_client_connection_read_available(rtmp_client_connection_t* conn, uint8_t* buffer, size_t max_length)
{
#if defined(RTMPS_BACKEND_OPENSSL) || defined(RTMPS_BACKEND_MBEDTLS)
    if (conn->use_tls) {
        /* Use the TLS abstraction function instead of direct SSL calls */
        int ret = rtmp_client_tls_read_bytes(conn, buffer, max_length);
        if (ret <= 0) {
            return ret; /* Error or no data available */
        }

        /* Track bytes received for acknowledgements */
        if (ret > 0) {
            conn->bytes_received += ret;

            IMP_LOG_DBG(TAG, "TLS read_available: received %d bytes, total: %u, window: %u, last_ack: %u",
                       ret, conn->bytes_received, conn->window_ack_size, conn->last_ack_sent);

            /* Check if we need to send an acknowledgement */
            rtmp_client_check_and_send_ack(conn);
        }

        return ret;
    }
#endif

    /* Set socket to non-blocking temporarily */
    int flags = fcntl(conn->socket_fd, F_GETFL, 0);
    fcntl(conn->socket_fd, F_SETFL, flags | O_NONBLOCK);

    ssize_t result = recv(conn->socket_fd, buffer, max_length, 0);

    /* Restore blocking mode */
    fcntl(conn->socket_fd, F_SETFL, flags);

    if (result < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return 0; /* No data available */
        }
        return -1; /* Error */
    }

    /* Track bytes received for acknowledgements */
    if (result > 0) {
        conn->bytes_received += result;

        IMP_LOG_DBG(TAG, "Read_available: received %d bytes, total: %u, window: %u, last_ack: %u",
                   result, conn->bytes_received, conn->window_ack_size, conn->last_ack_sent);

        /* Check if we need to send an acknowledgement */
        rtmp_client_check_and_send_ack(conn);
    }

    return result;
}



/* Helper function to write bytes to socket */
static int rtmp_client_write_bytes(int socket_fd, const uint8_t* buffer, size_t length)
{
    size_t bytes_written = 0;
    while (bytes_written < length) {
        ssize_t result = send(socket_fd, buffer + bytes_written, length - bytes_written, 0);
        if (result <= 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                IMP_LOG_ERR(TAG, "Socket timeout during write");
            } else {
                IMP_LOG_ERR(TAG, "send() failed: %s", strerror(errno));
            }
            return -1;
        }
        bytes_written += result;
    }
    return bytes_written;
}

/* Helper function to read bytes from socket */
static int rtmp_client_read_bytes(int socket_fd, uint8_t* buffer, size_t length)
{
    size_t bytes_read = 0;
    while (bytes_read < length) {
        ssize_t result = recv(socket_fd, buffer + bytes_read, length - bytes_read, 0);
        if (result <= 0) {
            if (result == 0) {
                IMP_LOG_DBG(TAG, "Connection closed during read");
                return 0; /* Connection closed */
            } else {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    IMP_LOG_ERR(TAG, "Socket timeout during read");
                } else {
                    IMP_LOG_ERR(TAG, "recv() failed: %s", strerror(errno));
                }
                return -1;
            }
        }
        bytes_read += result;
    }
    return bytes_read;
}

/* RTMP client handshake implementation */
int rtmp_client_handshake_process(rtmp_client_connection_t* conn)
{
    IMP_LOG_DBG(TAG, "Processing RTMP handshake for target: %s", conn->name);

    /* Step 1: Send C0 (client version) */
    uint8_t c0_version = RTMP_VERSION;
    if (rtmp_client_connection_write_bytes(conn, &c0_version, 1) != 1) {
        IMP_LOG_ERR(TAG, "Failed to send C0 version byte");
        return -1;
    }

    conn->state = RTMP_CLIENT_STATE_HANDSHAKE_C0_SENT;
    IMP_LOG_DBG(TAG, "Sent C0: version=%d", c0_version);

    /* Step 2: Send C1 (client random data) */
    uint8_t c1_data[RTMP_HANDSHAKE_SIZE];

    /* Set timestamp (current time) */
    uint32_t timestamp = (uint32_t)time(NULL);
    c1_data[0] = (timestamp >> 24) & 0xFF;
    c1_data[1] = (timestamp >> 16) & 0xFF;
    c1_data[2] = (timestamp >> 8) & 0xFF;
    c1_data[3] = timestamp & 0xFF;

    /* Set version (zeros for now) */
    c1_data[4] = 0;
    c1_data[5] = 0;
    c1_data[6] = 0;
    c1_data[7] = 0;

    /* Fill with random data */
    for (int i = 8; i < RTMP_HANDSHAKE_SIZE; i++) {
        c1_data[i] = rand() & 0xFF;
    }

    if (rtmp_client_connection_write_bytes(conn, c1_data, RTMP_HANDSHAKE_SIZE) != RTMP_HANDSHAKE_SIZE) {
        IMP_LOG_ERR(TAG, "Failed to send C1 data");
        return -1;
    }

    /* Store C1 data for later verification */
    memcpy(conn->c1_s1_data, c1_data, RTMP_HANDSHAKE_SIZE);

    conn->state = RTMP_CLIENT_STATE_HANDSHAKE_C1_SENT;
    IMP_LOG_DBG(TAG, "Sent C1: %d bytes", RTMP_HANDSHAKE_SIZE);

    /* Step 3: Read S0 (server version) */
    uint8_t s0_version;
    if (rtmp_client_connection_read_bytes(conn, &s0_version, 1) != 1) {
        IMP_LOG_ERR(TAG, "Failed to read S0 version byte");
        return -1;
    }

    if (s0_version != RTMP_VERSION) {
        IMP_LOG_ERR(TAG, "Unsupported RTMP version from server: %d (expected %d)", s0_version, RTMP_VERSION);
        return -1;
    }

    IMP_LOG_DBG(TAG, "Received S0: version=%d", s0_version);

    /* Step 4: Read S1 (server random data) */
    uint8_t s1_data[RTMP_HANDSHAKE_SIZE];
    if (rtmp_client_connection_read_bytes(conn, s1_data, RTMP_HANDSHAKE_SIZE) != RTMP_HANDSHAKE_SIZE) {
        IMP_LOG_ERR(TAG, "Failed to read S1 data");
        return -1;
    }

    IMP_LOG_DBG(TAG, "Received S1: %d bytes", RTMP_HANDSHAKE_SIZE);

    /* Step 5: Read S2 (echo of C1) */
    if (rtmp_client_connection_read_bytes(conn, conn->c2_s2_data, RTMP_HANDSHAKE_SIZE) != RTMP_HANDSHAKE_SIZE) {
        IMP_LOG_ERR(TAG, "Failed to read S2 data");
        return -1;
    }

    IMP_LOG_DBG(TAG, "Received S2: %d bytes", RTMP_HANDSHAKE_SIZE);

    /* Step 6: Send C2 (echo of S1) */
    if (rtmp_client_connection_write_bytes(conn, s1_data, RTMP_HANDSHAKE_SIZE) != RTMP_HANDSHAKE_SIZE) {
        IMP_LOG_ERR(TAG, "Failed to send C2 data");
        return -1;
    }

    conn->state = RTMP_CLIENT_STATE_HANDSHAKE_DONE;
    IMP_LOG_INFO(TAG, "RTMP handshake completed successfully for target: %s", conn->name);

    return 0;
}

/* AMF encoding helpers for RTMP client */
static int rtmp_client_encode_string(uint8_t* buffer, size_t buffer_size, size_t* pos, const char* string)
{
    if (!string) {
        /* Encode null */
        if (*pos + 1 > buffer_size) return -1;
        buffer[(*pos)++] = AMF0_NULL;
        return 0;
    }

    uint16_t length = strlen(string);
    if (*pos + 3 + length > buffer_size) return -1;

    buffer[(*pos)++] = AMF0_STRING;
    buffer[(*pos)++] = (length >> 8) & 0xFF;
    buffer[(*pos)++] = length & 0xFF;
    memcpy(buffer + *pos, string, length);
    *pos += length;

    return 0;
}

static int rtmp_client_encode_number(uint8_t* buffer, size_t buffer_size, size_t* pos, double number)
{
    if (*pos + 9 > buffer_size) return -1;

    buffer[(*pos)++] = AMF0_NUMBER;

    /* Encode double in big-endian format */
    union {
        double d;
        uint64_t i;
    } value;
    value.d = number;

    for (int i = 7; i >= 0; i--) {
        buffer[(*pos)++] = (value.i >> (i * 8)) & 0xFF;
    }

    return 0;
}

static int rtmp_client_encode_object_start(uint8_t* buffer, size_t buffer_size, size_t* pos)
{
    if (*pos + 1 > buffer_size) return -1;
    buffer[(*pos)++] = AMF0_OBJECT;
    return 0;
}

static int rtmp_client_encode_object_property(uint8_t* buffer, size_t buffer_size, size_t* pos, const char* name, uint8_t type, const void* value)
{
    /* Encode property name (without type marker) */
    uint16_t name_length = strlen(name);
    if (*pos + 2 + name_length > buffer_size) return -1;

    buffer[(*pos)++] = (name_length >> 8) & 0xFF;
    buffer[(*pos)++] = name_length & 0xFF;
    memcpy(buffer + *pos, name, name_length);
    *pos += name_length;

    /* Encode property value */
    switch (type) {
        case AMF0_STRING: {
            const char* str = (const char*)value;
            return rtmp_client_encode_string(buffer, buffer_size, pos, str);
        }
        case AMF0_NUMBER: {
            double num = *(const double*)value;
            return rtmp_client_encode_number(buffer, buffer_size, pos, num);
        }
        case AMF0_BOOLEAN: {
            uint8_t bool_val = *(const uint8_t*)value;
            if (*pos + 2 > buffer_size) return -1;
            buffer[(*pos)++] = AMF0_BOOLEAN;
            buffer[(*pos)++] = bool_val ? 1 : 0;
            return 0;
        }
    }

    return -1;
}

static int rtmp_client_encode_object_end(uint8_t* buffer, size_t buffer_size, size_t* pos)
{
    if (*pos + 3 > buffer_size) return -1;
    buffer[(*pos)++] = 0x00; /* Empty string name */
    buffer[(*pos)++] = 0x00;
    buffer[(*pos)++] = AMF0_OBJECT_END;
    return 0;
}

/* RTMP chunk writing for client */
static int rtmp_client_write_chunk(rtmp_client_connection_t* conn, uint8_t chunk_stream_id, uint8_t message_type, uint32_t message_stream_id, const uint8_t* data, uint32_t data_size)
{
    /* Create basic header (Type 0 - full header) */
    uint8_t basic_header = (0 << 6) | chunk_stream_id; /* Type 0, chunk stream ID */

    /* Create message header (11 bytes for Type 0) */
    uint8_t message_header[11];

    /* Timestamp (3 bytes, big-endian) - use 0 for commands */
    message_header[0] = 0;
    message_header[1] = 0;
    message_header[2] = 0;

    /* Message length (3 bytes, big-endian) */
    message_header[3] = (data_size >> 16) & 0xFF;
    message_header[4] = (data_size >> 8) & 0xFF;
    message_header[5] = data_size & 0xFF;

    /* Message type ID (1 byte) */
    message_header[6] = message_type;

    /* Message stream ID (4 bytes, little-endian) */
    message_header[7] = message_stream_id & 0xFF;
    message_header[8] = (message_stream_id >> 8) & 0xFF;
    message_header[9] = (message_stream_id >> 16) & 0xFF;
    message_header[10] = (message_stream_id >> 24) & 0xFF;

    /* Send basic header */
    if (rtmp_client_connection_write_bytes(conn, &basic_header, 1) != 1) {
        return -1;
    }

    /* Send message header */
    if (rtmp_client_connection_write_bytes(conn, message_header, 11) != 11) {
        return -1;
    }

    /* Send message data */
    if (rtmp_client_connection_write_bytes(conn, data, data_size) != (int)data_size) {
        return -1;
    }

    return 0;
}

/* RTMP connect command implementation */
int rtmp_client_connect(rtmp_client_connection_t* conn)
{
    IMP_LOG_DBG(TAG, "Sending RTMP connect command for target: %s", conn->name);

    /* Build connect command AMF message */
    uint8_t command_buffer[1024];
    size_t pos = 0;

    /* Command name: "connect" */
    if (rtmp_client_encode_string(command_buffer, sizeof(command_buffer), &pos, "connect") != 0) {
        IMP_LOG_ERR(TAG, "Failed to encode connect command name");
        return -1;
    }

    /* Transaction ID: 1.0 */
    double transaction_id = 1.0;
    if (rtmp_client_encode_number(command_buffer, sizeof(command_buffer), &pos, transaction_id) != 0) {
        IMP_LOG_ERR(TAG, "Failed to encode transaction ID");
        return -1;
    }

    /* Command object */
    if (rtmp_client_encode_object_start(command_buffer, sizeof(command_buffer), &pos) != 0) {
        IMP_LOG_ERR(TAG, "Failed to encode command object start");
        return -1;
    }

    /* App property */
    if (rtmp_client_encode_object_property(command_buffer, sizeof(command_buffer), &pos, "app", AMF0_STRING, conn->app) != 0) {
        IMP_LOG_ERR(TAG, "Failed to encode app property");
        return -1;
    }

    /* Type property */
    if (rtmp_client_encode_object_property(command_buffer, sizeof(command_buffer), &pos, "type", AMF0_STRING, "nonprivate") != 0) {
        IMP_LOG_ERR(TAG, "Failed to encode type property");
        return -1;
    }

    /* Flash version */
    if (rtmp_client_encode_object_property(command_buffer, sizeof(command_buffer), &pos, "flashVer", AMF0_STRING, "FMLE/3.0") != 0) {
        IMP_LOG_ERR(TAG, "Failed to encode flashVer property");
        return -1;
    }

    /* TC URL */
    char tc_url[512];
    snprintf(tc_url, sizeof(tc_url), "rtmp://%s:%d/%s", conn->host, conn->port, conn->app);
    if (rtmp_client_encode_object_property(command_buffer, sizeof(command_buffer), &pos, "tcUrl", AMF0_STRING, tc_url) != 0) {
        IMP_LOG_ERR(TAG, "Failed to encode tcUrl property");
        return -1;
    }

    /* Object end */
    if (rtmp_client_encode_object_end(command_buffer, sizeof(command_buffer), &pos) != 0) {
        IMP_LOG_ERR(TAG, "Failed to encode object end");
        return -1;
    }

    /* Send connect command */
    if (rtmp_client_write_chunk(conn, 3, RTMP_MSG_COMMAND_AMF0, 0, command_buffer, pos) != 0) {
        IMP_LOG_ERR(TAG, "Failed to send connect command");
        return -1;
    }

    IMP_LOG_INFO(TAG, "Sent RTMP connect command for target: %s (app: %s)", conn->name, conn->app);

    /* Wait for server response */
    usleep(200000); /* 200ms delay to allow server to respond */

    /* Read available server response - should be _result for connect */
    uint8_t response_buffer[2048];
    int bytes_read = rtmp_client_connection_read_available(conn, response_buffer, sizeof(response_buffer));
    if (bytes_read < 0) {
        IMP_LOG_ERR(TAG, "Error reading response to connect command for target: %s", conn->name);
        return -1;
    }

    if (bytes_read == 0) {
        IMP_LOG_WARN(TAG, "No response received to connect command for target: %s (server may not send immediate response)", conn->name);
        /* Some servers don't send immediate responses, continue anyway */
    } else {
        IMP_LOG_DBG(TAG, "Received %d bytes response to connect command for target: %s", bytes_read, conn->name);

        /* Display response content for debugging */
        char hex_str[512] = {0};
        int display_bytes = (bytes_read > 100) ? 100 : bytes_read;
        for (int i = 0; i < display_bytes; i++) {
            snprintf(hex_str + (i * 3), 4, "%02x ", response_buffer[i]);
        }
        IMP_LOG_INFO(TAG, "Connect response hex (%d bytes): %s", bytes_read, hex_str);

        /* Show full response if larger */
        if (bytes_read > 100) {
            char hex_str2[512] = {0};
            int start_byte = 100;
            int remaining = bytes_read - start_byte;
            int show_bytes = (remaining > 100) ? 100 : remaining;
            for (int i = 0; i < show_bytes; i++) {
                snprintf(hex_str2 + (i * 3), 4, "%02x ", response_buffer[start_byte + i]);
            }
            IMP_LOG_INFO(TAG, "Connect response hex (cont): %s", hex_str2);
        }

        /* Try to interpret as text if printable */
        bool has_text = false;
        for (int i = 0; i < display_bytes; i++) {
            if (response_buffer[i] >= 32 && response_buffer[i] <= 126) {
                has_text = true;
                break;
            }
        }
        if (has_text) {
            char text_str[256] = {0};
            int text_pos = 0;
            for (int i = 0; i < display_bytes && text_pos < 255; i++) {
                if (response_buffer[i] >= 32 && response_buffer[i] <= 126) {
                    text_str[text_pos++] = response_buffer[i];
                } else if (response_buffer[i] == 0) {
                    text_str[text_pos++] = '|';
                }
            }
            IMP_LOG_INFO(TAG, "Connect response text: '%s'", text_str);
        }
    }

    conn->state = RTMP_CLIENT_STATE_CONNECTED;

    return 0;
}

int rtmp_client_create_stream(rtmp_client_connection_t* conn)
{
    IMP_LOG_DBG(TAG, "Sending RTMP createStream command for target: %s", conn->name);

    /* Build createStream command AMF message */
    uint8_t command_buffer[256];
    size_t pos = 0;

    /* Command name: "createStream" */
    if (rtmp_client_encode_string(command_buffer, sizeof(command_buffer), &pos, "createStream") != 0) {
        IMP_LOG_ERR(TAG, "Failed to encode createStream command name");
        return -1;
    }

    /* Transaction ID: 2.0 */
    double transaction_id = 2.0;
    if (rtmp_client_encode_number(command_buffer, sizeof(command_buffer), &pos, transaction_id) != 0) {
        IMP_LOG_ERR(TAG, "Failed to encode transaction ID");
        return -1;
    }

    /* Command object: null */
    if (pos + 1 > sizeof(command_buffer)) {
        IMP_LOG_ERR(TAG, "Buffer too small for null object");
        return -1;
    }
    command_buffer[pos++] = AMF0_NULL;

    /* Send createStream command */
    if (rtmp_client_write_chunk(conn, 3, RTMP_MSG_COMMAND_AMF0, 0, command_buffer, pos) != 0) {
        IMP_LOG_ERR(TAG, "Failed to send createStream command");
        return -1;
    }

    IMP_LOG_INFO(TAG, "Sent RTMP createStream command for target: %s", conn->name);

    /* Wait for server response */
    usleep(200000); /* 200ms delay to allow server to respond */

    /* Read available server response - should be _result with stream ID */
    uint8_t response_buffer[2048];
    int bytes_read = rtmp_client_connection_read_available(conn, response_buffer, sizeof(response_buffer));
    if (bytes_read < 0) {
        IMP_LOG_ERR(TAG, "Error reading response to createStream command for target: %s", conn->name);
        return -1;
    }

    if (bytes_read == 0) {
        IMP_LOG_WARN(TAG, "No response received to createStream command for target: %s (server may not send immediate response)", conn->name);
        /* Some servers don't send immediate responses, use default stream ID */
        conn->stream_id = 1.0;
    } else {
        IMP_LOG_DBG(TAG, "Received %d bytes response to createStream command for target: %s", bytes_read, conn->name);

        /* Display response content for debugging */
        char hex_str[256] = {0};
        int display_bytes = (bytes_read > 50) ? 50 : bytes_read;
        for (int i = 0; i < display_bytes; i++) {
            snprintf(hex_str + (i * 3), 4, "%02x ", response_buffer[i]);
        }
        IMP_LOG_INFO(TAG, "CreateStream response hex: %s", hex_str);

        /* Try to extract stream ID from response */
        if (bytes_read >= 20) {
            /* Look for the last AMF number (0x00) which should be the stream ID */
            int last_number_pos = -1;
            for (int i = 0; i < bytes_read - 8; i++) {
                if (response_buffer[i] == 0x00) {
                    last_number_pos = i;
                }
            }

            if (last_number_pos >= 0) {
                /* Extract 8-byte double (big-endian) */
                uint64_t stream_id_bits = 0;
                for (int j = 0; j < 8; j++) {
                    stream_id_bits = (stream_id_bits << 8) | response_buffer[last_number_pos + 1 + j];
                }

                /* Convert bits to double using union to avoid strict aliasing issues */
                union {
                    uint64_t bits;
                    double value;
                } converter;
                converter.bits = stream_id_bits;

                if (converter.value > 0 && converter.value < 1000) {
                    conn->stream_id = converter.value;
                    IMP_LOG_INFO(TAG, "Extracted stream ID: %.0f", conn->stream_id);
                } else {
                    IMP_LOG_WARN(TAG, "Invalid stream ID extracted: %.0f, using default", converter.value);
                    conn->stream_id = 1.0;
                }
            } else {
                IMP_LOG_WARN(TAG, "No stream ID found in response, using default");
                conn->stream_id = 1.0;
            }
        } else {
            conn->stream_id = 1.0;
        }
    }

    conn->state = RTMP_CLIENT_STATE_STREAM_CREATED;

    return 0;
}

int rtmp_client_publish(rtmp_client_connection_t* conn)
{
    IMP_LOG_DBG(TAG, "Sending RTMP publish command for target: %s", conn->name);

    /* Use stream key if stream name is empty */
    const char* publish_name = (strlen(conn->stream) > 0) ? conn->stream : conn->stream_key;
    if (strlen(publish_name) == 0) {
        IMP_LOG_ERR(TAG, "No stream name or stream key provided for publish");
        return -1;
    }

    /* Build publish command AMF message */
    uint8_t command_buffer[512];
    size_t pos = 0;

    /* Command name: "publish" */
    if (rtmp_client_encode_string(command_buffer, sizeof(command_buffer), &pos, "publish") != 0) {
        IMP_LOG_ERR(TAG, "Failed to encode publish command name");
        return -1;
    }

    /* Transaction ID: 0.0 (no response expected) */
    double transaction_id = 0.0;
    if (rtmp_client_encode_number(command_buffer, sizeof(command_buffer), &pos, transaction_id) != 0) {
        IMP_LOG_ERR(TAG, "Failed to encode transaction ID");
        return -1;
    }

    /* Command object: null */
    if (pos + 1 > sizeof(command_buffer)) {
        IMP_LOG_ERR(TAG, "Buffer too small for null object");
        return -1;
    }
    command_buffer[pos++] = AMF0_NULL;

    /* Publishing name (stream key) */
    if (rtmp_client_encode_string(command_buffer, sizeof(command_buffer), &pos, publish_name) != 0) {
        IMP_LOG_ERR(TAG, "Failed to encode publishing name");
        return -1;
    }

    /* Publishing type: "live" */
    if (rtmp_client_encode_string(command_buffer, sizeof(command_buffer), &pos, "live") != 0) {
        IMP_LOG_ERR(TAG, "Failed to encode publishing type");
        return -1;
    }

    /* Send publish command on the stream */
    uint32_t stream_id = (uint32_t)conn->stream_id;
    if (rtmp_client_write_chunk(conn, 8, RTMP_MSG_COMMAND_AMF0, stream_id, command_buffer, pos) != 0) {
        IMP_LOG_ERR(TAG, "Failed to send publish command");
        return -1;
    }

    IMP_LOG_INFO(TAG, "Sent RTMP publish command for target: %s (stream: %s)", conn->name, publish_name);

    /* Wait for server response */
    usleep(200000); /* 200ms delay to allow server to respond */

    /* Read available server response - should be onStatus for publish */
    uint8_t response_buffer[2048];
    int bytes_read = rtmp_client_connection_read_available(conn, response_buffer, sizeof(response_buffer));
    if (bytes_read < 0) {
        IMP_LOG_ERR(TAG, "Error reading response to publish command for target: %s", conn->name);
        return -1;
    }

    if (bytes_read == 0) {
        /* Try reading again with a longer timeout for publish response */
        bytes_read = rtmp_client_connection_read_available(conn, response_buffer, sizeof(response_buffer));
        if (bytes_read == 0) {
            IMP_LOG_WARN(TAG, "No response received to publish command for target: %s (server may not send immediate response)", conn->name);
            /* Some servers don't send immediate responses, continue anyway */
        }
    }

    if (bytes_read > 0) {
        IMP_LOG_DBG(TAG, "Received %d bytes response to publish command for target: %s", bytes_read, conn->name);

        /* Display complete response content for debugging */
        IMP_LOG_INFO(TAG, "Publish response: %d bytes total", bytes_read);

        /* Show response in chunks of 50 bytes */
        for (int chunk = 0; chunk < bytes_read; chunk += 50) {
            char hex_str[256] = {0};
            int chunk_size = ((bytes_read - chunk) > 50) ? 50 : (bytes_read - chunk);
            for (int i = 0; i < chunk_size; i++) {
                snprintf(hex_str + (i * 3), 4, "%02x ", response_buffer[chunk + i]);
            }
            IMP_LOG_INFO(TAG, "Publish response hex [%d-%d]: %s", chunk, chunk + chunk_size - 1, hex_str);
        }

        /* Try to interpret as text for status messages */
        bool has_text = false;
        for (int i = 0; i < bytes_read; i++) {
            if (response_buffer[i] >= 32 && response_buffer[i] <= 126) {
                has_text = true;
                break;
            }
        }
        if (has_text) {
            char text_str[256] = {0};
            int text_pos = 0;
            for (int i = 0; i < bytes_read && text_pos < 255; i++) {
                if (response_buffer[i] >= 32 && response_buffer[i] <= 126) {
                    text_str[text_pos++] = response_buffer[i];
                } else if (response_buffer[i] == 0) {
                    text_str[text_pos++] = '|';
                }
            }
            IMP_LOG_INFO(TAG, "Publish response text: '%s'", text_str);
        }

        /* Look for common RTMP status codes */
        if (bytes_read > 20) {
            /* Look for "NetStream.Publish.Start" or error messages */
            for (int i = 0; i < bytes_read - 10; i++) {
                if (strncmp((char*)&response_buffer[i], "NetStream", 9) == 0) {
                    char status[64] = {0};
                    int status_len = 0;
                    for (int j = i; j < bytes_read && j < i + 63; j++) {
                        if (response_buffer[j] >= 32 && response_buffer[j] <= 126) {
                            status[status_len++] = response_buffer[j];
                        } else if (status_len > 0) {
                            break;
                        }
                    }
                    if (status_len > 0) {
                        IMP_LOG_INFO(TAG, "RTMP Status: %s", status);
                    }
                    break;
                }
            }
        }
    }

    return 0;
}

/* Cache SPS/PPS from frame for later use */
static void cache_sps_pps_from_frame(const uint8_t* frame_data, uint32_t frame_size,
                                     uint8_t** cached_sps, uint32_t* cached_sps_size,
                                     uint8_t** cached_pps, uint32_t* cached_pps_size)
{
    /* Use the same SPS/PPS detection logic as the working RTSP server */
    for (size_t offset = 0; offset < frame_size - 4; offset++) {
        bool found_start_code = false;
        size_t nal_start = 0;

        /* Check for 4-byte start code (00 00 00 01) */
        if (frame_data[offset] == 0x00 && frame_data[offset + 1] == 0x00 &&
            frame_data[offset + 2] == 0x00 && frame_data[offset + 3] == 0x01) {
            found_start_code = true;
            nal_start = offset + 4;
        }
        /* Check for 3-byte start code (00 00 01) */
        else if (offset < frame_size - 3 &&
                 frame_data[offset] == 0x00 && frame_data[offset + 1] == 0x00 &&
                 frame_data[offset + 2] == 0x01) {
            found_start_code = true;
            nal_start = offset + 3;
        }

        if (found_start_code && nal_start < frame_size) {
            uint8_t nal_type = frame_data[nal_start] & 0x1F;

            if (nal_type == 7 || nal_type == 8) { /* SPS or PPS */
                /* Find end of this NAL unit */
                size_t nal_end = frame_size;

                for (size_t j = nal_start + 1; j < frame_size - 3; j++) {
                    if ((frame_data[j] == 0x00 && frame_data[j + 1] == 0x00 &&
                         frame_data[j + 2] == 0x00 && frame_data[j + 3] == 0x01) ||
                        (frame_data[j] == 0x00 && frame_data[j + 1] == 0x00 &&
                         frame_data[j + 2] == 0x01)) {
                        nal_end = j;
                        break;
                    }
                }

                uint32_t nal_size = nal_end - nal_start;

                if (nal_type == 7 && !*cached_sps) { /* SPS */
                    *cached_sps = malloc(nal_size);
                    if (*cached_sps) {
                        memcpy(*cached_sps, &frame_data[nal_start], nal_size);
                        *cached_sps_size = nal_size;
                        IMP_LOG_INFO(TAG, "Extracted SPS from frame: %u bytes", nal_size);
                    }
                } else if (nal_type == 8 && !*cached_pps) { /* PPS */
                    *cached_pps = malloc(nal_size);
                    if (*cached_pps) {
                        memcpy(*cached_pps, &frame_data[nal_start], nal_size);
                        *cached_pps_size = nal_size;
                        IMP_LOG_INFO(TAG, "Extracted PPS from frame: %u bytes", nal_size);
                    }
                }

                /* Skip to end of this NAL unit */
                offset = nal_end - 1;
            }
        }
    }
}

/* Send cached AVC sequence header */
static int send_cached_avc_sequence_header(rtmp_client_connection_t* conn,
                                          const uint8_t* sps_data, uint32_t sps_size,
                                          const uint8_t* pps_data, uint32_t pps_size)
{
    if (!sps_data || !pps_data) {
        return -1;
    }

    /* Build AVC sequence header with cached SPS/PPS */
    /* Size calculation: 5 (video tag) + 6 (AVC config start) + 2 (SPS length) + sps_size + 3 (PPS count + length) + pps_size */
    uint32_t header_size = 5 + 6 + 2 + sps_size + 3 + pps_size;
    uint8_t* avc_header = malloc(header_size);
    if (!avc_header) {
        IMP_LOG_ERR(TAG, "Failed to allocate AVC header buffer");
        return -1;
    }

    uint8_t* ptr = avc_header;

    /* AVC sequence header format */
    *ptr++ = 0x17; /* Key frame + AVC */
    *ptr++ = 0x00; /* AVC sequence header */
    *ptr++ = 0x00; /* Composition time (3 bytes) */
    *ptr++ = 0x00;
    *ptr++ = 0x00;

    /* AVCDecoderConfigurationRecord */
    *ptr++ = 0x01; /* configurationVersion */
    *ptr++ = sps_data[1]; /* AVCProfileIndication */
    *ptr++ = sps_data[2]; /* profile_compatibility */
    *ptr++ = sps_data[3]; /* AVCLevelIndication */
    *ptr++ = 0xFF; /* lengthSizeMinusOne (4 bytes) */
    *ptr++ = 0xE1; /* numOfSequenceParameterSets */

    /* SPS */
    *ptr++ = (sps_size >> 8) & 0xFF;
    *ptr++ = sps_size & 0xFF;
    memcpy(ptr, sps_data, sps_size);
    ptr += sps_size;

    /* PPS */
    *ptr++ = 0x01; /* numOfPictureParameterSets */
    *ptr++ = (pps_size >> 8) & 0xFF;
    *ptr++ = pps_size & 0xFF;
    memcpy(ptr, pps_data, pps_size);

    /* Send the AVC sequence header directly as RTMP video message */
    int result = rtmp_client_send_chunked_message(conn, 6, RTMP_MSG_VIDEO, 0, avc_header, header_size);
    free(avc_header);

    if (result == 0) {
        IMP_LOG_INFO(TAG, "Sent cached AVC sequence header: %u bytes (SPS: %u, PPS: %u)", header_size, sps_size, pps_size);
    }

    return result;
}

/* Extract SPS/PPS from keyframe and send AVC sequence header */
static int send_avc_sequence_header_from_frame(rtmp_client_connection_t* conn, const uint8_t* frame_data, uint32_t frame_size)
{
    const uint8_t* sps_data = NULL;
    uint32_t sps_size = 0;
    const uint8_t* pps_data = NULL;
    uint32_t pps_size = 0;

    /* Find SPS and PPS NAL units in the frame */
    for (size_t offset = 0; offset < frame_size - 4; offset++) {
        /* Look for 4-byte start code: 0x00 0x00 0x00 0x01 */
        if (frame_data[offset] == 0x00 && frame_data[offset + 1] == 0x00 &&
            frame_data[offset + 2] == 0x00 && frame_data[offset + 3] == 0x01) {

            uint8_t nal_type = frame_data[offset + 4] & 0x1F;

            /* Find end of this NAL unit */
            size_t nal_start = offset + 4;
            size_t nal_end = frame_size;

            for (size_t i = nal_start + 3; i < frame_size; i++) {
                if (frame_data[i-3] == 0x00 && frame_data[i-2] == 0x00 &&
                    frame_data[i-1] == 0x00 && frame_data[i] == 0x01) {
                    nal_end = i - 3;
                    break;
                }
            }

            uint32_t nal_size = nal_end - nal_start;

            if (nal_type == 7 && !sps_data) { /* SPS */
                sps_data = &frame_data[nal_start];
                sps_size = nal_size;
                /* SPS found */
            } else if (nal_type == 8 && !pps_data) { /* PPS */
                pps_data = &frame_data[nal_start];
                pps_size = nal_size;
                /* PPS found */
            }

            /* Skip to end of this NAL unit */
            offset = nal_end - 1;
        }
    }

    if (!sps_data || !pps_data) {
        IMP_LOG_WARN(TAG, "Could not find SPS/PPS in keyframe (sps=%p, pps=%p, frame_size=%u)", sps_data, pps_data, frame_size);
        return -1;
    }

    IMP_LOG_INFO(TAG, "Found SPS/PPS in keyframe: SPS=%u bytes, PPS=%u bytes", sps_size, pps_size);

    /* Build AVC sequence header with proper video tag */
    uint32_t header_size = 5 + 6 + sps_size + 3 + pps_size;
    uint8_t* avc_header = malloc(header_size);
    if (!avc_header) {
        IMP_LOG_ERR(TAG, "Failed to allocate AVC header buffer");
        return -1;
    }

    size_t pos = 0;

    /* Video tag header: 0x17 (keyframe + AVC) */
    avc_header[pos++] = 0x17;

    /* AVC packet type: 0 (sequence header) */
    avc_header[pos++] = 0x00;

    /* Composition time: 0x00 0x00 0x00 */
    avc_header[pos++] = 0x00;
    avc_header[pos++] = 0x00;
    avc_header[pos++] = 0x00;

    /* AVC decoder configuration record (ISO 14496-15) */
    avc_header[pos++] = 0x01; /* configurationVersion */

    /* Validate SPS data before using it */
    if (sps_size < 4) {
        IMP_LOG_ERR(TAG, "Invalid SPS size: %u (minimum 4 bytes required)", sps_size);
        free(avc_header);
        return -1;
    }

    avc_header[pos++] = sps_data[1]; /* AVCProfileIndication */
    avc_header[pos++] = sps_data[2]; /* profile_compatibility */
    avc_header[pos++] = sps_data[3]; /* AVCLevelIndication */
    avc_header[pos++] = 0xFF; /* lengthSizeMinusOne (4 bytes) | reserved (111111) */
    avc_header[pos++] = 0xE1; /* reserved (111) | numOfSequenceParameterSets (1) */

    /* SPS length (big endian, 16-bit) */
    avc_header[pos++] = (sps_size >> 8) & 0xFF;
    avc_header[pos++] = sps_size & 0xFF;

    /* SPS data */
    memcpy(&avc_header[pos], sps_data, sps_size);
    pos += sps_size;

    /* numOfPictureParameterSets (1) */
    avc_header[pos++] = 0x01;

    /* PPS length (big endian, 16-bit) */
    avc_header[pos++] = (pps_size >> 8) & 0xFF;
    avc_header[pos++] = pps_size & 0xFF;

    /* PPS data */
    memcpy(&avc_header[pos], pps_data, pps_size);
    pos += pps_size;

    /* Debug: Log AVC sequence header details */
    IMP_LOG_INFO(TAG, "AVC sequence header: total_size=%zu, sps_size=%u, pps_size=%u", pos, sps_size, pps_size);

    /* Send as RTMP video message with timestamp 0 and chunk stream ID 6 */
    int ret = rtmp_client_send_chunked_message(conn, 6, RTMP_MSG_VIDEO, 0, avc_header, pos);

    free(avc_header);

    if (ret < 0) {
        IMP_LOG_ERR(TAG, "Failed to send AVC sequence header");
        return -1;
    }

    IMP_LOG_INFO(TAG, "Sent AVC sequence header: %zu bytes (SPS: %u, PPS: %u)", pos, sps_size, pps_size);
    return 0;
}

/* Send AAC configuration */
static int send_aac_config(rtmp_client_connection_t* conn)
{
    /* AAC AudioSpecificConfig for 44.1kHz stereo */
    uint8_t aac_config[] = {
        0xAF, /* Audio tag: AAC, 44.1kHz, 16-bit, stereo */
        0x00, /* AAC sequence header */
        0x12, 0x10 /* AudioSpecificConfig: AAC-LC, 44.1kHz, stereo */
    };

    /* Send as RTMP audio message with timestamp 0 and chunk stream ID 4 */
    int ret = rtmp_client_send_chunked_message(conn, 4, RTMP_MSG_AUDIO, 0, aac_config, sizeof(aac_config));
    if (ret < 0) {
        IMP_LOG_ERR(TAG, "Failed to send AAC configuration");
        return -1;
    }

    IMP_LOG_INFO(TAG, "Sent AAC configuration: %zu bytes", sizeof(aac_config));
    return 0;
}

/* Send RTMP video packet with proper formatting */
static int rtmp_send_video_packet(rtmp_client_connection_t* conn, const uint8_t* data, uint32_t size, uint32_t timestamp, bool is_sequence_header)
{
    if (!conn || !data || size == 0) {
        return -1;
    }

    if (is_sequence_header) {
        /* For sequence headers, data is already in proper format */
        uint32_t message_size = size + 1; /* +1 for video tag byte */
        uint8_t* video_message = malloc(message_size);
        if (!video_message) {
            IMP_LOG_ERR(TAG, "Failed to allocate video message buffer");
            return -1;
        }

        video_message[0] = 0x17; /* Keyframe + AVC */
        memcpy(video_message + 1, data, size);

        int result = rtmp_client_send_chunked_message(conn, 6, RTMP_MSG_VIDEO, timestamp, video_message, message_size);
        free(video_message);
        return result;
    } else {
        /* For regular frames, convert from Annex B (start codes) to AVCC format (length prefixes) */
        /* This is critical for RTMP compatibility */

        /* Estimate output size (worst case: same size + some overhead for length prefixes) */
        uint32_t output_size = size + 1024; /* Extra space for length prefixes */
        uint8_t* video_message = malloc(output_size);
        if (!video_message) {
            IMP_LOG_ERR(TAG, "Failed to allocate video message buffer");
            return -1;
        }

        /* Determine frame type by scanning NAL units */
        bool is_keyframe = false;
        for (size_t i = 0; i < size - 4; i++) {
            if (data[i] == 0x00 && data[i+1] == 0x00 && data[i+2] == 0x00 && data[i+3] == 0x01) {
                uint8_t nal_type = data[i+4] & 0x1F;
                if (nal_type == 5) { /* IDR frame */
                    is_keyframe = true;
                    break;
                }
            }
        }

        /* Video tag byte: frame type (4 bits) + codec ID (4 bits) */
        video_message[0] = is_keyframe ? 0x17 : 0x27; /* Keyframe or Inter frame + AVC */

        /* AVC packet type: 1 (NALU) */
        video_message[1] = 0x01;

        /* Composition time: 0x00 0x00 0x00 */
        video_message[2] = 0x00;
        video_message[3] = 0x00;
        video_message[4] = 0x00;

        /* Convert Annex B format to AVCC format */
        size_t output_pos = 5; /* Start after the 5-byte header */
        size_t input_pos = 0;

        while (input_pos < size) {
            /* Find start code */
            if (input_pos + 4 <= size &&
                data[input_pos] == 0x00 && data[input_pos+1] == 0x00 &&
                data[input_pos+2] == 0x00 && data[input_pos+3] == 0x01) {

                /* Skip start code */
                input_pos += 4;

                /* Find end of NAL unit */
                size_t nal_start = input_pos;
                size_t nal_end = size;

                for (size_t i = nal_start + 3; i < size; i++) {
                    if (data[i-3] == 0x00 && data[i-2] == 0x00 &&
                        data[i-1] == 0x00 && data[i] == 0x01) {
                        nal_end = i - 3;
                        break;
                    }
                }

                uint32_t nal_size = nal_end - nal_start;

                /* Check buffer space */
                if (output_pos + 4 + nal_size > output_size) {
                    IMP_LOG_ERR(TAG, "Video message buffer too small");
                    free(video_message);
                    return -1;
                }

                /* Write NAL size (big endian) */
                video_message[output_pos++] = (nal_size >> 24) & 0xFF;
                video_message[output_pos++] = (nal_size >> 16) & 0xFF;
                video_message[output_pos++] = (nal_size >> 8) & 0xFF;
                video_message[output_pos++] = nal_size & 0xFF;

                /* Write NAL data */
                memcpy(&video_message[output_pos], &data[nal_start], nal_size);
                output_pos += nal_size;

                input_pos = nal_end;
            } else {
                input_pos++;
            }
        }

        /* Send as RTMP video message with chunk stream ID 6 */
        int result = rtmp_client_send_chunked_message(conn, 6, RTMP_MSG_VIDEO, timestamp, video_message, output_pos);

        free(video_message);
        return result;
    }
}

/* RTMP client connection thread */
static void* rtmp_client_connection_thread(void* arg)
{
    rtmp_client_connection_t* conn = (rtmp_client_connection_t*)arg;

    IMP_LOG_INFO(TAG, "RTMP connection thread started for target: %s", conn->name);

    /* Parse URL once and store components in connection structure */
    if (rtmp_client_parse_url(conn->url, conn->host, &conn->port, conn->app, conn->stream, &conn->use_tls) != 0) {
        IMP_LOG_ERR(TAG, "Failed to parse RTMP URL: %s", conn->url);
        return NULL;
    }

    IMP_LOG_DBG(TAG, "Parsed URL: host=%s, port=%d, app=%s, stream=%s, tls=%s",
                conn->host, conn->port, conn->app, conn->stream, conn->use_tls ? "yes" : "no");

    while (conn->thread_running) {

        /* Check for TLS requirement */
        if (conn->use_tls) {
#if defined(RTMPS_BACKEND_OPENSSL) || defined(RTMPS_BACKEND_MBEDTLS)
            IMP_LOG_INFO(TAG, "RTMPS (TLS) detected for target: %s - will establish secure connection", conn->name);
#else
            IMP_LOG_WARN(TAG, "RTMPS (TLS) detected for target: %s - TLS support not compiled, attempting plain connection", conn->name);
            IMP_LOG_WARN(TAG, "This connection will likely fail. Please recompile with TLS backend enabled.");
#endif
        }

        /* Attempt to connect */
        conn->state = RTMP_CLIENT_STATE_CONNECTING;
        conn->socket_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (conn->socket_fd < 0) {
            IMP_LOG_ERR(TAG, "Failed to create socket for target: %s", conn->name);
            sleep(30);
            continue;
        }

        /* Set socket timeout */
        struct timeval timeout;
        timeout.tv_sec = 30;
        timeout.tv_usec = 0;
        setsockopt(conn->socket_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        setsockopt(conn->socket_fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

        /* Resolve hostname */
        struct hostent* he = gethostbyname(conn->host);
        if (!he) {
            IMP_LOG_ERR(TAG, "Failed to resolve hostname: %s", conn->host);
            close(conn->socket_fd);
            conn->socket_fd = -1;
            sleep(30);
            continue;
        }

        /* Connect to server */
        struct sockaddr_in server_addr;
        memset(&server_addr, 0, sizeof(server_addr));
        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons(conn->port);
        memcpy(&server_addr.sin_addr, he->h_addr_list[0], he->h_length);

        if (connect(conn->socket_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) != 0) {
            IMP_LOG_ERR(TAG, "Failed to connect to %s:%d for target: %s", conn->host, conn->port, conn->name);
            close(conn->socket_fd);
            conn->socket_fd = -1;
            sleep(30);
            continue;
        }

        IMP_LOG_INFO(TAG, "Connected to %s:%d for target: %s", conn->host, conn->port, conn->name);

        /* Set up TLS if required */
        if (conn->use_tls) {
#if defined(RTMPS_BACKEND_OPENSSL) || defined(RTMPS_BACKEND_MBEDTLS)
            if (rtmp_client_tls_setup(conn, conn->host) != 0) {
                IMP_LOG_ERR(TAG, "TLS setup failed for target: %s", conn->name);
                close(conn->socket_fd);
                conn->socket_fd = -1;
                sleep(30);
                continue;
            }

            if (rtmp_client_tls_handshake(conn) != 0) {
                IMP_LOG_ERR(TAG, "TLS handshake failed for target: %s", conn->name);
                close(conn->socket_fd);
                conn->socket_fd = -1;
                sleep(30);
                continue;
            }

            IMP_LOG_INFO(TAG, "TLS connection established for target: %s", conn->name);
#else
            IMP_LOG_ERR(TAG, "TLS required but not compiled for target: %s", conn->name);
            close(conn->socket_fd);
            conn->socket_fd = -1;
            sleep(30);
            continue;
#endif
        }

        /* Perform RTMP handshake */
        if (rtmp_client_handshake_process(conn) != 0) {
            IMP_LOG_ERR(TAG, "RTMP handshake failed for target: %s", conn->name);
            close(conn->socket_fd);
            conn->socket_fd = -1;
            sleep(30);
            continue;
        }

        /* Send connect command */
        if (rtmp_client_connect(conn) != 0) {
            IMP_LOG_ERR(TAG, "RTMP connect failed for target: %s", conn->name);
            close(conn->socket_fd);
            conn->socket_fd = -1;
            sleep(30);
            continue;
        }

        /* Create stream */
        if (rtmp_client_create_stream(conn) != 0) {
            IMP_LOG_ERR(TAG, "RTMP createStream failed for target: %s", conn->name);
            close(conn->socket_fd);
            conn->socket_fd = -1;
            sleep(30);
            continue;
        }

        /* Start publishing */
        if (rtmp_client_publish(conn) != 0) {
            IMP_LOG_ERR(TAG, "RTMP publish failed for target: %s", conn->name);
            close(conn->socket_fd);
            conn->socket_fd = -1;
            sleep(30);
            continue;
        }

        /* Connection established successfully */
        conn->state = RTMP_CLIENT_STATE_PUBLISHING;
        conn->connected_time = time(NULL);
        conn->retry_count = 0;

        /* Initialize AVC header wait timer */
        extern uint64_t get_monotonic_time_us(void);
        conn->avc_wait_start_us = (unsigned long)get_monotonic_time_us();

        IMP_LOG_INFO(TAG, "RTMP publishing started for target: %s", conn->name);
        IMP_LOG_INFO(TAG, "Connection state set to PUBLISHING, thread_running=%s",
                    conn->thread_running ? "true" : "false");

        /* Send chunk size message - use configured chunk size for compatibility */
        uint8_t chunk_size_msg[4];
        rtmp_client_t* client = rtmp_client_module_get_client();
        uint32_t new_chunk_size = client ? client->config.connection.chunk_size : RTMP_DEFAULT_CHUNK_SIZE; /* Use configured chunk size */
        chunk_size_msg[0] = (new_chunk_size >> 24) & 0xFF;
        chunk_size_msg[1] = (new_chunk_size >> 16) & 0xFF;
        chunk_size_msg[2] = (new_chunk_size >> 8) & 0xFF;
        chunk_size_msg[3] = new_chunk_size & 0xFF;

        if (rtmp_client_send_chunked_message(conn, 2, RTMP_MSG_SET_CHUNK_SIZE, 0, chunk_size_msg, 4) < 0) {
            IMP_LOG_ERR(TAG, "Failed to send chunk size message for target: %s", conn->name);
            conn->state = RTMP_CLIENT_STATE_ERROR;
            return NULL;
        }

        conn->chunk_size_out = new_chunk_size;
        IMP_LOG_INFO(TAG, "Sent chunk size message: %u bytes for target: %s (configured size for compatibility)", new_chunk_size, conn->name);

        /* Send AAC configuration for audio streams */
        if (send_aac_config(conn) < 0) {
            IMP_LOG_ERR(TAG, "Failed to send AAC configuration for target: %s", conn->name);
            conn->state = RTMP_CLIENT_STATE_ERROR;
            return NULL;
        }

        /* Keep connection alive and handle any incoming messages */
        while (conn->thread_running && conn->state == RTMP_CLIENT_STATE_PUBLISHING) {
            /* TODO: Handle incoming RTMP messages if needed */
            /* For now, just sleep and check connection status */
            sleep(1);

            /* Check if socket is still connected */
            int error = 0;
            socklen_t len = sizeof(error);
            if (getsockopt(conn->socket_fd, SOL_SOCKET, SO_ERROR, &error, &len) != 0 || error != 0) {
                IMP_LOG_WARN(TAG, "Socket error detected for target: %s", conn->name);
                break;
            }
        }

        /* Connection lost or stopped */
#if defined(RTMPS_BACKEND_OPENSSL) || defined(RTMPS_BACKEND_MBEDTLS)
        if (conn->use_tls) {
            rtmp_client_tls_cleanup(conn);
        }
#endif
        if (conn->socket_fd >= 0) {
            close(conn->socket_fd);
            conn->socket_fd = -1;
        }

        conn->state = RTMP_CLIENT_STATE_DISCONNECTED;

        /* If we were publishing and lost connection, try to reconnect after a delay */
        if (conn->thread_running && conn->state == RTMP_CLIENT_STATE_DISCONNECTED) {
            IMP_LOG_WARN(TAG, "Connection lost for target: %s, will attempt reconnection in 5 seconds", conn->name);
            sleep(5);
            /* The main loop will attempt to reconnect */
        }

        if (conn->thread_running) {
            IMP_LOG_WARN(TAG, "Connection lost for target: %s, will retry in 30 seconds", conn->name);
            sleep(30);
        }
    }

    IMP_LOG_INFO(TAG, "RTMP connection thread finished for target: %s", conn->name);
    return NULL;
}

/* URL parsing helper function */
static int rtmp_client_parse_url(const char* url, char* host, int* port, char* app, char* stream, bool* use_tls)
{
    if (!url || !host || !port || !app || !stream || !use_tls) {
        return -1;
    }

    const char* url_part;

    /* Expected format: rtmp://host:port/app/stream or rtmps://host:port/app/stream */
    if (strncmp(url, "rtmp://", 7) == 0) {
        url_part = url + 7; /* Skip "rtmp://" */
        *use_tls = false;
    } else if (strncmp(url, "rtmps://", 8) == 0) {
        url_part = url + 8; /* Skip "rtmps://" */
        *use_tls = true;
    } else {
        IMP_LOG_ERR(TAG, "Invalid RTMP URL format: %s (must start with rtmp:// or rtmps://)", url);
        return -1;
    }

    /* Find host and port */
    const char* slash = strchr(url_part, '/');
    if (!slash) {
        IMP_LOG_ERR(TAG, "No app/stream path in URL: %s", url);
        return -1;
    }

    /* Extract host:port */
    size_t host_len = slash - url_part;
    char host_port[256];
    strncpy(host_port, url_part, host_len);
    host_port[host_len] = '\0';

    /* Parse host and port */
    char* colon = strchr(host_port, ':');
    if (colon) {
        *colon = '\0';
        strcpy(host, host_port);
        *port = atoi(colon + 1);
    } else {
        strcpy(host, host_port);
        /* Use appropriate default port based on protocol */
        *port = *use_tls ? 443 : RTMP_DEFAULT_PORT;
    }

    /* Parse app and stream */
    const char* path = slash + 1;
    const char* stream_slash = strchr(path, '/');
    if (stream_slash) {
        /* App is before the slash, stream is after */
        size_t app_len = stream_slash - path;
        strncpy(app, path, app_len);
        app[app_len] = '\0';
        strcpy(stream, stream_slash + 1);
    } else {
        /* Only app, no stream specified */
        strcpy(app, path);
        strcpy(stream, "");
    }

    IMP_LOG_DBG(TAG, "Parsed URL: host=%s, port=%d, app=%s, stream=%s, tls=%s",
               host, *port, app, stream, *use_tls ? "yes" : "no");
    return 0;
}

#if defined(RTMPS_BACKEND_OPENSSL) || defined(RTMPS_BACKEND_MBEDTLS)
/* TLS/SSL implementation with backend abstraction */

#ifdef RTMPS_BACKEND_OPENSSL
/* OpenSSL backend implementation */

static int rtmp_client_tls_setup(rtmp_client_connection_t* conn, const char* hostname)
{
    if (!conn || !hostname) {
        IMP_LOG_ERR(TAG, "Invalid parameters for TLS setup");
        return -1;
    }

    IMP_LOG_INFO(TAG, "Setting up OpenSSL TLS connection for %s", hostname);

    /* Initialize OpenSSL */
    SSL_library_init();
    SSL_load_error_strings();
    OpenSSL_add_all_algorithms();

    /* Create SSL context */
    const SSL_METHOD* method = TLS_client_method();
    SSL_CTX* ctx = SSL_CTX_new(method);
    if (!ctx) {
        IMP_LOG_ERR(TAG, "Failed to create SSL context");
        return -1;
    }

    /* Configure SSL context */
    SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, NULL); /* Skip certificate verification for embedded use */
    SSL_CTX_set_options(ctx, SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3); /* Disable old protocols */

    /* Create SSL connection */
    SSL* ssl = SSL_new(ctx);
    if (!ssl) {
        IMP_LOG_ERR(TAG, "Failed to create SSL connection");
        SSL_CTX_free(ctx);
        return -1;
    }

    /* Set hostname for SNI */
    SSL_set_tlsext_host_name(ssl, hostname);

    /* Set socket */
    SSL_set_fd(ssl, conn->socket_fd);

    /* Store contexts */
    conn->ssl_context = ssl;
    conn->ssl_config = ctx;
    conn->entropy_context = NULL;
    conn->ctr_drbg_context = NULL;

    IMP_LOG_INFO(TAG, "OpenSSL TLS setup completed for %s", hostname);
    return 0;
}

static void rtmp_client_tls_cleanup(rtmp_client_connection_t* conn)
{
    if (!conn) {
        return;
    }

    if (conn->ssl_context) {
        SSL_free((SSL*)conn->ssl_context);
        conn->ssl_context = NULL;
    }

    if (conn->ssl_config) {
        SSL_CTX_free((SSL_CTX*)conn->ssl_config);
        conn->ssl_config = NULL;
    }
}

static int rtmp_client_tls_handshake(rtmp_client_connection_t* conn)
{
    if (!conn || !conn->ssl_context) {
        IMP_LOG_ERR(TAG, "Invalid TLS context for handshake");
        return -1;
    }

    SSL* ssl = (SSL*)conn->ssl_context;

    IMP_LOG_INFO(TAG, "Starting OpenSSL TLS handshake for target: %s", conn->name);

    int ret = SSL_connect(ssl);
    if (ret <= 0) {
        int ssl_error = SSL_get_error(ssl, ret);
        IMP_LOG_ERR(TAG, "OpenSSL handshake failed: %d (SSL error: %d)", ret, ssl_error);
        return -1;
    }

    IMP_LOG_INFO(TAG, "OpenSSL TLS handshake completed successfully for target: %s", conn->name);
    return 0;
}

static int rtmp_client_tls_write_bytes(rtmp_client_connection_t* conn, const uint8_t* buffer, size_t length)
{
    if (!conn || !buffer || length == 0) {
        return -1;
    }

    if (!conn->use_tls || !conn->ssl_context) {
        return rtmp_client_write_bytes(conn->socket_fd, buffer, length);
    }

    SSL* ssl = (SSL*)conn->ssl_context;
    size_t bytes_written = 0;

    while (bytes_written < length) {
        int ret = SSL_write(ssl, buffer + bytes_written, length - bytes_written);
        if (ret <= 0) {
            int ssl_error = SSL_get_error(ssl, ret);
            IMP_LOG_ERR(TAG, "OpenSSL write failed: %d (SSL error: %d)", ret, ssl_error);

            /* Try to read any error response immediately after SSL write failure */
            uint8_t error_buffer[1024];
            int error_bytes = rtmp_client_connection_read_available(conn, error_buffer, sizeof(error_buffer));
            if (error_bytes > 0) {
                IMP_LOG_ERR(TAG, "*** %s RESPONSE AFTER SSL WRITE FAILURE (%d bytes) ***", conn->name, error_bytes);

                /* Print as hex */
                char hex_str[256] = {0};
                int print_bytes = (error_bytes > 80) ? 80 : error_bytes;
                for (int i = 0; i < print_bytes; i++) {
                    snprintf(hex_str + (i * 3), 4, "%02x ", error_buffer[i]);
                }
                IMP_LOG_ERR(TAG, "%s hex: %s", conn->name, hex_str);

                /* Try to interpret as text */
                bool is_text = true;
                for (int i = 0; i < print_bytes; i++) {
                    if (error_buffer[i] < 32 && error_buffer[i] != '\n' && error_buffer[i] != '\r' && error_buffer[i] != '\t') {
                        is_text = false;
                        break;
                    }
                }
                if (is_text) {
                    error_buffer[print_bytes] = '\0';
                    IMP_LOG_ERR(TAG, "%s text: '%s'", conn->name, error_buffer);
                }

                /* Check if it looks like an RTMP message */
                if (error_bytes >= 1) {
                    uint8_t basic_header = error_buffer[0];
                    uint8_t chunk_type = (basic_header >> 6) & 0x03;
                    uint8_t chunk_stream_id = basic_header & 0x3F;
                    IMP_LOG_ERR(TAG, "RTMP analysis: basic_header=0x%02x, chunk_type=%d, stream_id=%d",
                               basic_header, chunk_type, chunk_stream_id);

                    if (error_bytes >= 12 && chunk_type == 0) {
                        uint32_t timestamp = (error_buffer[1] << 16) | (error_buffer[2] << 8) | error_buffer[3];
                        uint32_t msg_length = (error_buffer[4] << 16) | (error_buffer[5] << 8) | error_buffer[6];
                        uint8_t msg_type = error_buffer[7];
                        IMP_LOG_ERR(TAG, "RTMP message: timestamp=%u, length=%u, type=%d",
                                   timestamp, msg_length, msg_type);
                    }
                }
            } else {
                IMP_LOG_ERR(TAG, "*** NO RESPONSE FROM %s AFTER SSL WRITE FAILURE ***", conn->name);

                /* Try to read any pending response from %s */
                uint8_t error_response[512];
                int error_bytes = rtmp_client_connection_read_available(conn, error_response, sizeof(error_response));
                if (error_bytes > 0) {
                    char hex_str[512] = {0};
                    int show_bytes = (error_bytes > 100) ? 100 : error_bytes;
                    for (int i = 0; i < show_bytes; i++) {
                        snprintf(hex_str + (i * 3), 4, "%02x ", error_response[i]);
                    }
                    IMP_LOG_ERR(TAG, "%s error response (%d bytes): %s", conn->name, error_bytes, hex_str);

                    /* Try to extract any text */
                    char text_str[256] = {0};
                    int text_pos = 0;
                    for (int i = 0; i < show_bytes && text_pos < 255; i++) {
                        if (error_response[i] >= 32 && error_response[i] <= 126) {
                            text_str[text_pos++] = error_response[i];
                        } else if (error_response[i] == 0 && text_pos > 0) {
                            text_str[text_pos++] = '|';
                        }
                    }
                    if (text_pos > 0) {
                        IMP_LOG_ERR(TAG, "%s error text: '%s'", conn->name, text_str);
                    }
                } else {
                    IMP_LOG_ERR(TAG, "No error response available from %s", conn->name);
                }
            }

            /* Check for connection errors that require reconnection */
            if (ssl_error == SSL_ERROR_SYSCALL || ssl_error == SSL_ERROR_SSL ||
                ssl_error == SSL_ERROR_ZERO_RETURN) {
                IMP_LOG_WARN(TAG, "TLS connection broken for target: %s, marking for reconnection", conn->name);
                conn->state = RTMP_CLIENT_STATE_DISCONNECTED;
            }
            return -1;
        }
        bytes_written += ret;
    }

    return bytes_written;
}

static int rtmp_client_tls_read_bytes(rtmp_client_connection_t* conn, uint8_t* buffer, size_t length)
{
    if (!conn || !buffer || length == 0) {
        return -1;
    }

    if (!conn->use_tls || !conn->ssl_context) {
        return rtmp_client_read_bytes(conn->socket_fd, buffer, length);
    }

    SSL* ssl = (SSL*)conn->ssl_context;
    size_t bytes_read = 0;

    while (bytes_read < length) {
        int ret = SSL_read(ssl, buffer + bytes_read, length - bytes_read);
        if (ret <= 0) {
            int ssl_error = SSL_get_error(ssl, ret);
            if (ssl_error == SSL_ERROR_ZERO_RETURN) {
                IMP_LOG_DBG(TAG, "OpenSSL connection closed by peer");
                return bytes_read;
            }
            IMP_LOG_ERR(TAG, "OpenSSL read failed: %d (SSL error: %d)", ret, ssl_error);
            return -1;
        }
        bytes_read += ret;
    }

    return bytes_read;
}

#elif defined(RTMPS_BACKEND_MBEDTLS)
/* mbedTLS backend implementation */

static int rtmp_client_tls_setup(rtmp_client_connection_t* conn, const char* hostname)
{
    if (!conn || !hostname) {
        IMP_LOG_ERR(TAG, "Invalid parameters for TLS setup");
        return -1;
    }

    IMP_LOG_INFO(TAG, "Setting up mbedTLS connection for %s", hostname);

    /* Allocate mbedTLS contexts */
    conn->ssl_context = malloc(sizeof(mbedtls_ssl_context));
    conn->ssl_config = malloc(sizeof(mbedtls_ssl_config));
    conn->entropy_context = malloc(sizeof(mbedtls_entropy_context));
    conn->ctr_drbg_context = malloc(sizeof(mbedtls_ctr_drbg_context));

    if (!conn->ssl_context || !conn->ssl_config || !conn->entropy_context || !conn->ctr_drbg_context) {
        IMP_LOG_ERR(TAG, "Failed to allocate TLS contexts");
        rtmp_client_tls_cleanup(conn);
        return -1;
    }

    mbedtls_ssl_context* ssl = (mbedtls_ssl_context*)conn->ssl_context;
    mbedtls_ssl_config* conf = (mbedtls_ssl_config*)conn->ssl_config;
    mbedtls_entropy_context* entropy = (mbedtls_entropy_context*)conn->entropy_context;
    mbedtls_ctr_drbg_context* ctr_drbg = (mbedtls_ctr_drbg_context*)conn->ctr_drbg_context;

    /* Initialize contexts */
    mbedtls_ssl_init(ssl);
    mbedtls_ssl_config_init(conf);
    mbedtls_entropy_init(entropy);
    mbedtls_ctr_drbg_init(ctr_drbg);

    /* Seed the random number generator */
    const char* pers = "rtmp_client";
    int ret = mbedtls_ctr_drbg_seed(ctr_drbg, mbedtls_entropy_func, entropy,
                                   (const unsigned char*)pers, strlen(pers));
    if (ret != 0) {
        IMP_LOG_ERR(TAG, "mbedtls_ctr_drbg_seed failed: -0x%04x", -ret);
        rtmp_client_tls_cleanup(conn);
        return -1;
    }

    /* Setup SSL configuration */
    ret = mbedtls_ssl_config_defaults(conf, MBEDTLS_SSL_IS_CLIENT,
                                     MBEDTLS_SSL_TRANSPORT_STREAM,
                                     MBEDTLS_SSL_PRESET_DEFAULT);
    if (ret != 0) {
        IMP_LOG_ERR(TAG, "mbedtls_ssl_config_defaults failed: -0x%04x", -ret);
        rtmp_client_tls_cleanup(conn);
        return -1;
    }

    /* Configure certificate verification (skip for now - embedded use) */
    mbedtls_ssl_conf_authmode(conf, MBEDTLS_SSL_VERIFY_NONE);
    mbedtls_ssl_conf_rng(conf, mbedtls_ctr_drbg_random, ctr_drbg);

    /* Setup SSL context */
    ret = mbedtls_ssl_setup(ssl, conf);
    if (ret != 0) {
        IMP_LOG_ERR(TAG, "mbedtls_ssl_setup failed: -0x%04x", -ret);
        rtmp_client_tls_cleanup(conn);
        return -1;
    }

    /* Set hostname for SNI */
    ret = mbedtls_ssl_set_hostname(ssl, hostname);
    if (ret != 0) {
        IMP_LOG_ERR(TAG, "mbedtls_ssl_set_hostname failed: -0x%04x", -ret);
        rtmp_client_tls_cleanup(conn);
        return -1;
    }

    /* Set BIO callbacks */
    mbedtls_ssl_set_bio(ssl, &conn->socket_fd, mbedtls_net_send, mbedtls_net_recv, NULL);

    IMP_LOG_INFO(TAG, "mbedTLS setup completed for %s", hostname);
    return 0;
}

static void rtmp_client_tls_cleanup(rtmp_client_connection_t* conn)
{
    if (!conn) {
        return;
    }

    if (conn->ssl_context) {
        mbedtls_ssl_free((mbedtls_ssl_context*)conn->ssl_context);
        free(conn->ssl_context);
        conn->ssl_context = NULL;
    }

    if (conn->ssl_config) {
        mbedtls_ssl_config_free((mbedtls_ssl_config*)conn->ssl_config);
        free(conn->ssl_config);
        conn->ssl_config = NULL;
    }

    if (conn->entropy_context) {
        mbedtls_entropy_free((mbedtls_entropy_context*)conn->entropy_context);
        free(conn->entropy_context);
        conn->entropy_context = NULL;
    }

    if (conn->ctr_drbg_context) {
        mbedtls_ctr_drbg_free((mbedtls_ctr_drbg_context*)conn->ctr_drbg_context);
        free(conn->ctr_drbg_context);
        conn->ctr_drbg_context = NULL;
    }
}

static int rtmp_client_tls_handshake(rtmp_client_connection_t* conn)
{
    if (!conn || !conn->ssl_context) {
        IMP_LOG_ERR(TAG, "Invalid TLS context for handshake");
        return -1;
    }

    mbedtls_ssl_context* ssl = (mbedtls_ssl_context*)conn->ssl_context;

    IMP_LOG_INFO(TAG, "Starting mbedTLS handshake for target: %s", conn->name);

    int ret;
    while ((ret = mbedtls_ssl_handshake(ssl)) != 0) {
        if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
            IMP_LOG_ERR(TAG, "mbedTLS handshake failed: -0x%04x", -ret);
            return -1;
        }
    }

    IMP_LOG_INFO(TAG, "mbedTLS handshake completed successfully for target: %s", conn->name);
    return 0;
}

static int rtmp_client_tls_write_bytes(rtmp_client_connection_t* conn, const uint8_t* buffer, size_t length)
{
    if (!conn || !buffer || length == 0) {
        return -1;
    }

    if (!conn->use_tls || !conn->ssl_context) {
        return rtmp_client_write_bytes(conn->socket_fd, buffer, length);
    }

    mbedtls_ssl_context* ssl = (mbedtls_ssl_context*)conn->ssl_context;
    size_t bytes_written = 0;

    while (bytes_written < length) {
        int ret = mbedtls_ssl_write(ssl, buffer + bytes_written, length - bytes_written);
        if (ret < 0) {
            if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
                continue;
            }
            IMP_LOG_ERR(TAG, "mbedTLS write failed: -0x%04x", -ret);
            return -1;
        }
        bytes_written += ret;
    }

    return bytes_written;
}

static int rtmp_client_tls_read_bytes(rtmp_client_connection_t* conn, uint8_t* buffer, size_t length)
{
    if (!conn || !buffer || length == 0) {
        return -1;
    }

    if (!conn->use_tls || !conn->ssl_context) {
        return rtmp_client_read_bytes(conn->socket_fd, buffer, length);
    }

    mbedtls_ssl_context* ssl = (mbedtls_ssl_context*)conn->ssl_context;
    size_t bytes_read = 0;

    while (bytes_read < length) {
        int ret = mbedtls_ssl_read(ssl, buffer + bytes_read, length - bytes_read);
        if (ret < 0) {
            if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
                continue;
            }
            if (ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
                IMP_LOG_DBG(TAG, "mbedTLS connection closed by peer");
                return bytes_read;
            }
            IMP_LOG_ERR(TAG, "mbedTLS read failed: -0x%04x", -ret);
            return -1;
        }
        if (ret == 0) {
            return bytes_read;
        }
        bytes_read += ret;
    }

    return bytes_read;
}

#else
/* No TLS backend selected */
static int rtmp_client_tls_setup(rtmp_client_connection_t* conn, const char* hostname)
{
    IMP_LOG_ERR(TAG, "TLS support not compiled - cannot setup TLS for %s", hostname);
    return -1;
}

static void rtmp_client_tls_cleanup(rtmp_client_connection_t* conn)
{
    /* Nothing to cleanup */
}

static int rtmp_client_tls_handshake(rtmp_client_connection_t* conn)
{
    IMP_LOG_ERR(TAG, "TLS support not compiled - cannot perform handshake");
    return -1;
}

static int rtmp_client_tls_write_bytes(rtmp_client_connection_t* conn, const uint8_t* buffer, size_t length)
{
    return rtmp_client_write_bytes(conn->socket_fd, buffer, length);
}

static int rtmp_client_tls_read_bytes(rtmp_client_connection_t* conn, uint8_t* buffer, size_t length)
{
    return rtmp_client_read_bytes(conn->socket_fd, buffer, length);
}

#endif /* TLS backend selection */

#endif /* ENABLE_RTMPS */

/* Function to get active RTMP connection count - used by frame processing */
int rtmp_client_get_active_connection_count(void)
{
    if (!g_rtmp_client_module_state.running || !g_rtmp_client) {
        return 0;
    }

    int active_connections = 0;
    int total_connections = 0;
    pthread_mutex_lock(&g_rtmp_client->connections_mutex);

    rtmp_client_connection_t* conn = g_rtmp_client->connections;
    while (conn) {
        total_connections++;
        if (conn->state == RTMP_CLIENT_STATE_PUBLISHING && conn->thread_running) {
            active_connections++;
        } else {
            /* Debug why connection is not active */
            static int debug_count = 0;
            if (++debug_count % 50 == 1) {
                IMP_LOG_INFO(TAG, "RTMP connection not active: state=%d (PUBLISHING=%d), thread_running=%s, target=%s",
                            conn->state, RTMP_CLIENT_STATE_PUBLISHING, conn->thread_running ? "true" : "false", conn->name);
            }
        }
        conn = conn->next;
    }

    pthread_mutex_unlock(&g_rtmp_client->connections_mutex);

    /* FIXME: RTMP client shows active connections but may not be receiving frames
     * Need to verify frame manager is calling rtmp_client_send_frame() for RTMP consumers */

    /* Debug logging every 100 calls */
    static int count_calls = 0;
    count_calls++;
    if (count_calls % 100 == 0) {
        IMP_LOG_INFO(TAG, "RTMP active connection count: total=%d, active=%d", total_connections, active_connections);
    }

    return active_connections;
}

/* Auto-register module at startup */
MODULE_REGISTER(rtmp_client_module_info);
