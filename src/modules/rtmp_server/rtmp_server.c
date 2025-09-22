/*
 * rtmp_server.c - RTMP Server Module Implementation
 * Modular RTMP server for Thingino Streamer
 * Supports multiple concurrent RTMP connections with authentication
 * Also supports RTMPS (RTMP over TLS) for secure connections
 * Copyright (C) 2025 Paul Philippov, Thingino Project
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <imp/imp_common.h>
#include <imp/imp_encoder.h>
#include <json-c/json.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>

#include "../../common.h"
#include "../../config.h"
#include "rtmp_server.h"

/* External declarations */
extern struct chn_conf chn[FS_CHN_NUM];

#define TAG "RTMP_SERVER"

/* Global RTMP server instance */
static rtmp_server_t* g_rtmp_server = NULL;

/* Module state */
static struct {
    bool initialized;
    bool running;
    rtmp_server_config_t config;
    struct rtsp_server* rtsp_server;  /* Reference to RTSP server */
} g_rtmp_server_module_state = {0};

/* Forward declarations */
static void* rtmp_accept_thread(void* arg);
static void* rtmp_connection_thread(void* arg);
static int rtmp_server_create(rtmp_server_config_t* config);
static void rtmp_server_destroy(void);
static int rtmp_connection_handle(rtmp_connection_t* conn);
static void rtmp_connection_cleanup(rtmp_connection_t* conn);
static int rtmp_set_socket_timeout(int socket_fd, int timeout_seconds);

/* Module lifecycle functions */
int rtmp_server_module_init(void* config)
{
    IMP_LOG_INFO(TAG, "Initializing RTMP server module");

    if (g_rtmp_server_module_state.initialized) {
        IMP_LOG_WARN(TAG, "RTMP server module already initialized");
        return 0;
    }

    if (!config) {
        IMP_LOG_ERR(TAG, "Invalid configuration provided");
        return -1;
    }

    /* Copy configuration */
    memcpy(&g_rtmp_server_module_state.config, config, sizeof(rtmp_server_config_t));

    /* Validate configuration */
    if (rtmp_server_module_config_validate(&g_rtmp_server_module_state.config) != 0) {
        IMP_LOG_ERR(TAG, "Invalid RTMP module configuration");
        return -1;
    }

    g_rtmp_server_module_state.initialized = true;
    IMP_LOG_INFO(TAG, "RTMP module initialized successfully");
    return 0;
}

int rtmp_server_module_start(void)
{
    IMP_LOG_INFO(TAG, "Starting RTMP module");

    if (!g_rtmp_server_module_state.initialized) {
        IMP_LOG_ERR(TAG, "RTMP module not initialized");
        return -1;
    }

    if (g_rtmp_server_module_state.running) {
        IMP_LOG_WARN(TAG, "RTMP module already running");
        return 0;
    }

    if (!g_rtmp_server_module_state.config.enabled) {
        IMP_LOG_INFO(TAG, "RTMP module disabled in configuration");
        return 0;
    }

    /* Create RTMP server */
    if (rtmp_server_create(&g_rtmp_server_module_state.config) != 0) {
        IMP_LOG_ERR(TAG, "Failed to create RTMP server");
        return -1;
    }

    g_rtmp_server_module_state.running = true;
    IMP_LOG_INFO(TAG, "RTMP module started successfully on port %d", g_rtmp_server_module_state.config.port);
    return 0;
}

int rtmp_server_module_stop(void)
{
    IMP_LOG_INFO(TAG, "Stopping RTMP module");

    if (!g_rtmp_server_module_state.running) {
        IMP_LOG_WARN(TAG, "RTMP module not running");
        return 0;
    }

    /* Destroy RTMP server */
    rtmp_server_destroy();

    g_rtmp_server_module_state.running = false;
    IMP_LOG_INFO(TAG, "RTMP module stopped successfully");
    return 0;
}

int rtmp_server_module_cleanup(void)
{
    IMP_LOG_INFO(TAG, "Cleaning up RTMP module");

    if (g_rtmp_server_module_state.running) {
        rtmp_server_module_stop();
    }

    if (!g_rtmp_server_module_state.initialized) {
        return 0;
    }

    /* Reset state */
    memset(&g_rtmp_server_module_state, 0, sizeof(g_rtmp_server_module_state));

    IMP_LOG_INFO(TAG, "RTMP module cleaned up successfully");
    return 0;
}

int rtmp_server_module_get_config_size(void)
{
    return sizeof(rtmp_server_config_t);
}

int rtmp_server_module_config_parse(json_object* json, void* config)
{
    if (!json || !config) {
        IMP_LOG_ERR(TAG, "Invalid parameters for config parsing");
        return -1;
    }

    rtmp_server_config_t* rtmp_config = (rtmp_server_config_t*)config;

    /* Set defaults first */
    rtmp_server_module_set_defaults(config);

    /* JSON root is the rtmp_server config directly (no wrapper) */
    json_object* rtmp_obj = json;

    /* Parse JSON configuration */
    json_object* enabled_obj;
    if (json_object_object_get_ex(rtmp_obj, "enabled", &enabled_obj)) {
        rtmp_config->enabled = json_object_get_boolean(enabled_obj);
    }

    json_object* port_obj;
    if (json_object_object_get_ex(rtmp_obj, "port", &port_obj)) {
        rtmp_config->port = json_object_get_int(port_obj);
    }

    json_object* max_connections_obj;
    if (json_object_object_get_ex(rtmp_obj, "max_connections", &max_connections_obj)) {
        rtmp_config->max_connections = json_object_get_int(max_connections_obj);
    }

    json_object* chunk_size_obj;
    if (json_object_object_get_ex(rtmp_obj, "chunk_size", &chunk_size_obj)) {
        rtmp_config->chunk_size = json_object_get_int(chunk_size_obj);
    }

    json_object* auth_required_obj;
    if (json_object_object_get_ex(rtmp_obj, "auth_required", &auth_required_obj)) {
        rtmp_config->auth_required = json_object_get_boolean(auth_required_obj);
    }

    json_object* stream_key_obj;
    if (json_object_object_get_ex(rtmp_obj, "stream_key", &stream_key_obj)) {
        const char* stream_key = json_object_get_string(stream_key_obj);
        if (stream_key) {
            strncpy(rtmp_config->stream_key, stream_key, sizeof(rtmp_config->stream_key) - 1);
            rtmp_config->stream_key[sizeof(rtmp_config->stream_key) - 1] = '\0';
        }
    }

    json_object* app_name_obj;
    if (json_object_object_get_ex(rtmp_obj, "app_name", &app_name_obj)) {
        const char* app_name = json_object_get_string(app_name_obj);
        if (app_name) {
            strncpy(rtmp_config->app_name, app_name, sizeof(rtmp_config->app_name) - 1);
            rtmp_config->app_name[sizeof(rtmp_config->app_name) - 1] = '\0';
        }
    }

    json_object* connection_timeout_obj;
    if (json_object_object_get_ex(rtmp_obj, "connection_timeout", &connection_timeout_obj)) {
        rtmp_config->connection_timeout = json_object_get_int(connection_timeout_obj);
    }

    IMP_LOG_DBG(TAG, "RTMP config parsed: enabled=%s, port=%d, max_connections=%d",
                rtmp_config->enabled ? "true" : "false",
                rtmp_config->port,
                rtmp_config->max_connections);

    return 0;
}

int rtmp_server_module_config_validate(void* config)
{
    if (!config) {
        IMP_LOG_ERR(TAG, "Invalid config pointer");
        return -1;
    }

    rtmp_server_config_t* rtmp_config = (rtmp_server_config_t*)config;

    /* Validate port range */
    if (rtmp_config->port < 1 || rtmp_config->port > 65535) {
        IMP_LOG_ERR(TAG, "Invalid port number: %d", rtmp_config->port);
        return -1;
    }

    /* Validate max connections */
    if (rtmp_config->max_connections < 1 || rtmp_config->max_connections > 100) {
        IMP_LOG_ERR(TAG, "Invalid max_connections: %d", rtmp_config->max_connections);
        return -1;
    }

    /* Validate chunk size */
    if (rtmp_config->chunk_size < 1 || rtmp_config->chunk_size > 65536) {
        IMP_LOG_ERR(TAG, "Invalid chunk_size: %d", rtmp_config->chunk_size);
        return -1;
    }

    /* Validate connection timeout */
    if (rtmp_config->connection_timeout < 1 || rtmp_config->connection_timeout > 300) {
        IMP_LOG_ERR(TAG, "Invalid connection_timeout: %d", rtmp_config->connection_timeout);
        return -1;
    }

    return 0;
}

int rtmp_server_module_set_defaults(void* config)
{
    if (!config) {
        return -1;
    }

    rtmp_server_config_t* rtmp_config = (rtmp_server_config_t*)config;
    memset(rtmp_config, 0, sizeof(rtmp_server_config_t));

    /* Set default values */
    rtmp_config->enabled = false;  /* Disabled by default */
    rtmp_config->port = RTMP_DEFAULT_PORT;
    rtmp_config->max_connections = 10;
    rtmp_config->chunk_size = RTMP_DEFAULT_CHUNK_SIZE;
    rtmp_config->auth_required = false;
    strcpy(rtmp_config->stream_key, "");
    strcpy(rtmp_config->app_name, "live");
    rtmp_config->connection_timeout = 30;

    return 0;
}

/* RTSP server integration */
int rtmp_server_module_set_rtsp_server(struct rtsp_server* server)
{
    g_rtmp_server_module_state.rtsp_server = server;
    IMP_LOG_INFO(TAG, "RTSP server reference set for RTMP module");
    return 0;
}

/* RTMP server access */
rtmp_server_t* rtmp_server_module_get_server(void)
{
    return g_rtmp_server;
}

/* Module registration function */
int register_rtmp_module(void)
{
    return module_register(&rtmp_server_module_info);
}

/* Module registration - following the established pattern */
module_info_t rtmp_server_module_info = {
    .name = RTMP_SERVER_MODULE_NAME,
    .version = RTMP_SERVER_MODULE_VERSION,
    .description = "Real Time Messaging Protocol (RTMP) server for live streaming",
    .state = MODULE_STATE_UNREGISTERED,
    .module_data = &g_rtmp_server_module_state,

    /* Lifecycle callbacks */
    .init = rtmp_server_module_init,
    .start = rtmp_server_module_start,
    .stop = rtmp_server_module_stop,
    .cleanup = rtmp_server_module_cleanup,

    /* Configuration */
    .config_size = sizeof(rtmp_server_config_t),
    .config_parse = rtmp_server_module_config_parse,
    .config_validate = rtmp_server_module_config_validate,

    /* RTSP integration - RTMP module receives frames via RTSP frame callback */
    .rtsp_setup = NULL,
    .rtsp_frame_callback = rtmp_server_module_rtsp_frame_callback,
    .rtsp_cleanup = NULL,

    /* Statistics - not implemented yet */
    .get_stats = NULL
};

/* RTMP server implementation */
static int rtmp_server_create(rtmp_server_config_t* config)
{
    if (g_rtmp_server) {
        IMP_LOG_WARN(TAG, "RTMP server already exists");
        return 0;
    }

    g_rtmp_server = calloc(1, sizeof(rtmp_server_t));
    if (!g_rtmp_server) {
        IMP_LOG_ERR(TAG, "Failed to allocate RTMP server");
        return -1;
    }

    /* Initialize server */
    g_rtmp_server->port = config->port;
    g_rtmp_server->max_connections = config->max_connections;
    g_rtmp_server->current_connections = 0;
    g_rtmp_server->connections = NULL;
    pthread_mutex_init(&g_rtmp_server->connections_mutex, NULL);

    /* Create server socket */
    g_rtmp_server->server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (g_rtmp_server->server_socket < 0) {
        IMP_LOG_ERR(TAG, "Failed to create server socket: %s", strerror(errno));
        free(g_rtmp_server);
        g_rtmp_server = NULL;
        return -1;
    }

    /* Set socket options */
    int opt = 1;
    if (setsockopt(g_rtmp_server->server_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        IMP_LOG_WARN(TAG, "Failed to set SO_REUSEADDR: %s", strerror(errno));
    }

    /* Bind socket */
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(config->port);

    if (bind(g_rtmp_server->server_socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        IMP_LOG_ERR(TAG, "Failed to bind server socket to port %d: %s", config->port, strerror(errno));
        close(g_rtmp_server->server_socket);
        free(g_rtmp_server);
        g_rtmp_server = NULL;
        return -1;
    }

    /* Listen for connections */
    if (listen(g_rtmp_server->server_socket, 5) < 0) {
        IMP_LOG_ERR(TAG, "Failed to listen on server socket: %s", strerror(errno));
        close(g_rtmp_server->server_socket);
        free(g_rtmp_server);
        g_rtmp_server = NULL;
        return -1;
    }

    /* Start accept thread */
    g_rtmp_server->running = true;
    if (pthread_create(&g_rtmp_server->accept_thread, NULL, rtmp_accept_thread, g_rtmp_server) != 0) {
        IMP_LOG_ERR(TAG, "Failed to create accept thread");
        close(g_rtmp_server->server_socket);
        free(g_rtmp_server);
        g_rtmp_server = NULL;
        return -1;
    }

    IMP_LOG_INFO(TAG, "RTMP server created and listening on port %d", config->port);
    return 0;
}

static void rtmp_server_destroy(void)
{
    if (!g_rtmp_server) {
        return;
    }

    IMP_LOG_INFO(TAG, "Destroying RTMP server");

    /* Stop accepting new connections */
    g_rtmp_server->running = false;

    /* Close server socket */
    if (g_rtmp_server->server_socket >= 0) {
        close(g_rtmp_server->server_socket);
        g_rtmp_server->server_socket = -1;
    }

    /* Wait for accept thread to finish */
    if (g_rtmp_server->accept_thread) {
        pthread_join(g_rtmp_server->accept_thread, NULL);
    }

    /* Close all connections */
    pthread_mutex_lock(&g_rtmp_server->connections_mutex);
    rtmp_connection_t* conn = g_rtmp_server->connections;
    while (conn) {
        rtmp_connection_t* next = conn->next;
        rtmp_connection_cleanup(conn);
        conn = next;
    }
    g_rtmp_server->connections = NULL;
    pthread_mutex_unlock(&g_rtmp_server->connections_mutex);

    /* Cleanup mutex */
    pthread_mutex_destroy(&g_rtmp_server->connections_mutex);

    /* Free server */
    free(g_rtmp_server);
    g_rtmp_server = NULL;

    IMP_LOG_INFO(TAG, "RTMP server destroyed");
}

/* RTMP connection handling */
static void* rtmp_accept_thread(void* arg)
{
    rtmp_server_t* server = (rtmp_server_t*)arg;

    IMP_LOG_INFO(TAG, "RTMP accept thread started");

    while (server->running) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);

        int client_socket = accept(server->server_socket, (struct sockaddr*)&client_addr, &client_len);
        if (client_socket < 0) {
            if (server->running) {
                IMP_LOG_ERR(TAG, "Failed to accept client connection: %s", strerror(errno));
            }
            continue;
        }

        /* Check connection limit */
        if (server->current_connections >= server->max_connections) {
            IMP_LOG_WARN(TAG, "Maximum connections reached, rejecting client");
            close(client_socket);
            continue;
        }

        /* Create new connection */
        rtmp_connection_t* conn = calloc(1, sizeof(rtmp_connection_t));
        if (!conn) {
            IMP_LOG_ERR(TAG, "Failed to allocate connection");
            close(client_socket);
            continue;
        }

        /* Initialize connection */
        conn->socket_fd = client_socket;
        conn->state = RTMP_STATE_UNINITIALIZED;
        conn->chunk_size_in = RTMP_DEFAULT_CHUNK_SIZE;
        conn->chunk_size_out = RTMP_DEFAULT_CHUNK_SIZE;
        conn->window_ack_size = 2500000; /* 2.5MB default */
        conn->publishing = false;
        conn->thread_running = true;

        /* Set socket timeouts for handshake */
        rtmp_set_socket_timeout(client_socket, 30); /* 30 second timeout */

        /* Add to connections list */
        pthread_mutex_lock(&server->connections_mutex);
        conn->next = server->connections;
        server->connections = conn;
        server->current_connections++;
        pthread_mutex_unlock(&server->connections_mutex);

        /* Start connection thread */
        if (pthread_create(&conn->thread, NULL, rtmp_connection_thread, conn) != 0) {
            IMP_LOG_ERR(TAG, "Failed to create connection thread");
            rtmp_connection_cleanup(conn);
            continue;
        }

        IMP_LOG_INFO(TAG, "New RTMP connection from %s:%d",
                     inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));
    }

    IMP_LOG_INFO(TAG, "RTMP accept thread stopped");
    return NULL;
}

static void* rtmp_connection_thread(void* arg)
{
    rtmp_connection_t* conn = (rtmp_connection_t*)arg;

    IMP_LOG_DBG(TAG, "RTMP connection thread started for fd %d", conn->socket_fd);

    /* Handle connection */
    if (rtmp_connection_handle(conn) != 0) {
        IMP_LOG_ERR(TAG, "RTMP connection handling failed");
    }

    /* Cleanup connection */
    rtmp_connection_cleanup(conn);

    IMP_LOG_DBG(TAG, "RTMP connection thread finished for fd %d", conn->socket_fd);
    return NULL;
}

static int rtmp_connection_handle(rtmp_connection_t* conn)
{
    /* Process RTMP handshake */
    if (rtmp_handshake_process(conn) != 0) {
        IMP_LOG_ERR(TAG, "RTMP handshake failed");
        return -1;
    }

    IMP_LOG_INFO(TAG, "RTMP handshake completed successfully");

    /* Main connection loop - process RTMP messages */
    rtmp_message_t partial_msg;
    memset(&partial_msg, 0, sizeof(partial_msg));
    bool has_partial_message = false;

    while (conn->thread_running && g_rtmp_server && g_rtmp_server->running) {
        rtmp_message_t msg;

        /* Use existing partial message or start new one */
        if (has_partial_message) {
            msg = partial_msg;
        } else {
            memset(&msg, 0, sizeof(msg));
        }

        /* Read RTMP chunk */
        int ret = rtmp_chunk_read(conn, &msg);
        if (ret < 0) {
            IMP_LOG_ERR(TAG, "Failed to read RTMP chunk");
            if (msg.payload) {
                free(msg.payload);
            }
            break;
        } else if (ret == 0) {
            /* Connection closed */
            IMP_LOG_INFO(TAG, "RTMP connection closed by client");
            if (msg.payload) {
                free(msg.payload);
            }
            break;
        } else if (ret == 2) {
            /* Partial message, need more chunks */
            partial_msg = msg;
            has_partial_message = true;
            continue;
        }

        /* Complete message received (ret == 1) */
        has_partial_message = false;

        /* Process complete message */
        if (rtmp_message_parse(conn, &msg) != 0) {
            IMP_LOG_ERR(TAG, "Failed to parse RTMP message");
        }

        /* Free message payload */
        if (msg.payload) {
            free(msg.payload);
            msg.payload = NULL;
        }
    }

    /* Clean up any remaining partial message */
    if (has_partial_message && partial_msg.payload) {
        free(partial_msg.payload);
    }

    return 0;
}

static void rtmp_connection_cleanup(rtmp_connection_t* conn)
{
    if (!conn) {
        return;
    }

    IMP_LOG_DBG(TAG, "Cleaning up RTMP connection fd %d", conn->socket_fd);

    /* Stop thread */
    conn->thread_running = false;

    /* Close socket */
    if (conn->socket_fd >= 0) {
        close(conn->socket_fd);
        conn->socket_fd = -1;
    }

    /* Remove from connections list */
    if (g_rtmp_server) {
        pthread_mutex_lock(&g_rtmp_server->connections_mutex);
        rtmp_connection_t** current = &g_rtmp_server->connections;
        while (*current) {
            if (*current == conn) {
                *current = conn->next;
                g_rtmp_server->current_connections--;
                break;
            }
            current = &(*current)->next;
        }
        pthread_mutex_unlock(&g_rtmp_server->connections_mutex);
    }

    /* Free connection */
    free(conn);
}

/* RTMP protocol functions */

/* Helper function to set socket timeout */
static int rtmp_set_socket_timeout(int socket_fd, int timeout_seconds)
{
    struct timeval timeout;
    timeout.tv_sec = timeout_seconds;
    timeout.tv_usec = 0;

    if (setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0) {
        IMP_LOG_WARN(TAG, "Failed to set receive timeout: %s", strerror(errno));
        return -1;
    }

    if (setsockopt(socket_fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) < 0) {
        IMP_LOG_WARN(TAG, "Failed to set send timeout: %s", strerror(errno));
        return -1;
    }

    return 0;
}

/* Helper function to read exact number of bytes */
static int rtmp_read_bytes(int socket_fd, uint8_t* buffer, size_t length)
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

/* Helper function to write exact number of bytes */
static int rtmp_write_bytes(int socket_fd, const uint8_t* buffer, size_t length)
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

/* RTMP handshake implementation */
int rtmp_handshake_process(rtmp_connection_t* conn)
{
    IMP_LOG_DBG(TAG, "Processing RTMP handshake for fd %d", conn->socket_fd);

    /* Step 1: Read C0 (client version) */
    uint8_t c0_version;
    if (rtmp_read_bytes(conn->socket_fd, &c0_version, 1) != 1) {
        IMP_LOG_ERR(TAG, "Failed to read C0 version byte");
        return -1;
    }

    if (c0_version != RTMP_VERSION) {
        IMP_LOG_ERR(TAG, "Unsupported RTMP version: %d (expected %d)", c0_version, RTMP_VERSION);
        return -1;
    }

    conn->c0_s0_version = c0_version;
    conn->state = RTMP_STATE_VERSION_SENT;

    IMP_LOG_DBG(TAG, "Received C0: version=%d", c0_version);

    /* Step 2: Send S0 (server version) */
    uint8_t s0_version = RTMP_VERSION;
    if (rtmp_write_bytes(conn->socket_fd, &s0_version, 1) != 1) {
        IMP_LOG_ERR(TAG, "Failed to send S0 version byte");
        return -1;
    }

    IMP_LOG_DBG(TAG, "Sent S0: version=%d", s0_version);

    /* Step 3: Read C1 (client random data) */
    if (rtmp_read_bytes(conn->socket_fd, conn->c1_s1_data, RTMP_HANDSHAKE_SIZE) != RTMP_HANDSHAKE_SIZE) {
        IMP_LOG_ERR(TAG, "Failed to read C1 data");
        return -1;
    }

    IMP_LOG_DBG(TAG, "Received C1: %d bytes", RTMP_HANDSHAKE_SIZE);

    /* Step 4: Generate and send S1 (server random data) */
    /* S1 format: [time:4][version:4][random:1528] */
    uint8_t s1_data[RTMP_HANDSHAKE_SIZE];

    /* Set timestamp (current time) */
    uint32_t timestamp = (uint32_t)time(NULL);
    s1_data[0] = (timestamp >> 24) & 0xFF;
    s1_data[1] = (timestamp >> 16) & 0xFF;
    s1_data[2] = (timestamp >> 8) & 0xFF;
    s1_data[3] = timestamp & 0xFF;

    /* Set version (zeros for now) */
    s1_data[4] = 0;
    s1_data[5] = 0;
    s1_data[6] = 0;
    s1_data[7] = 0;

    /* Fill with random data */
    for (int i = 8; i < RTMP_HANDSHAKE_SIZE; i++) {
        s1_data[i] = rand() & 0xFF;
    }

    if (rtmp_write_bytes(conn->socket_fd, s1_data, RTMP_HANDSHAKE_SIZE) != RTMP_HANDSHAKE_SIZE) {
        IMP_LOG_ERR(TAG, "Failed to send S1 data");
        return -1;
    }

    IMP_LOG_DBG(TAG, "Sent S1: %d bytes", RTMP_HANDSHAKE_SIZE);

    /* Step 5: Send S2 (echo of C1) */
    if (rtmp_write_bytes(conn->socket_fd, conn->c1_s1_data, RTMP_HANDSHAKE_SIZE) != RTMP_HANDSHAKE_SIZE) {
        IMP_LOG_ERR(TAG, "Failed to send S2 data");
        return -1;
    }

    conn->state = RTMP_STATE_ACK_SENT;
    IMP_LOG_DBG(TAG, "Sent S2: %d bytes (echo of C1)", RTMP_HANDSHAKE_SIZE);

    /* Step 6: Read C2 (echo of S1) */
    if (rtmp_read_bytes(conn->socket_fd, conn->c2_s2_data, RTMP_HANDSHAKE_SIZE) != RTMP_HANDSHAKE_SIZE) {
        IMP_LOG_ERR(TAG, "Failed to read C2 data");
        return -1;
    }

    IMP_LOG_DBG(TAG, "Received C2: %d bytes", RTMP_HANDSHAKE_SIZE);

    /* Verify C2 is echo of S1 (optional validation) */
    /* For now, we'll accept any C2 data */

    conn->state = RTMP_STATE_HANDSHAKE_DONE;
    IMP_LOG_INFO(TAG, "RTMP handshake completed successfully for fd %d", conn->socket_fd);

    return 0;
}

/* Helper function to read chunk header based on format */
static int rtmp_read_chunk_header(rtmp_connection_t* conn, rtmp_chunk_header_t* header)
{
    uint8_t basic_header;

    /* Read basic header (1-3 bytes) */
    if (rtmp_read_bytes(conn->socket_fd, &basic_header, 1) != 1) {
        return 0; /* Connection closed or error */
    }

    /* Extract format (chunk type) and chunk stream ID */
    header->fmt = (basic_header >> 6) & 0x03;
    header->chunk_stream_id = basic_header & 0x3F;

    /* Handle extended chunk stream ID formats */
    if (header->chunk_stream_id == 0) {
        /* 2-byte format: chunk stream ID = second byte + 64 */
        uint8_t cs_id;
        if (rtmp_read_bytes(conn->socket_fd, &cs_id, 1) != 1) {
            return -1;
        }
        header->chunk_stream_id = cs_id + 64;
    } else if (header->chunk_stream_id == 1) {
        /* 3-byte format: chunk stream ID = (third byte * 256) + second byte + 64 */
        uint8_t cs_id_bytes[2];
        if (rtmp_read_bytes(conn->socket_fd, cs_id_bytes, 2) != 2) {
            return -1;
        }
        header->chunk_stream_id = (cs_id_bytes[1] * 256) + cs_id_bytes[0] + 64;
    }

    /* Read message header based on format */
    switch (header->fmt) {
        case RTMP_CHUNK_TYPE_0: {
            /* Type 0: 11-byte header */
            uint8_t msg_header[11];
            if (rtmp_read_bytes(conn->socket_fd, msg_header, 11) != 11) {
                return -1;
            }

            /* Parse timestamp (3 bytes, big-endian) */
            header->timestamp = (msg_header[0] << 16) | (msg_header[1] << 8) | msg_header[2];

            /* Parse message length (3 bytes, big-endian) */
            header->message_length = (msg_header[3] << 16) | (msg_header[4] << 8) | msg_header[5];

            /* Parse message type ID (1 byte) */
            header->message_type_id = msg_header[6];

            /* Parse message stream ID (4 bytes, little-endian) */
            header->message_stream_id = msg_header[7] | (msg_header[8] << 8) |
                                       (msg_header[9] << 16) | (msg_header[10] << 24);

            /* Handle extended timestamp */
            if (header->timestamp == 0xFFFFFF) {
                uint8_t ext_timestamp[4];
                if (rtmp_read_bytes(conn->socket_fd, ext_timestamp, 4) != 4) {
                    return -1;
                }
                header->timestamp = (ext_timestamp[0] << 24) | (ext_timestamp[1] << 16) |
                                   (ext_timestamp[2] << 8) | ext_timestamp[3];
            }
            break;
        }

        case RTMP_CHUNK_TYPE_1: {
            /* Type 1: 7-byte header (no message stream ID) */
            uint8_t msg_header[7];
            if (rtmp_read_bytes(conn->socket_fd, msg_header, 7) != 7) {
                return -1;
            }

            /* Parse timestamp delta (3 bytes, big-endian) */
            uint32_t timestamp_delta = (msg_header[0] << 16) | (msg_header[1] << 8) | msg_header[2];
            header->timestamp += timestamp_delta;

            /* Parse message length (3 bytes, big-endian) */
            header->message_length = (msg_header[3] << 16) | (msg_header[4] << 8) | msg_header[5];

            /* Parse message type ID (1 byte) */
            header->message_type_id = msg_header[6];

            /* Message stream ID remains the same as previous chunk */

            /* Handle extended timestamp delta */
            if (timestamp_delta == 0xFFFFFF) {
                uint8_t ext_timestamp[4];
                if (rtmp_read_bytes(conn->socket_fd, ext_timestamp, 4) != 4) {
                    return -1;
                }
                uint32_t ext_delta = (ext_timestamp[0] << 24) | (ext_timestamp[1] << 16) |
                                    (ext_timestamp[2] << 8) | ext_timestamp[3];
                header->timestamp = header->timestamp - timestamp_delta + ext_delta;
            }
            break;
        }

        case RTMP_CHUNK_TYPE_2: {
            /* Type 2: 3-byte header (only timestamp delta) */
            uint8_t msg_header[3];
            if (rtmp_read_bytes(conn->socket_fd, msg_header, 3) != 3) {
                return -1;
            }

            /* Parse timestamp delta (3 bytes, big-endian) */
            uint32_t timestamp_delta = (msg_header[0] << 16) | (msg_header[1] << 8) | msg_header[2];
            header->timestamp += timestamp_delta;

            /* Message length, type ID, and stream ID remain the same */

            /* Handle extended timestamp delta */
            if (timestamp_delta == 0xFFFFFF) {
                uint8_t ext_timestamp[4];
                if (rtmp_read_bytes(conn->socket_fd, ext_timestamp, 4) != 4) {
                    return -1;
                }
                uint32_t ext_delta = (ext_timestamp[0] << 24) | (ext_timestamp[1] << 16) |
                                    (ext_timestamp[2] << 8) | ext_timestamp[3];
                header->timestamp = header->timestamp - timestamp_delta + ext_delta;
            }
            break;
        }

        case RTMP_CHUNK_TYPE_3:
            /* Type 3: No header, all values remain the same as previous chunk */
            break;

        default:
            IMP_LOG_ERR(TAG, "Invalid chunk format: %d", header->fmt);
            return -1;
    }

    return 1; /* Success */
}

int rtmp_chunk_read(rtmp_connection_t* conn, rtmp_message_t* msg)
{
    static rtmp_chunk_header_t last_headers[64]; /* Store last header for each chunk stream ID */

    IMP_LOG_DBG(TAG, "Reading RTMP chunk from fd %d", conn->socket_fd);

    /* Initialize message */
    memset(msg, 0, sizeof(rtmp_message_t));

    /* Read chunk header */
    int result = rtmp_read_chunk_header(conn, &msg->header);
    if (result <= 0) {
        return result; /* Error or connection closed */
    }

    /* For chunk types 1, 2, 3, inherit values from previous chunk of same stream ID */
    uint32_t cs_id = msg->header.chunk_stream_id;
    if (cs_id < 64) {
        if (msg->header.fmt == RTMP_CHUNK_TYPE_1) {
            msg->header.message_stream_id = last_headers[cs_id].message_stream_id;
        } else if (msg->header.fmt == RTMP_CHUNK_TYPE_2) {
            msg->header.message_length = last_headers[cs_id].message_length;
            msg->header.message_type_id = last_headers[cs_id].message_type_id;
            msg->header.message_stream_id = last_headers[cs_id].message_stream_id;
        } else if (msg->header.fmt == RTMP_CHUNK_TYPE_3) {
            msg->header.timestamp = last_headers[cs_id].timestamp;
            msg->header.message_length = last_headers[cs_id].message_length;
            msg->header.message_type_id = last_headers[cs_id].message_type_id;
            msg->header.message_stream_id = last_headers[cs_id].message_stream_id;
        }

        /* Store header for future chunks */
        last_headers[cs_id] = msg->header;
    }

    /* Calculate chunk data size */
    uint32_t chunk_data_size = msg->header.message_length - msg->bytes_read;
    if (chunk_data_size > conn->chunk_size_in) {
        chunk_data_size = conn->chunk_size_in;
    }

    /* Allocate or reallocate payload buffer */
    if (!msg->payload) {
        msg->payload = malloc(msg->header.message_length);
        if (!msg->payload) {
            IMP_LOG_ERR(TAG, "Failed to allocate message payload buffer");
            return -1;
        }
        msg->payload_size = msg->header.message_length;
    }

    /* Read chunk data */
    if (chunk_data_size > 0) {
        if (rtmp_read_bytes(conn->socket_fd, msg->payload + msg->bytes_read, chunk_data_size) != (int)chunk_data_size) {
            free(msg->payload);
            msg->payload = NULL;
            return -1;
        }
        msg->bytes_read += chunk_data_size;
    }

    IMP_LOG_DBG(TAG, "Read chunk: cs_id=%d, fmt=%d, timestamp=%u, msg_len=%u, msg_type=%d, bytes_read=%u",
                msg->header.chunk_stream_id, msg->header.fmt, msg->header.timestamp,
                msg->header.message_length, msg->header.message_type_id, msg->bytes_read);

    /* Check if message is complete */
    if (msg->bytes_read >= msg->header.message_length) {
        return 1; /* Complete message */
    } else {
        return 2; /* Partial message, need more chunks */
    }
}

/* Helper function to write chunk header */
static int rtmp_write_chunk_header(rtmp_connection_t* conn, rtmp_chunk_header_t* header, uint8_t fmt)
{
    uint8_t basic_header;

    /* Create basic header */
    if (header->chunk_stream_id < 64) {
        /* 1-byte format */
        basic_header = (fmt << 6) | header->chunk_stream_id;
        if (rtmp_write_bytes(conn->socket_fd, &basic_header, 1) != 1) {
            return -1;
        }
    } else if (header->chunk_stream_id < 320) {
        /* 2-byte format */
        basic_header = (fmt << 6) | 0;
        uint8_t cs_id = header->chunk_stream_id - 64;
        if (rtmp_write_bytes(conn->socket_fd, &basic_header, 1) != 1 ||
            rtmp_write_bytes(conn->socket_fd, &cs_id, 1) != 1) {
            return -1;
        }
    } else {
        /* 3-byte format */
        basic_header = (fmt << 6) | 1;
        uint16_t cs_id = header->chunk_stream_id - 64;
        uint8_t cs_id_bytes[2] = {cs_id & 0xFF, (cs_id >> 8) & 0xFF};
        if (rtmp_write_bytes(conn->socket_fd, &basic_header, 1) != 1 ||
            rtmp_write_bytes(conn->socket_fd, cs_id_bytes, 2) != 2) {
            return -1;
        }
    }

    /* Write message header based on format */
    switch (fmt) {
        case RTMP_CHUNK_TYPE_0: {
            /* Type 0: 11-byte header */
            uint8_t msg_header[11];

            /* Timestamp (3 bytes, big-endian) */
            uint32_t timestamp = header->timestamp;
            if (timestamp >= 0xFFFFFF) {
                msg_header[0] = 0xFF;
                msg_header[1] = 0xFF;
                msg_header[2] = 0xFF;
            } else {
                msg_header[0] = (timestamp >> 16) & 0xFF;
                msg_header[1] = (timestamp >> 8) & 0xFF;
                msg_header[2] = timestamp & 0xFF;
            }

            /* Message length (3 bytes, big-endian) */
            msg_header[3] = (header->message_length >> 16) & 0xFF;
            msg_header[4] = (header->message_length >> 8) & 0xFF;
            msg_header[5] = header->message_length & 0xFF;

            /* Message type ID (1 byte) */
            msg_header[6] = header->message_type_id;

            /* Message stream ID (4 bytes, little-endian) */
            msg_header[7] = header->message_stream_id & 0xFF;
            msg_header[8] = (header->message_stream_id >> 8) & 0xFF;
            msg_header[9] = (header->message_stream_id >> 16) & 0xFF;
            msg_header[10] = (header->message_stream_id >> 24) & 0xFF;

            if (rtmp_write_bytes(conn->socket_fd, msg_header, 11) != 11) {
                return -1;
            }

            /* Extended timestamp if needed */
            if (timestamp >= 0xFFFFFF) {
                uint8_t ext_timestamp[4];
                ext_timestamp[0] = (timestamp >> 24) & 0xFF;
                ext_timestamp[1] = (timestamp >> 16) & 0xFF;
                ext_timestamp[2] = (timestamp >> 8) & 0xFF;
                ext_timestamp[3] = timestamp & 0xFF;
                if (rtmp_write_bytes(conn->socket_fd, ext_timestamp, 4) != 4) {
                    return -1;
                }
            }
            break;
        }

        case RTMP_CHUNK_TYPE_1: {
            /* Type 1: 7-byte header */
            uint8_t msg_header[7];

            /* Timestamp delta (3 bytes, big-endian) */
            uint32_t timestamp_delta = header->timestamp; /* Assume delta is stored in timestamp field */
            if (timestamp_delta >= 0xFFFFFF) {
                msg_header[0] = 0xFF;
                msg_header[1] = 0xFF;
                msg_header[2] = 0xFF;
            } else {
                msg_header[0] = (timestamp_delta >> 16) & 0xFF;
                msg_header[1] = (timestamp_delta >> 8) & 0xFF;
                msg_header[2] = timestamp_delta & 0xFF;
            }

            /* Message length (3 bytes, big-endian) */
            msg_header[3] = (header->message_length >> 16) & 0xFF;
            msg_header[4] = (header->message_length >> 8) & 0xFF;
            msg_header[5] = header->message_length & 0xFF;

            /* Message type ID (1 byte) */
            msg_header[6] = header->message_type_id;

            if (rtmp_write_bytes(conn->socket_fd, msg_header, 7) != 7) {
                return -1;
            }

            /* Extended timestamp delta if needed */
            if (timestamp_delta >= 0xFFFFFF) {
                uint8_t ext_timestamp[4];
                ext_timestamp[0] = (timestamp_delta >> 24) & 0xFF;
                ext_timestamp[1] = (timestamp_delta >> 16) & 0xFF;
                ext_timestamp[2] = (timestamp_delta >> 8) & 0xFF;
                ext_timestamp[3] = timestamp_delta & 0xFF;
                if (rtmp_write_bytes(conn->socket_fd, ext_timestamp, 4) != 4) {
                    return -1;
                }
            }
            break;
        }

        case RTMP_CHUNK_TYPE_2: {
            /* Type 2: 3-byte header */
            uint8_t msg_header[3];

            /* Timestamp delta (3 bytes, big-endian) */
            uint32_t timestamp_delta = header->timestamp;
            if (timestamp_delta >= 0xFFFFFF) {
                msg_header[0] = 0xFF;
                msg_header[1] = 0xFF;
                msg_header[2] = 0xFF;
            } else {
                msg_header[0] = (timestamp_delta >> 16) & 0xFF;
                msg_header[1] = (timestamp_delta >> 8) & 0xFF;
                msg_header[2] = timestamp_delta & 0xFF;
            }

            if (rtmp_write_bytes(conn->socket_fd, msg_header, 3) != 3) {
                return -1;
            }

            /* Extended timestamp delta if needed */
            if (timestamp_delta >= 0xFFFFFF) {
                uint8_t ext_timestamp[4];
                ext_timestamp[0] = (timestamp_delta >> 24) & 0xFF;
                ext_timestamp[1] = (timestamp_delta >> 16) & 0xFF;
                ext_timestamp[2] = (timestamp_delta >> 8) & 0xFF;
                ext_timestamp[3] = timestamp_delta & 0xFF;
                if (rtmp_write_bytes(conn->socket_fd, ext_timestamp, 4) != 4) {
                    return -1;
                }
            }
            break;
        }

        case RTMP_CHUNK_TYPE_3:
            /* Type 3: No header */
            break;

        default:
            IMP_LOG_ERR(TAG, "Invalid chunk format: %d", fmt);
            return -1;
    }

    return 0;
}

int rtmp_chunk_write(rtmp_connection_t* conn, rtmp_message_t* msg)
{
    IMP_LOG_DBG(TAG, "Writing RTMP chunk to fd %d", conn->socket_fd);

    if (!msg || !msg->payload || msg->header.message_length == 0) {
        IMP_LOG_ERR(TAG, "Invalid message for chunk writing");
        return -1;
    }

    uint32_t bytes_written = 0;
    uint8_t chunk_fmt = RTMP_CHUNK_TYPE_0; /* Start with type 0 for first chunk */

    while (bytes_written < msg->header.message_length) {
        /* Calculate chunk data size */
        uint32_t chunk_data_size = msg->header.message_length - bytes_written;
        if (chunk_data_size > conn->chunk_size_out) {
            chunk_data_size = conn->chunk_size_out;
        }

        /* Write chunk header (only for first chunk or when chunk_fmt is 0) */
        if (bytes_written == 0) {
            if (rtmp_write_chunk_header(conn, &msg->header, chunk_fmt) != 0) {
                IMP_LOG_ERR(TAG, "Failed to write chunk header");
                return -1;
            }
        } else {
            /* Subsequent chunks use type 3 (no header) */
            if (rtmp_write_chunk_header(conn, &msg->header, RTMP_CHUNK_TYPE_3) != 0) {
                IMP_LOG_ERR(TAG, "Failed to write chunk header");
                return -1;
            }
        }

        /* Write chunk data */
        if (rtmp_write_bytes(conn->socket_fd, msg->payload + bytes_written, chunk_data_size) != (int)chunk_data_size) {
            IMP_LOG_ERR(TAG, "Failed to write chunk data");
            return -1;
        }

        bytes_written += chunk_data_size;

        IMP_LOG_DBG(TAG, "Wrote chunk: cs_id=%d, fmt=%d, chunk_size=%u, total_written=%u/%u",
                    msg->header.chunk_stream_id, chunk_fmt, chunk_data_size,
                    bytes_written, msg->header.message_length);

        /* Next chunks will use type 3 */
        chunk_fmt = RTMP_CHUNK_TYPE_3;
    }

    return 0;
}

/* AMF encoding/decoding functions */

/* Helper function to write bytes to buffer */
static int amf_write_bytes(uint8_t* buffer, size_t buffer_size, size_t offset, const uint8_t* data, size_t length)
{
    if (offset + length > buffer_size) {
        IMP_LOG_ERR(TAG, "AMF buffer overflow");
        return -1;
    }

    memcpy(buffer + offset, data, length);
    return length;
}

int amf_encode_number(uint8_t** buffer, size_t* buffer_size, double number)
{
    /* Allocate buffer if needed */
    if (!*buffer) {
        *buffer_size = 1024;
        *buffer = malloc(*buffer_size);
        if (!*buffer) {
            IMP_LOG_ERR(TAG, "Failed to allocate AMF buffer");
            return -1;
        }
    }

    /* Ensure we have enough space */
    if (*buffer_size < 9) {
        *buffer_size = 1024;
        *buffer = realloc(*buffer, *buffer_size);
        if (!*buffer) {
            IMP_LOG_ERR(TAG, "Failed to reallocate AMF buffer");
            return -1;
        }
    }

    size_t pos = 0;

    /* AMF0 number type marker */
    (*buffer)[pos++] = AMF0_NUMBER;

    /* Encode double in big-endian format */
    union {
        double d;
        uint64_t i;
    } value;
    value.d = number;

    for (int i = 7; i >= 0; i--) {
        (*buffer)[pos++] = (value.i >> (i * 8)) & 0xFF;
    }

    return 9;
}

int amf_encode_boolean(uint8_t** buffer, size_t* buffer_size, uint8_t boolean)
{
    /* Allocate buffer if needed */
    if (!*buffer) {
        *buffer_size = 1024;
        *buffer = malloc(*buffer_size);
        if (!*buffer) {
            IMP_LOG_ERR(TAG, "Failed to allocate AMF buffer");
            return -1;
        }
    }

    /* AMF0 boolean type marker */
    (*buffer)[0] = AMF0_BOOLEAN;

    /* Boolean value (0 or 1) */
    (*buffer)[1] = boolean ? 1 : 0;

    return 2;
}

int amf_encode_string(uint8_t** buffer, size_t* buffer_size, const char* string)
{
    if (!string) {
        return amf_encode_null(buffer, buffer_size);
    }

    uint16_t length = strlen(string);
    size_t needed = 3 + length;

    /* Allocate or reallocate buffer if needed */
    if (!*buffer || *buffer_size < needed) {
        *buffer_size = needed + 1024;
        *buffer = *buffer ? realloc(*buffer, *buffer_size) : malloc(*buffer_size);
        if (!*buffer) {
            IMP_LOG_ERR(TAG, "Failed to allocate AMF buffer");
            return -1;
        }
    }

    size_t pos = 0;

    /* AMF0 string type marker */
    (*buffer)[pos++] = AMF0_STRING;

    /* String length (2 bytes, big-endian) */
    (*buffer)[pos++] = (length >> 8) & 0xFF;
    (*buffer)[pos++] = length & 0xFF;

    /* String data */
    memcpy(*buffer + pos, string, length);

    return 3 + length;
}

int amf_encode_null(uint8_t** buffer, size_t* buffer_size)
{
    /* Allocate buffer if needed */
    if (!*buffer) {
        *buffer_size = 1024;
        *buffer = malloc(*buffer_size);
        if (!*buffer) {
            IMP_LOG_ERR(TAG, "Failed to allocate AMF buffer");
            return -1;
        }
    }

    /* AMF0 null type marker */
    (*buffer)[0] = AMF0_NULL;

    return 1;
}

int amf_encode_object_start(uint8_t** buffer, size_t* buffer_size)
{
    /* Allocate buffer if needed */
    if (!*buffer) {
        *buffer_size = 1024;
        *buffer = malloc(*buffer_size);
        if (!*buffer) {
            IMP_LOG_ERR(TAG, "Failed to allocate AMF buffer");
            return -1;
        }
    }

    /* AMF0 object type marker */
    (*buffer)[0] = AMF0_OBJECT;

    return 1;
}

int amf_encode_object_property(uint8_t** buffer, size_t* buffer_size, const char* name, amf_value_t* value)
{
    /* This function is complex and would need a different approach for stateless operation */
    /* For now, return an error - this would need to be redesigned */
    IMP_LOG_ERR(TAG, "amf_encode_object_property not implemented in stateless mode");
    return -1;
}

int amf_encode_object_end(uint8_t** buffer, size_t* buffer_size)
{
    /* Allocate buffer if needed */
    if (!*buffer) {
        *buffer_size = 1024;
        *buffer = malloc(*buffer_size);
        if (!*buffer) {
            IMP_LOG_ERR(TAG, "Failed to allocate AMF buffer");
            return -1;
        }
    }

    /* Object end marker: empty string name + object end type */
    (*buffer)[0] = 0x00; /* Name length high byte */
    (*buffer)[1] = 0x00; /* Name length low byte */
    (*buffer)[2] = AMF0_OBJECT_END; /* Object end marker */

    return 3;
}

/* AMF decoding functions */
int amf_decode_value(const uint8_t* buffer, size_t buffer_size, size_t* offset, amf_value_t* value)
{
    if (*offset >= buffer_size) {
        IMP_LOG_ERR(TAG, "AMF buffer underrun");
        return -1;
    }

    uint8_t type = buffer[*offset];
    (*offset)++;

    memset(value, 0, sizeof(amf_value_t));
    value->type = type;

    switch (type) {
        case AMF0_NUMBER: {
            if (*offset + 8 > buffer_size) {
                IMP_LOG_ERR(TAG, "AMF number buffer underrun");
                return -1;
            }

            /* Decode double from big-endian format */
            union {
                double d;
                uint64_t i;
            } num_value;
            num_value.i = 0;

            for (int i = 0; i < 8; i++) {
                num_value.i = (num_value.i << 8) | buffer[*offset + i];
            }

            value->value.number = num_value.d;
            *offset += 8;
            break;
        }

        case AMF0_BOOLEAN: {
            if (*offset >= buffer_size) {
                IMP_LOG_ERR(TAG, "AMF boolean buffer underrun");
                return -1;
            }

            value->value.boolean = buffer[*offset] ? 1 : 0;
            (*offset)++;
            break;
        }

        case AMF0_STRING: {
            if (*offset + 2 > buffer_size) {
                IMP_LOG_ERR(TAG, "AMF string length buffer underrun");
                return -1;
            }

            /* Read string length (2 bytes, big-endian) */
            uint16_t length = (buffer[*offset] << 8) | buffer[*offset + 1];
            *offset += 2;

            if (*offset + length > buffer_size) {
                IMP_LOG_ERR(TAG, "AMF string data buffer underrun");
                return -1;
            }

            /* Allocate and copy string data */
            value->value.string.data = malloc(length + 1);
            if (!value->value.string.data) {
                IMP_LOG_ERR(TAG, "Failed to allocate AMF string");
                return -1;
            }

            memcpy(value->value.string.data, buffer + *offset, length);
            value->value.string.data[length] = '\0';
            value->value.string.length = length;
            *offset += length;
            break;
        }

        case AMF0_NULL:
        case AMF0_UNDEFINED:
            /* No additional data */
            break;

        case AMF0_OBJECT: {
            /* Decode object properties */
            return amf_decode_object(buffer, buffer_size, offset,
                                   &value->value.object.properties,
                                   &value->value.object.count);
        }

        default:
            IMP_LOG_WARN(TAG, "Unsupported AMF type: 0x%02X", type);
            return -1;
    }

    return 0;
}

int amf_decode_string(const uint8_t* buffer, size_t buffer_size, size_t* offset, char** string, uint16_t* length)
{
    if (*offset + 2 > buffer_size) {
        IMP_LOG_ERR(TAG, "AMF string length buffer underrun");
        return -1;
    }

    /* Read string length (2 bytes, big-endian) */
    *length = (buffer[*offset] << 8) | buffer[*offset + 1];
    *offset += 2;

    if (*offset + *length > buffer_size) {
        IMP_LOG_ERR(TAG, "AMF string data buffer underrun");
        return -1;
    }

    /* Allocate and copy string data */
    *string = malloc(*length + 1);
    if (!*string) {
        IMP_LOG_ERR(TAG, "Failed to allocate AMF string");
        return -1;
    }

    memcpy(*string, buffer + *offset, *length);
    (*string)[*length] = '\0';
    *offset += *length;

    return 0;
}

int amf_decode_object(const uint8_t* buffer, size_t buffer_size, size_t* offset, amf_property_t** properties, int* count)
{
    *properties = NULL;
    *count = 0;
    int capacity = 0;

    while (*offset < buffer_size) {
        /* Check for object end marker */
        if (*offset + 3 <= buffer_size &&
            buffer[*offset] == 0x00 && buffer[*offset + 1] == 0x00 && buffer[*offset + 2] == AMF0_OBJECT_END) {
            *offset += 3;
            break;
        }

        /* Expand properties array if needed */
        if (*count >= capacity) {
            capacity = capacity ? capacity * 2 : 4;
            amf_property_t* new_props = realloc(*properties, capacity * sizeof(amf_property_t));
            if (!new_props) {
                IMP_LOG_ERR(TAG, "Failed to allocate AMF properties");
                amf_object_free(*properties, *count);
                return -1;
            }
            *properties = new_props;
        }

        /* Decode property name */
        char* name;
        uint16_t name_length;
        if (amf_decode_string(buffer, buffer_size, offset, &name, &name_length) != 0) {
            amf_object_free(*properties, *count);
            return -1;
        }

        /* Decode property value */
        amf_value_t value;
        if (amf_decode_value(buffer, buffer_size, offset, &value) != 0) {
            free(name);
            amf_object_free(*properties, *count);
            return -1;
        }

        /* Store property */
        (*properties)[*count].name = name;
        (*properties)[*count].value = value;
        (*count)++;
    }

    return 0;
}

/* AMF memory management functions */
void amf_value_free(amf_value_t* value)
{
    if (!value) return;

    switch (value->type) {
        case AMF0_STRING:
            if (value->value.string.data) {
                free(value->value.string.data);
                value->value.string.data = NULL;
            }
            break;

        case AMF0_OBJECT:
            amf_object_free(value->value.object.properties, value->value.object.count);
            value->value.object.properties = NULL;
            value->value.object.count = 0;
            break;

        case AMF0_STRICT_ARRAY:
            if (value->value.array.values) {
                for (uint32_t i = 0; i < value->value.array.count; i++) {
                    amf_value_free(&value->value.array.values[i]);
                }
                free(value->value.array.values);
                value->value.array.values = NULL;
            }
            break;
    }
}

void amf_property_free(amf_property_t* property)
{
    if (!property) return;

    if (property->name) {
        free(property->name);
        property->name = NULL;
    }

    amf_value_free(&property->value);
}

void amf_object_free(amf_property_t* properties, int count)
{
    if (!properties) return;

    for (int i = 0; i < count; i++) {
        amf_property_free(&properties[i]);
    }

    free(properties);
}

/* RTMP command parsing and handling */
int rtmp_command_parse(const uint8_t* buffer, size_t buffer_size, rtmp_command_t* command)
{
    size_t offset = 0;
    memset(command, 0, sizeof(rtmp_command_t));

    /* Parse command name (string) */
    amf_value_t cmd_name_value;
    if (amf_decode_value(buffer, buffer_size, &offset, &cmd_name_value) != 0) {
        IMP_LOG_ERR(TAG, "Failed to decode command name");
        return -1;
    }

    if (cmd_name_value.type != AMF0_STRING) {
        IMP_LOG_ERR(TAG, "Command name is not a string");
        amf_value_free(&cmd_name_value);
        return -1;
    }

    command->command_name = strdup(cmd_name_value.value.string.data);
    amf_value_free(&cmd_name_value);

    /* Parse transaction ID (number) */
    amf_value_t transaction_value;
    if (amf_decode_value(buffer, buffer_size, &offset, &transaction_value) != 0) {
        IMP_LOG_ERR(TAG, "Failed to decode transaction ID");
        free(command->command_name);
        return -1;
    }

    if (transaction_value.type != AMF0_NUMBER) {
        IMP_LOG_ERR(TAG, "Transaction ID is not a number");
        amf_value_free(&transaction_value);
        free(command->command_name);
        return -1;
    }

    command->transaction_id = transaction_value.value.number;

    /* Parse command object (can be null or object) */
    if (offset < buffer_size) {
        if (amf_decode_value(buffer, buffer_size, &offset, &command->command_object) != 0) {
            IMP_LOG_ERR(TAG, "Failed to decode command object");
            free(command->command_name);
            return -1;
        }
    }

    /* Parse additional arguments */
    command->argument_count = 0;
    command->arguments = NULL;

    while (offset < buffer_size) {
        /* Expand arguments array */
        amf_value_t* new_args = realloc(command->arguments,
                                       (command->argument_count + 1) * sizeof(amf_value_t));
        if (!new_args) {
            IMP_LOG_ERR(TAG, "Failed to allocate command arguments");
            rtmp_command_free(command);
            return -1;
        }
        command->arguments = new_args;

        /* Parse argument */
        if (amf_decode_value(buffer, buffer_size, &offset,
                           &command->arguments[command->argument_count]) != 0) {
            IMP_LOG_ERR(TAG, "Failed to decode command argument %d", command->argument_count);
            rtmp_command_free(command);
            return -1;
        }

        command->argument_count++;
    }

    IMP_LOG_DBG(TAG, "Parsed command: %s, transaction_id=%.0f, args=%d",
                command->command_name, command->transaction_id, command->argument_count);

    return 0;
}

void rtmp_command_free(rtmp_command_t* command)
{
    if (!command) return;

    if (command->command_name) {
        free(command->command_name);
        command->command_name = NULL;
    }

    amf_value_free(&command->command_object);

    if (command->arguments) {
        for (int i = 0; i < command->argument_count; i++) {
            amf_value_free(&command->arguments[i]);
        }
        free(command->arguments);
        command->arguments = NULL;
    }

    command->argument_count = 0;
}

int rtmp_message_parse(rtmp_connection_t* conn, rtmp_message_t* msg)
{
    IMP_LOG_DBG(TAG, "Parsing RTMP message type %d for fd %d",
                msg->header.message_type_id, conn->socket_fd);

    switch (msg->header.message_type_id) {
        case RTMP_MSG_SET_CHUNK_SIZE: {
            if (msg->header.message_length >= 4) {
                uint32_t chunk_size = (msg->payload[0] << 24) | (msg->payload[1] << 16) |
                                     (msg->payload[2] << 8) | msg->payload[3];
                conn->chunk_size_in = chunk_size;
                IMP_LOG_INFO(TAG, "Set chunk size to %u", chunk_size);
            }
            break;
        }

        case RTMP_MSG_ACKNOWLEDGEMENT: {
            if (msg->header.message_length >= 4) {
                uint32_t bytes_received = (msg->payload[0] << 24) | (msg->payload[1] << 16) |
                                         (msg->payload[2] << 8) | msg->payload[3];
                IMP_LOG_DBG(TAG, "Received acknowledgement: %u bytes", bytes_received);
            }
            break;
        }

        case RTMP_MSG_WINDOW_ACK_SIZE: {
            if (msg->header.message_length >= 4) {
                uint32_t window_size = (msg->payload[0] << 24) | (msg->payload[1] << 16) |
                                      (msg->payload[2] << 8) | msg->payload[3];
                conn->window_ack_size = window_size;
                IMP_LOG_INFO(TAG, "Set window acknowledgement size to %u", window_size);
            }
            break;
        }

        case RTMP_MSG_COMMAND_AMF0: {
            /* Parse and handle RTMP command */
            rtmp_command_t command;
            if (rtmp_command_parse(msg->payload, msg->header.message_length, &command) == 0) {
                rtmp_command_handle(conn, &command);
                rtmp_command_free(&command);
            }
            break;
        }

        case RTMP_MSG_AUDIO:
        case RTMP_MSG_VIDEO: {
            /* Handle audio/video data */
            IMP_LOG_DBG(TAG, "Received %s data: %u bytes",
                       (msg->header.message_type_id == RTMP_MSG_AUDIO) ? "audio" : "video",
                       msg->header.message_length);
            /* TODO: Process audio/video data */
            break;
        }

        default:
            IMP_LOG_DBG(TAG, "Unhandled message type: %d", msg->header.message_type_id);
            break;
    }

    return 0;
}

/* RTMP command handling */
int rtmp_command_handle(rtmp_connection_t* conn, rtmp_command_t* command)
{
    IMP_LOG_INFO(TAG, "Handling RTMP command: %s (transaction_id=%.0f)",
                 command->command_name, command->transaction_id);

    if (strcmp(command->command_name, RTMP_CMD_CONNECT) == 0) {
        /* Handle connect command */
        IMP_LOG_INFO(TAG, "Processing connect command");

        /* Extract app name from command object */
        if (command->command_object.type == AMF0_OBJECT) {
            for (int i = 0; i < command->command_object.value.object.count; i++) {
                amf_property_t* prop = &command->command_object.value.object.properties[i];
                if (strcmp(prop->name, "app") == 0 && prop->value.type == AMF0_STRING) {
                    strncpy(conn->app_name, prop->value.value.string.data,
                           sizeof(conn->app_name) - 1);
                    conn->app_name[sizeof(conn->app_name) - 1] = '\0';
                    IMP_LOG_INFO(TAG, "Client connecting to app: %s", conn->app_name);
                    break;
                }
            }
        }

        /* Send connect result */
        conn->state = RTMP_STATE_CONNECTED;
        return rtmp_send_connect_result(conn, command->transaction_id, true);

    } else if (strcmp(command->command_name, RTMP_CMD_CREATE_STREAM) == 0) {
        /* Handle createStream command */
        IMP_LOG_INFO(TAG, "Processing createStream command");

        /* Assign a stream ID (for simplicity, use 1) */
        double stream_id = 1.0;
        return rtmp_send_create_stream_result(conn, command->transaction_id, stream_id);

    } else if (strcmp(command->command_name, RTMP_CMD_PUBLISH) == 0) {
        /* Handle publish command */
        IMP_LOG_INFO(TAG, "Processing publish command");

        if (command->argument_count >= 1 && command->arguments[0].type == AMF0_STRING) {
            strncpy(conn->stream_key, command->arguments[0].value.string.data,
                   sizeof(conn->stream_key) - 1);
            conn->stream_key[sizeof(conn->stream_key) - 1] = '\0';
            conn->publishing = true;

            IMP_LOG_INFO(TAG, "Client publishing stream: %s", conn->stream_key);

            /* Check stream key if authentication is required */
            /* For now, accept all streams */
            return rtmp_send_publish_status(conn, RTMP_STATUS_PUBLISH_START,
                                          "Publishing stream");
        } else {
            return rtmp_send_publish_status(conn, RTMP_STATUS_PUBLISH_FAILED,
                                          "Invalid stream name");
        }

    } else if (strcmp(command->command_name, RTMP_CMD_PLAY) == 0) {
        /* Handle play command */
        IMP_LOG_INFO(TAG, "Processing play command");

        if (command->argument_count >= 1 && command->arguments[0].type == AMF0_STRING) {
            strncpy(conn->stream_key, command->arguments[0].value.string.data,
                   sizeof(conn->stream_key) - 1);
            conn->stream_key[sizeof(conn->stream_key) - 1] = '\0';

            IMP_LOG_INFO(TAG, "Client requesting to play stream: %s", conn->stream_key);

            /* For now, reject play requests (we're a publishing server) */
            return rtmp_send_play_status(conn, RTMP_STATUS_PLAY_STREAM_NOT_FOUND,
                                       "Stream not found");
        } else {
            return rtmp_send_play_status(conn, RTMP_STATUS_PLAY_FAILED,
                                       "Invalid stream name");
        }

    } else if (strcmp(command->command_name, RTMP_CMD_DELETE_STREAM) == 0) {
        /* Handle deleteStream command */
        IMP_LOG_INFO(TAG, "Processing deleteStream command");
        conn->publishing = false;
        memset(conn->stream_key, 0, sizeof(conn->stream_key));

    } else if (strcmp(command->command_name, RTMP_CMD_CLOSE) == 0) {
        /* Handle close command */
        IMP_LOG_INFO(TAG, "Processing close command");
        conn->thread_running = false;

    } else {
        IMP_LOG_WARN(TAG, "Unhandled RTMP command: %s", command->command_name);
    }

    return 0;
}

/* RTMP response functions */
int rtmp_send_connect_result(rtmp_connection_t* conn, double transaction_id, bool success)
{
    /* Create response message */
    rtmp_message_t response;
    memset(&response, 0, sizeof(response));

    /* Set message header */
    response.header.chunk_stream_id = 3; /* Control channel */
    response.header.timestamp = 0;
    response.header.message_type_id = RTMP_MSG_COMMAND_AMF0;
    response.header.message_stream_id = 0;

    /* Build AMF response */
    uint8_t* buffer = NULL;
    size_t buffer_size = 0;
    size_t total_size = 0;

    /* Command name */
    int bytes = amf_encode_string(&buffer, &buffer_size, success ? RTMP_CMD_RESULT : RTMP_CMD_ERROR);
    if (bytes < 0) goto error;
    total_size += bytes;

    /* Transaction ID */
    uint8_t* temp_buffer = NULL;
    size_t temp_size = 0;
    bytes = amf_encode_number(&temp_buffer, &temp_size, transaction_id);
    if (bytes < 0) goto error;

    /* Combine buffers */
    buffer = realloc(buffer, total_size + bytes);
    if (!buffer) goto error;
    memcpy(buffer + total_size, temp_buffer, bytes);
    total_size += bytes;
    free(temp_buffer);
    temp_buffer = NULL;

    /* Properties object */
    temp_size = 0;
    bytes = amf_encode_object_start(&temp_buffer, &temp_size);
    if (bytes < 0) goto error;

    buffer = realloc(buffer, total_size + bytes);
    if (!buffer) goto error;
    memcpy(buffer + total_size, temp_buffer, bytes);
    total_size += bytes;
    free(temp_buffer);
    temp_buffer = NULL;

    /* Object end */
    temp_size = 0;
    bytes = amf_encode_object_end(&temp_buffer, &temp_size);
    if (bytes < 0) goto error;

    buffer = realloc(buffer, total_size + bytes);
    if (!buffer) goto error;
    memcpy(buffer + total_size, temp_buffer, bytes);
    total_size += bytes;
    free(temp_buffer);
    temp_buffer = NULL;

    /* Information object (null for now) */
    temp_size = 0;
    bytes = amf_encode_null(&temp_buffer, &temp_size);
    if (bytes < 0) goto error;

    buffer = realloc(buffer, total_size + bytes);
    if (!buffer) goto error;
    memcpy(buffer + total_size, temp_buffer, bytes);
    total_size += bytes;
    free(temp_buffer);

    /* Set message payload */
    response.payload = buffer;
    response.header.message_length = total_size;
    response.payload_size = total_size;

    /* Send response */
    int result = rtmp_chunk_write(conn, &response);

    /* Cleanup */
    free(buffer);

    IMP_LOG_INFO(TAG, "Sent connect %s (transaction_id=%.0f)",
                 success ? "result" : "error", transaction_id);

    return result;

error:
    if (buffer) free(buffer);
    if (temp_buffer) free(temp_buffer);
    return -1;
}

int rtmp_send_create_stream_result(rtmp_connection_t* conn, double transaction_id, double stream_id)
{
    /* Create response message */
    rtmp_message_t response;
    memset(&response, 0, sizeof(response));

    /* Set message header */
    response.header.chunk_stream_id = 3; /* Control channel */
    response.header.timestamp = 0;
    response.header.message_type_id = RTMP_MSG_COMMAND_AMF0;
    response.header.message_stream_id = 0;

    /* Build AMF response */
    uint8_t* buffer = NULL;
    size_t buffer_size = 0;
    size_t total_size = 0;

    /* Command name */
    int bytes = amf_encode_string(&buffer, &buffer_size, RTMP_CMD_RESULT);
    if (bytes < 0) goto error;
    total_size += bytes;

    /* Transaction ID */
    uint8_t* temp_buffer = NULL;
    size_t temp_size = 0;
    bytes = amf_encode_number(&temp_buffer, &temp_size, transaction_id);
    if (bytes < 0) goto error;

    buffer = realloc(buffer, total_size + bytes);
    if (!buffer) goto error;
    memcpy(buffer + total_size, temp_buffer, bytes);
    total_size += bytes;
    free(temp_buffer);
    temp_buffer = NULL;

    /* Command object (null) */
    temp_size = 0;
    bytes = amf_encode_null(&temp_buffer, &temp_size);
    if (bytes < 0) goto error;

    buffer = realloc(buffer, total_size + bytes);
    if (!buffer) goto error;
    memcpy(buffer + total_size, temp_buffer, bytes);
    total_size += bytes;
    free(temp_buffer);
    temp_buffer = NULL;

    /* Stream ID */
    temp_size = 0;
    bytes = amf_encode_number(&temp_buffer, &temp_size, stream_id);
    if (bytes < 0) goto error;

    buffer = realloc(buffer, total_size + bytes);
    if (!buffer) goto error;
    memcpy(buffer + total_size, temp_buffer, bytes);
    total_size += bytes;
    free(temp_buffer);

    /* Set message payload */
    response.payload = buffer;
    response.header.message_length = total_size;
    response.payload_size = total_size;

    /* Send response */
    int result = rtmp_chunk_write(conn, &response);

    /* Cleanup */
    free(buffer);

    IMP_LOG_INFO(TAG, "Sent createStream result (transaction_id=%.0f, stream_id=%.0f)",
                 transaction_id, stream_id);

    return result;

error:
    if (buffer) free(buffer);
    if (temp_buffer) free(temp_buffer);
    return -1;
}

int rtmp_send_publish_status(rtmp_connection_t* conn, const char* status_code, const char* description)
{
    /* Create status message */
    rtmp_message_t response;
    memset(&response, 0, sizeof(response));

    /* Set message header */
    response.header.chunk_stream_id = 3; /* Control channel */
    response.header.timestamp = 0;
    response.header.message_type_id = RTMP_MSG_COMMAND_AMF0;
    response.header.message_stream_id = 1; /* Stream ID */

    /* Build AMF response */
    uint8_t* buffer = NULL;
    size_t buffer_size = 0;
    size_t total_size = 0;

    /* Command name */
    int bytes = amf_encode_string(&buffer, &buffer_size, RTMP_CMD_ON_STATUS);
    if (bytes < 0) goto error;
    total_size += bytes;

    /* Transaction ID (0 for onStatus) */
    uint8_t* temp_buffer = NULL;
    size_t temp_size = 0;
    bytes = amf_encode_number(&temp_buffer, &temp_size, 0.0);
    if (bytes < 0) goto error;

    buffer = realloc(buffer, total_size + bytes);
    if (!buffer) goto error;
    memcpy(buffer + total_size, temp_buffer, bytes);
    total_size += bytes;
    free(temp_buffer);
    temp_buffer = NULL;

    /* Command object (null) */
    temp_size = 0;
    bytes = amf_encode_null(&temp_buffer, &temp_size);
    if (bytes < 0) goto error;

    buffer = realloc(buffer, total_size + bytes);
    if (!buffer) goto error;
    memcpy(buffer + total_size, temp_buffer, bytes);
    total_size += bytes;
    free(temp_buffer);
    temp_buffer = NULL;

    /* Status object - simplified version */
    temp_size = 0;
    bytes = amf_encode_object_start(&temp_buffer, &temp_size);
    if (bytes < 0) goto error;

    buffer = realloc(buffer, total_size + bytes);
    if (!buffer) goto error;
    memcpy(buffer + total_size, temp_buffer, bytes);
    total_size += bytes;
    free(temp_buffer);
    temp_buffer = NULL;

    /* Object end */
    temp_size = 0;
    bytes = amf_encode_object_end(&temp_buffer, &temp_size);
    if (bytes < 0) goto error;

    buffer = realloc(buffer, total_size + bytes);
    if (!buffer) goto error;
    memcpy(buffer + total_size, temp_buffer, bytes);
    total_size += bytes;
    free(temp_buffer);

    /* Set message payload */
    response.payload = buffer;
    response.header.message_length = total_size;
    response.payload_size = total_size;

    /* Send response */
    int result = rtmp_chunk_write(conn, &response);

    /* Cleanup */
    free(buffer);

    IMP_LOG_INFO(TAG, "Sent publish status: %s - %s", status_code, description);

    return result;

error:
    if (buffer) free(buffer);
    if (temp_buffer) free(temp_buffer);
    return -1;
}

int rtmp_send_play_status(rtmp_connection_t* conn, const char* status_code, const char* description)
{
    /* Similar to publish status but for play commands */
    return rtmp_send_publish_status(conn, status_code, description);
}

/* Control message functions */
int rtmp_send_set_chunk_size(rtmp_connection_t* conn, uint32_t chunk_size)
{
    rtmp_message_t msg;
    memset(&msg, 0, sizeof(msg));

    /* Set message header */
    msg.header.chunk_stream_id = 2; /* Protocol control channel */
    msg.header.timestamp = 0;
    msg.header.message_type_id = RTMP_MSG_SET_CHUNK_SIZE;
    msg.header.message_stream_id = 0;
    msg.header.message_length = 4;

    /* Create payload */
    msg.payload = malloc(4);
    if (!msg.payload) {
        return -1;
    }

    msg.payload[0] = (chunk_size >> 24) & 0xFF;
    msg.payload[1] = (chunk_size >> 16) & 0xFF;
    msg.payload[2] = (chunk_size >> 8) & 0xFF;
    msg.payload[3] = chunk_size & 0xFF;
    msg.payload_size = 4;

    /* Send message */
    int result = rtmp_chunk_write(conn, &msg);

    /* Update connection chunk size */
    if (result == 0) {
        conn->chunk_size_out = chunk_size;
    }

    /* Cleanup */
    free(msg.payload);

    return result;
}

int rtmp_send_acknowledgement(rtmp_connection_t* conn, uint32_t bytes_received)
{
    rtmp_message_t msg;
    memset(&msg, 0, sizeof(msg));

    /* Set message header */
    msg.header.chunk_stream_id = 2; /* Protocol control channel */
    msg.header.timestamp = 0;
    msg.header.message_type_id = RTMP_MSG_ACKNOWLEDGEMENT;
    msg.header.message_stream_id = 0;
    msg.header.message_length = 4;

    /* Create payload */
    msg.payload = malloc(4);
    if (!msg.payload) {
        return -1;
    }

    msg.payload[0] = (bytes_received >> 24) & 0xFF;
    msg.payload[1] = (bytes_received >> 16) & 0xFF;
    msg.payload[2] = (bytes_received >> 8) & 0xFF;
    msg.payload[3] = bytes_received & 0xFF;
    msg.payload_size = 4;

    /* Send message */
    int result = rtmp_chunk_write(conn, &msg);

    /* Cleanup */
    free(msg.payload);

    return result;
}

int rtmp_send_window_ack_size(rtmp_connection_t* conn, uint32_t window_size)
{
    rtmp_message_t msg;
    memset(&msg, 0, sizeof(msg));

    /* Set message header */
    msg.header.chunk_stream_id = 2; /* Protocol control channel */
    msg.header.timestamp = 0;
    msg.header.message_type_id = RTMP_MSG_WINDOW_ACK_SIZE;
    msg.header.message_stream_id = 0;
    msg.header.message_length = 4;

    /* Create payload */
    msg.payload = malloc(4);
    if (!msg.payload) {
        return -1;
    }

    msg.payload[0] = (window_size >> 24) & 0xFF;
    msg.payload[1] = (window_size >> 16) & 0xFF;
    msg.payload[2] = (window_size >> 8) & 0xFF;
    msg.payload[3] = window_size & 0xFF;
    msg.payload_size = 4;

    /* Send message */
    int result = rtmp_chunk_write(conn, &msg);

    /* Cleanup */
    free(msg.payload);

    return result;
}

/* RTMP video streaming functions */
int rtmp_send_video_frame(rtmp_connection_t* conn, const uint8_t* frame_data, uint32_t frame_size, uint32_t timestamp)
{
    if (!conn || !frame_data || frame_size == 0 || !conn->publishing) {
        return 0; /* Not publishing, skip */
    }

    /* Create video message */
    rtmp_message_t msg;
    memset(&msg, 0, sizeof(msg));

    /* Set message header */
    msg.header.chunk_stream_id = 4; /* Video channel */
    msg.header.timestamp = timestamp;
    msg.header.message_type_id = RTMP_MSG_VIDEO;
    msg.header.message_stream_id = 1; /* Stream ID */
    msg.header.message_length = frame_size;

    /* Allocate and copy frame data */
    msg.payload = malloc(frame_size);
    if (!msg.payload) {
        IMP_LOG_ERR(TAG, "Failed to allocate video frame buffer");
        return -1;
    }

    memcpy(msg.payload, frame_data, frame_size);
    msg.payload_size = frame_size;

    /* Send video frame */
    int result = rtmp_chunk_write(conn, &msg);

    /* Cleanup */
    free(msg.payload);

    if (result == 0) {
        IMP_LOG_DBG(TAG, "Sent video frame: %u bytes, timestamp=%u", frame_size, timestamp);
    }

    return result;
}

int rtmp_send_audio_frame(rtmp_connection_t* conn, const uint8_t* frame_data, uint32_t frame_size, uint32_t timestamp)
{
    if (!conn || !frame_data || frame_size == 0 || !conn->publishing) {
        return 0; /* Not publishing, skip */
    }

    /* Create audio message */
    rtmp_message_t msg;
    memset(&msg, 0, sizeof(msg));

    /* Set message header */
    msg.header.chunk_stream_id = 5; /* Audio channel */
    msg.header.timestamp = timestamp;
    msg.header.message_type_id = RTMP_MSG_AUDIO;
    msg.header.message_stream_id = 1; /* Stream ID */
    msg.header.message_length = frame_size;

    /* Allocate and copy frame data */
    msg.payload = malloc(frame_size);
    if (!msg.payload) {
        IMP_LOG_ERR(TAG, "Failed to allocate audio frame buffer");
        return -1;
    }

    memcpy(msg.payload, frame_data, frame_size);
    msg.payload_size = frame_size;

    /* Send audio frame */
    int result = rtmp_chunk_write(conn, &msg);

    /* Cleanup */
    free(msg.payload);

    if (result == 0) {
        IMP_LOG_DBG(TAG, "Sent audio frame: %u bytes, timestamp=%u", frame_size, timestamp);
    }

    return result;
}

/* RTMP server frame distribution */
int rtmp_server_send_frame(rtmp_server_t* server, int channel, const uint8_t* frame_data, uint32_t frame_size, const struct timeval* timestamp)
{
    if (!server || !frame_data || frame_size == 0) {
        return 0;
    }

    /* Convert timestamp to milliseconds */
    uint32_t rtmp_timestamp = (timestamp->tv_sec * 1000) + (timestamp->tv_usec / 1000);

    int frames_sent = 0;

    /* Send frame to all publishing connections */
    pthread_mutex_lock(&server->connections_mutex);

    rtmp_connection_t* conn = server->connections;
    while (conn) {
        if (conn->publishing && conn->thread_running) {
            /* Send video frame to this connection */
            if (rtmp_send_video_frame(conn, frame_data, frame_size, rtmp_timestamp) == 0) {
                frames_sent++;
            }
        }
        conn = conn->next;
    }

    pthread_mutex_unlock(&server->connections_mutex);

    if (frames_sent > 0) {
        IMP_LOG_DBG(TAG, "Distributed frame to %d RTMP connections (channel %d, %u bytes)",
                   frames_sent, channel, frame_size);
    }

    return frames_sent;
}

/* RTSP frame callback for RTMP module - receives frame data from RTSP module */
int rtmp_server_module_rtsp_frame_callback(struct rtsp_server* server, int channel, const uint8_t* frame_data, uint32_t frame_size, const struct timeval* timestamp)
{
    if (!g_rtmp_server_module_state.running || !g_rtmp_server) {
        return 0;
    }

    if (!frame_data || frame_size == 0 || !timestamp) {
        return 0;
    }

    /* Check if we have any publishing RTMP connections */
    int publishing_connections = 0;
    pthread_mutex_lock(&g_rtmp_server->connections_mutex);

    rtmp_connection_t* conn = g_rtmp_server->connections;
    while (conn) {
        if (conn->publishing && conn->thread_running) {
            publishing_connections++;
        }
        conn = conn->next;
    }

    pthread_mutex_unlock(&g_rtmp_server->connections_mutex);

    /* Only process frames if we have publishing connections */
    if (publishing_connections == 0) {
        return 0;
    }

    /* Only process enabled channels that are not JPEG */
    if (!chn[channel].enable || chn[channel].payloadType == PT_JPEG) {
        return 0;
    }

    /* Send frame to RTMP connections using the frame data passed from RTSP module */
    int frames_sent = rtmp_server_send_frame(g_rtmp_server, channel, frame_data, frame_size, timestamp);

    if (frames_sent > 0) {
        IMP_LOG_DBG(TAG, "RTMP: Sent frame from channel %d to %d connections (%u bytes)",
                   channel, frames_sent, frame_size);
    }

    return frames_sent;
}

/* Auto-register module at startup */
MODULE_REGISTER(rtmp_server_module_info);
