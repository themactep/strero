/*
 * http_module.c - HTTP Server Module Implementation
 * Copyright (C) 2025 Paul Philippov, Thingino Project
 */

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <stdarg.h>

#include <arpa/inet.h>
#include <json-c/json.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/time.h>

#include "http_module.h"
#include "http_router.h"
#include "../../auth_utils.h"
#include "../../common.h"
#include "../../config.h"

#include "hal/imp.h"

#define TAG "HTTP_MODULE"

/* External references */
extern struct streamer_config* g_config;

/* Module state */
static struct {
    bool initialized;
    bool running;
    http_module_config_t config;
    pthread_t server_thread;
    bool thread_should_exit;
    int server_socket;
} g_http_module_state = {0};

/* Global state variables for compatibility with existing endpoint handlers */
int http_server_socket = -1;
pthread_t http_thread;

/* Forward declarations */
static void* http_module_server_thread(void* arg);
static void handle_api_overview_request(int client_socket);
static void handle_api_overview_wrapper(int client_socket, const char* request);
static void* mjpeg_stream_thread(void* arg);
static int http_register_core_routes(void);

/* Core route handlers */
static void handle_status_json(int client_socket, const char* request);
static void handle_config_json(int client_socket, const char* request);
static void handle_info_json(int client_socket, const char* request);
static void handle_health_json(int client_socket, const char* request);
static void handle_snap0_jpg(int client_socket, const char* request);
static void handle_snap1_jpg(int client_socket, const char* request);
static void handle_snap2_jpg(int client_socket, const char* request);
static void handle_snap3_jpg(int client_socket, const char* request);
static void handle_stream0_mjpeg(int client_socket, const char* request);
static void handle_stream1_mjpeg(int client_socket, const char* request);

/* Image grab fallback handlers */
static void handle_image_grab_fallback(int client_socket, const char* request);
static int http_register_image_grab_fallback_routes(void);
static void handle_stream2_mjpeg(int client_socket, const char* request);
static void handle_stream3_mjpeg(int client_socket, const char* request);
static void handle_stream0_h264(int client_socket, const char* request);
static void handle_stream1_h264(int client_socket, const char* request);
static void handle_stream2_h264(int client_socket, const char* request);
static void handle_stream3_h264(int client_socket, const char* request);

/* Structure for passing data to MJPEG thread */
typedef struct {
    int client_socket;
    int channel;
} mjpeg_thread_data_t;

/* Module lifecycle functions */
int http_module_init(void* config)
{
    IMP_LOG_INFO(TAG, "Initializing HTTP module");

    if (g_http_module_state.initialized) {
        IMP_LOG_WARN(TAG, "HTTP module already initialized");
        return 0;
    }

    if (!config) {
        IMP_LOG_ERR(TAG, "Invalid configuration provided");
        return -1;
    }

    /* Copy configuration */
    memcpy(&g_http_module_state.config, config, sizeof(http_module_config_t));

    /* Debug: Show what configuration was copied */
    IMP_LOG_DBG(TAG, "HTTP module init - copied config: enabled=%s, port=%d, endpoints.snapshots=%s",
                g_http_module_state.config.enabled ? "true" : "false",
                g_http_module_state.config.port,
                g_http_module_state.config.endpoints.snapshots ? "true" : "false");

    /* Initialize HTTP server resources */
    g_http_module_state.server_socket = -1;
    g_http_module_state.thread_should_exit = false;

    g_http_module_state.initialized = true;
    IMP_LOG_INFO(TAG, "HTTP module initialized successfully");
    return 0;
}

int http_module_start(void)
{
    IMP_LOG_INFO(TAG, "Starting HTTP module");

    if (!g_http_module_state.initialized) {
        IMP_LOG_ERR(TAG, "HTTP module not initialized");
        return -1;
    }

    if (g_http_module_state.running) {
        IMP_LOG_WARN(TAG, "HTTP module already running");
        return 0;
    }

    if (!g_http_module_state.config.enabled) {
        IMP_LOG_INFO(TAG, "HTTP module disabled in configuration");
        return 0;
    }

    /* Initialize HTTP router */
    if (http_router_init() < 0) {
        IMP_LOG_ERR(TAG, "Failed to initialize HTTP router");
        return -1;
    }

    /* Register core HTTP routes */
    if (http_register_core_routes() < 0) {
        IMP_LOG_ERR(TAG, "Failed to register core HTTP routes");
        http_router_cleanup();
        return -1;
    }

    /* Register fallback routes for image_grab if module is disabled */
#ifdef ENABLE_IMAGE_GRAB
    /* Check if image_grab module is enabled */
    extern int is_image_grab_module_enabled(void);
    if (!is_image_grab_module_enabled()) {
        IMP_LOG_INFO(TAG, "Image grab module disabled, registering fallback routes");
        if (http_register_image_grab_fallback_routes() < 0) {
            IMP_LOG_ERR(TAG, "Failed to register image grab fallback routes");
            http_router_cleanup();
            return -1;
        }
    }
#endif

    /* Module routes are now registered by modules themselves during their start() phase */
    /* This ensures routes are only registered when modules are actually enabled */

#ifdef ENABLE_METRICS
    extern int metrics_register_routes(void);
    if (metrics_register_routes() < 0) {
        IMP_LOG_ERR(TAG, "Failed to register metrics routes");
        http_router_cleanup();
        return -1;
    }
#endif

#ifdef ENABLE_IMP_CONTROL
    extern int imp_control_register_routes(void);
    if (imp_control_register_routes() < 0) {
        IMP_LOG_ERR(TAG, "Failed to register IMP control routes");
        http_router_cleanup();
        return -1;
    }
#endif

#ifdef ENABLE_ONVIF
    extern int onvif_register_routes(void);
    if (onvif_register_routes() < 0) {
        IMP_LOG_ERR(TAG, "Failed to register ONVIF routes");
        http_router_cleanup();
        return -1;
    }
#endif

    /* Ignore SIGPIPE to prevent crashes when clients disconnect */
    signal(SIGPIPE, SIG_IGN);

    /* Start HTTP server thread */
    g_http_module_state.thread_should_exit = false;
    if (pthread_create(&g_http_module_state.server_thread, NULL, http_module_server_thread, NULL) != 0) {
        IMP_LOG_ERR(TAG, "Failed to create HTTP server thread");
        return -1;
    }

    g_http_module_state.running = true;

    /* Update global variables for compatibility */
    http_server_running = true;
    http_server_socket = g_http_module_state.server_socket;
    http_thread = g_http_module_state.server_thread;

    IMP_LOG_INFO(TAG, "HTTP module started successfully on port %d", g_http_module_state.config.port);
    return 0;
}

int http_module_stop(void)
{
    IMP_LOG_INFO(TAG, "Stopping HTTP module");

    if (!g_http_module_state.running) {
        IMP_LOG_WARN(TAG, "HTTP module not running");
        return 0;
    }

    /* Signal thread to exit */
    g_http_module_state.thread_should_exit = true;

    /* Close server socket to unblock accept() */
    if (g_http_module_state.server_socket >= 0) {
        close(g_http_module_state.server_socket);
        g_http_module_state.server_socket = -1;
    }

    /* Wait for thread to finish */
    if (pthread_join(g_http_module_state.server_thread, NULL) != 0) {
        IMP_LOG_ERR(TAG, "Failed to join HTTP server thread");
        return -1;
    }

    g_http_module_state.running = false;

    /* Cleanup HTTP router */
    http_router_cleanup();

    /* Update global variables for compatibility */
    http_server_running = false;
    http_server_socket = -1;

    IMP_LOG_INFO(TAG, "HTTP module stopped successfully");
    return 0;
}

int http_module_cleanup(void)
{
    IMP_LOG_INFO(TAG, "Cleaning up HTTP module");

    if (g_http_module_state.running) {
        http_module_stop();
    }

    if (!g_http_module_state.initialized) {
        return 0;
    }

    /* Cleanup HTTP server resources */
    if (g_http_module_state.server_socket >= 0) {
        close(g_http_module_state.server_socket);
        g_http_module_state.server_socket = -1;
    }

    /* Reset state */
    memset(&g_http_module_state, 0, sizeof(g_http_module_state));

    IMP_LOG_INFO(TAG, "HTTP module cleaned up successfully");
    return 0;
}

int http_module_get_config_size(void)
{
    return sizeof(http_module_config_t);
}

int http_module_config_parse(json_object* json, void* config)
{
    if (!json || !config) {
        return -1;
    }

    http_module_config_t* http_config = (http_module_config_t*)config;

    /* Parse enabled flag */
    json_object* enabled_obj;
    if (json_object_object_get_ex(json, "enabled", &enabled_obj)) {
        http_config->enabled = json_object_get_boolean(enabled_obj);
    }

    /* Parse port */
    json_object* port_obj;
    if (json_object_object_get_ex(json, "port", &port_obj)) {
        http_config->port = json_object_get_int(port_obj);
    }

    /* Parse max_connections */
    json_object* max_conn_obj;
    if (json_object_object_get_ex(json, "max_connections", &max_conn_obj)) {
        http_config->max_connections = json_object_get_int(max_conn_obj);
    }

    /* Parse request_timeout_ms */
    json_object* timeout_obj;
    if (json_object_object_get_ex(json, "request_timeout_ms", &timeout_obj)) {
        http_config->request_timeout_ms = json_object_get_int(timeout_obj);
    }

    /* Parse endpoints configuration */
    json_object* endpoints_obj;
    if (json_object_object_get_ex(json, "endpoints", &endpoints_obj)) {
        json_object* snapshots_obj;
        if (json_object_object_get_ex(endpoints_obj, "snapshots", &snapshots_obj)) {
            http_config->endpoints.snapshots = json_object_get_boolean(snapshots_obj);
        }

        json_object* mjpeg_obj;
        if (json_object_object_get_ex(endpoints_obj, "mjpeg_streams", &mjpeg_obj)) {
            http_config->endpoints.mjpeg_streams = json_object_get_boolean(mjpeg_obj);
        }

        json_object* json_api_obj;
        if (json_object_object_get_ex(endpoints_obj, "json_api", &json_api_obj)) {
            http_config->endpoints.json_api = json_object_get_boolean(json_api_obj);
        }

        json_object* metrics_obj;
        if (json_object_object_get_ex(endpoints_obj, "metrics", &metrics_obj)) {
            http_config->endpoints.metrics = json_object_get_boolean(metrics_obj);
        }

        json_object* onvif_obj;
        if (json_object_object_get_ex(endpoints_obj, "onvif", &onvif_obj)) {
            http_config->endpoints.onvif = json_object_get_boolean(onvif_obj);
        }

        json_object* api_overview_obj;
        if (json_object_object_get_ex(endpoints_obj, "api_overview", &api_overview_obj)) {
            http_config->endpoints.api_overview = json_object_get_boolean(api_overview_obj);
        }
    }

    /* Parse authentication configuration */
    json_object* auth_obj;
    if (json_object_object_get_ex(json, "auth", &auth_obj)) {
        json_object* auth_enabled_obj;
        if (json_object_object_get_ex(auth_obj, "enabled", &auth_enabled_obj)) {
            http_config->auth.enabled = json_object_get_boolean(auth_enabled_obj);
        }

        json_object* localhost_bypass_obj;
        if (json_object_object_get_ex(auth_obj, "localhost_bypass", &localhost_bypass_obj)) {
            http_config->auth.localhost_bypass = json_object_get_boolean(localhost_bypass_obj);
        }

        json_object* username_obj;
        if (json_object_object_get_ex(auth_obj, "username", &username_obj)) {
            const char* username = json_object_get_string(username_obj);
            if (username) {
                strncpy(http_config->auth.username, username, sizeof(http_config->auth.username) - 1);
                http_config->auth.username[sizeof(http_config->auth.username) - 1] = '\0';
            }
        }

        json_object* password_obj;
        if (json_object_object_get_ex(auth_obj, "password", &password_obj)) {
            const char* password = json_object_get_string(password_obj);
            if (password) {
                strncpy(http_config->auth.password, password, sizeof(http_config->auth.password) - 1);
                http_config->auth.password[sizeof(http_config->auth.password) - 1] = '\0';
            }
        }
    }

    /* Parse performance configuration */
    json_object* performance_obj;
    if (json_object_object_get_ex(json, "performance", &performance_obj)) {
        json_object* buffer_size_obj;
        if (json_object_object_get_ex(performance_obj, "buffer_size", &buffer_size_obj)) {
            http_config->performance.buffer_size = json_object_get_int(buffer_size_obj);
        }

        json_object* snapshot_buffer_obj;
        if (json_object_object_get_ex(performance_obj, "snapshot_buffer_size", &snapshot_buffer_obj)) {
            http_config->performance.snapshot_buffer_size = json_object_get_int(snapshot_buffer_obj);
        }

        json_object* keep_alive_obj;
        if (json_object_object_get_ex(performance_obj, "keep_alive", &keep_alive_obj)) {
            http_config->performance.keep_alive = json_object_get_boolean(keep_alive_obj);
        }

        json_object* keep_alive_timeout_obj;
        if (json_object_object_get_ex(performance_obj, "keep_alive_timeout", &keep_alive_timeout_obj)) {
            http_config->performance.keep_alive_timeout = json_object_get_int(keep_alive_timeout_obj);
        }
    }

    return 0;
}

int http_module_set_defaults(void* config)
{
    if (!config) {
        return -1;
    }

    http_module_config_t* http_config = (http_module_config_t*)config;
    memset(http_config, 0, sizeof(http_module_config_t));

    /* Set default values */
    http_config->enabled = true;
    http_config->port = 8080;
    http_config->max_connections = 10;
    http_config->request_timeout_ms = 30000;  /* 30 seconds */

    /* Enable all endpoints by default */
    http_config->endpoints.snapshots = true;
    http_config->endpoints.mjpeg_streams = true;
    http_config->endpoints.json_api = true;
    http_config->endpoints.metrics = true;
    http_config->endpoints.onvif = true;
    http_config->endpoints.api_overview = true;

    /* Authentication defaults */
    http_config->auth.enabled = false;
    http_config->auth.localhost_bypass = true;
    strcpy(http_config->auth.username, "admin");
    strcpy(http_config->auth.password, "admin");

    /* Security defaults */
    strcpy(http_config->security.allowed_origins, "*");

    /* Performance defaults */
    http_config->performance.buffer_size = 4096;
    http_config->performance.snapshot_buffer_size = 1024 * 1024;  /* 1MB */
    http_config->performance.keep_alive = false;
    http_config->performance.keep_alive_timeout = 30;

    IMP_LOG_DBG(TAG, "HTTP module defaults set - enabled: %s, port: %d",
              http_config->enabled ? "true" : "false", http_config->port);

    return 0;
}

/* Module registration */
module_info_t http_module_info = {
    .name = HTTP_MODULE_NAME,
    .version = HTTP_MODULE_VERSION,
    .description = "HTTP API server with endpoints for monitoring, media access, and ONVIF",
    .state = MODULE_STATE_UNREGISTERED,
    .module_data = &g_http_module_state,

    /* Lifecycle callbacks */
    .init = http_module_init,
    .start = http_module_start,
    .stop = http_module_stop,
    .cleanup = http_module_cleanup,

    /* Configuration */
    .config_size = sizeof(http_module_config_t),
    .config_parse = http_module_config_parse,

    /* RTSP integration - not needed for HTTP module */
    .rtsp_setup = NULL,
    .rtsp_frame_callback = NULL,
    .rtsp_cleanup = NULL,

    /* Statistics - not implemented */
    .get_stats = NULL
};

/* Module registration function */
int register_http_module(void)
{
    return module_register(&http_module_info);
}

/* HTTP server thread implementation */
static void* http_module_server_thread(void* arg)
{
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_len = sizeof(client_addr);
    int client_socket;

    IMP_LOG_INFO(TAG, "HTTP server thread started on port %d", g_http_module_state.config.port);

    /* Create socket */
    g_http_module_state.server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (g_http_module_state.server_socket < 0) {
        IMP_LOG_ERR(TAG, "Failed to create HTTP server socket");
        return NULL;
    }

    /* Update global variable for compatibility */
    http_server_socket = g_http_module_state.server_socket;

    /* Set socket options */
    int opt = 1;
    setsockopt(g_http_module_state.server_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    /* Bind socket */
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(g_http_module_state.config.port);

    if (bind(g_http_module_state.server_socket, (struct sockaddr*) &server_addr, sizeof(server_addr)) < 0) {
        IMP_LOG_ERR(TAG, "Failed to bind HTTP server socket to port %d", g_http_module_state.config.port);
        close(g_http_module_state.server_socket);
        return NULL;
    }

    /* Listen for connections */
    if (listen(g_http_module_state.server_socket, g_http_module_state.config.max_connections) < 0) {
        IMP_LOG_ERR(TAG, "Failed to listen on HTTP server socket");
        close(g_http_module_state.server_socket);
        return NULL;
    }

    IMP_LOG_INFO(TAG, "HTTP server listening on http://0.0.0.0:%d", g_http_module_state.config.port);

    /* Accept connections */
    while (!g_http_module_state.thread_should_exit) {
        client_socket = accept(g_http_module_state.server_socket, (struct sockaddr*) &client_addr, &client_len);
        if (client_socket < 0) {
            if (!g_http_module_state.thread_should_exit) {
                IMP_LOG_ERR(TAG, "Failed to accept client connection");
            }
            continue;
        }

        /* Optimize client socket for performance */
        int opt = 1;
        /* Disable Nagle's algorithm for faster small packet transmission */
        setsockopt(client_socket, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
        /* Set larger send buffer for better throughput */
        int send_buffer = 64 * 1024; /* 64KB send buffer */
        setsockopt(client_socket, SOL_SOCKET, SO_SNDBUF, &send_buffer, sizeof(send_buffer));

        /* Read HTTP request - handle large ONVIF SOAP requests properly */
        char request[8192]; /* Larger buffer for ONVIF SOAP requests */
        int total_bytes_read = 0;
        int bytes_read;

        /* Read initial chunk */
        bytes_read = recv(client_socket, request, sizeof(request) - 1, 0);
        if (bytes_read > 0) {
            total_bytes_read = bytes_read;
            request[total_bytes_read] = '\0';

            /* Check if we have Content-Length header and need to read more */
            char* content_length_header = strstr(request, "Content-Length: ");
            if (content_length_header) {
                int content_length = atoi(content_length_header + 16);
                char* headers_end = strstr(request, "\r\n\r\n");
                if (headers_end) {
                    int headers_len = (headers_end - request) + 4;
                    int body_received = total_bytes_read - headers_len;

                    /* Read remaining body if needed */
                    while (body_received < content_length && total_bytes_read < sizeof(request) - 1) {
                        bytes_read = recv(client_socket, request + total_bytes_read,
                                        sizeof(request) - 1 - total_bytes_read, 0);
                        if (bytes_read <= 0) break;
                        total_bytes_read += bytes_read;
                        body_received += bytes_read;
                        request[total_bytes_read] = '\0';
                    }
                }
            }
        }

        if (total_bytes_read > 0) {

            /* Debug: Log the request */
            IMP_LOG_DBG(TAG, "HTTP Request: %s", request);

            /* Get client information for authentication */
            client_info_t client_info;
            if (auth_get_client_info(client_socket, &client_info) < 0) {
                IMP_LOG_ERR(TAG, "Failed to get client information");
                close(client_socket);
                continue;
            }

            /* Check if this is an ONVIF request - let ONVIF module handle its own authentication */
            bool is_onvif_request = (strstr(request, "/onvif/") != NULL);

            auth_result_t auth_result = AUTH_RESULT_SUCCESS; /* Default to success for ONVIF */

            /* Only apply HTTP module authentication to non-ONVIF requests */
            if (!is_onvif_request) {
                auth_result = auth_check_http_request(request, &g_http_module_state.config.auth, &client_info);
            } else {
                IMP_LOG_DBG(TAG, "ONVIF request detected - bypassing HTTP authentication");
            }

            if (auth_result == AUTH_RESULT_REQUIRED) {
                /* Send 401 Unauthorized with WWW-Authenticate header */
                char auth_header[256];
                auth_generate_www_authenticate_header("Thingino Streamer", auth_header);

                const char* html_content = "<!DOCTYPE html>"
                                           "<html><head><title>401 Unauthorized</title></head><body>"
                                           "<h1>401 - Unauthorized</h1>"
                                           "<p>Authentication required.</p>"
                                           "</body></html>";

                char response[1024];
                snprintf(response, sizeof(response),
                    "HTTP/1.1 401 Unauthorized\r\n"
                    "%s\r\n"
                    "Content-Type: text/html\r\n"
                    "Content-Length: %zu\r\n"
                    "Connection: close\r\n"
                    "\r\n"
                    "%s",
                    auth_header, strlen(html_content), html_content);

                send(client_socket, response, strlen(response), 0);
                IMP_LOG_INFO(TAG, "401 - Authentication required for %s", client_info.ip_string);
            } else if (auth_result == AUTH_RESULT_INVALID) {
                /* Send 403 Forbidden */
                const char* html_content = "<!DOCTYPE html>"
                                           "<html><head><title>403 Forbidden</title></head><body>"
                                           "<h1>403 - Forbidden</h1>"
                                           "<p>Invalid credentials.</p>"
                                           "</body></html>";

                http_send_response(client_socket, HTTP_STATUS_FORBIDDEN, "text/html", html_content);
                IMP_LOG_WARN(TAG, "403 - Invalid credentials from %s", client_info.ip_string);
            } else if (auth_result == AUTH_RESULT_SUCCESS) {
                /* Authentication successful or not required, route the request */
                if (auth_is_required(&g_http_module_state.config.auth, &client_info)) {
                    IMP_LOG_INFO(TAG, "Authenticated request from %s", client_info.ip_string);
                } else {
                    IMP_LOG_DBG(TAG, "Localhost bypass for %s", client_info.ip_string);
                }

                /* Route request using dynamic router */
                if (http_router_dispatch(request, client_socket) == 0) {
                    /* Route handled by dynamic router */
                } else {
                    /* Send 404 for unknown paths */
                    IMP_LOG_WARN(TAG, "404 - Unknown path in request: %s", request);
                    const char* html_content = "<!DOCTYPE html>"
                                               "<html><head><title>404 Not Found</title></head><body>"
                                               "<h1>404 - Not Found</h1>"
                                               "<p>The requested path was not found.</p>"
                                               "<p><a href=\"/\">View API documentation</a></p>"
                                               "</body></html>";

                    http_send_response(client_socket, HTTP_STATUS_NOT_FOUND, "text/html", html_content);
                }
            } else {
                /* Authentication system error */
                IMP_LOG_ERR(TAG, "Authentication system error for %s", client_info.ip_string);
                http_send_error(client_socket, HTTP_STATUS_INTERNAL_SERVER_ERROR, "Authentication system error");
            }
        } else {
            close(client_socket);
        }
    }

    close(g_http_module_state.server_socket);
    IMP_LOG_INFO(TAG, "HTTP server thread exiting");
    return NULL;
}

/* Safe buffer append with overflow protection */
static size_t safe_append_html(char* buffer, size_t buffer_size, size_t current_len, const char* format, ...)
{
    if (current_len >= buffer_size - 1) {
        return current_len; /* Buffer full, no more space */
    }

    va_list args;
    va_start(args, format);
    int written = vsnprintf(buffer + current_len, buffer_size - current_len, format, args);
    va_end(args);

    if (written < 0) {
        return current_len; /* Error occurred */
    }

    size_t new_len = current_len + (size_t)written;
    return (new_len >= buffer_size) ? buffer_size - 1 : new_len;
}

/* Handle API overview request */
static void handle_api_overview_request(int client_socket)
{
    char html_buffer[8192]; /* Increased buffer size */
    size_t len = 0;

    /* Build HTML content dynamically with safe buffer management */
    len = safe_append_html(html_buffer, sizeof(html_buffer), len,
        "<!DOCTYPE html>"
        "<html><head><title>Thingino Streamer API</title></head><body>"
        "<h1>Thingino Streamer HTTP API</h1>"
        "<h2>Available Endpoints</h2>"
        "<ul>");

    /* Add dynamic routes from router */
    size_t route_count = http_router_get_route_count();
    for (size_t i = 0; i < route_count; i++) {
        const http_route_t* route = http_router_get_route(i);
        if (route) {
            size_t old_len = len;
            char route_html[256];
            const char* method_str = http_method_to_string(route->method);

            if (route->method == HTTP_METHOD_GET) {
                /* GET endpoints can be linked */
                snprintf(route_html, sizeof(route_html),
                    "<li><a href=\"%s\">%s %s</a> - %s (%s)</li>",
                    route->path, method_str, route->path,
                    route->description ? route->description : "No description",
                    route->module_name);
            } else {
                /* Non-GET endpoints just show info */
                snprintf(route_html, sizeof(route_html),
                    "<li>%s %s - %s (%s)</li>",
                    method_str, route->path,
                    route->description ? route->description : "No description",
                    route->module_name);
            }

            len = safe_append_html(html_buffer, sizeof(html_buffer), len, route_html);

            /* If buffer is full, break out of loop */
            if (len == old_len) {
                break;
            }
        }
    }

    len = safe_append_html(html_buffer, sizeof(html_buffer), len, "</ul></body></html>");

    /* Ensure null termination */
    html_buffer[len] = '\0';

    http_send_response(client_socket, HTTP_STATUS_OK, "text/html", html_buffer);
}

/* Compatibility functions for existing code */
int http_server_init(void)
{
    /* This is now handled by the module system, but we provide a stub for compatibility */
    IMP_LOG_INFO(TAG, "HTTP server init called - handled by module system");
    return 0;
}

void http_server_cleanup(void)
{
    /* This is now handled by the module system, but we provide a stub for compatibility */
    IMP_LOG_INFO(TAG, "HTTP server cleanup called - handled by module system");
}

/* HTTP response utilities - available to all modules */

/**
 * Get HTTP status text for a given status code
 */
const char* http_get_status_text(int status_code)
{
    switch (status_code) {
        case HTTP_STATUS_OK:                    return "OK";
        case HTTP_STATUS_CREATED:               return "Created";
        case HTTP_STATUS_NO_CONTENT:            return "No Content";
        case HTTP_STATUS_BAD_REQUEST:           return "Bad Request";
        case HTTP_STATUS_UNAUTHORIZED:          return "Unauthorized";
        case HTTP_STATUS_FORBIDDEN:             return "Forbidden";
        case HTTP_STATUS_NOT_FOUND:             return "Not Found";
        case HTTP_STATUS_METHOD_NOT_ALLOWED:    return "Method Not Allowed";
        case HTTP_STATUS_INTERNAL_SERVER_ERROR: return "Internal Server Error";
        case HTTP_STATUS_NOT_IMPLEMENTED:       return "Not Implemented";
        case HTTP_STATUS_SERVICE_UNAVAILABLE:   return "Service Unavailable";
        default:                                return "Unknown";
    }
}

/**
 * Send a complete HTTP response with status, content type, and body
 */
void http_send_response(int client_socket, int status_code, const char* content_type, const char* body)
{
    const char* status_text = http_get_status_text(status_code);
    int body_len = body ? strlen(body) : 0;

    /* Calculate header size */
    char header[512];
    int header_len = snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n"
        "Cache-Control: no-cache\r\n"
        "\r\n",
        status_code, status_text,
        content_type ? content_type : "text/plain",
        body_len);

    /* Send header first */
    if (send(client_socket, header, header_len, 0) < 0) {
        return;
    }

    /* Send body if present */
    if (body && body_len > 0) {
        send(client_socket, body, body_len, 0);
    }
}

/**
 * Send an HTTP error response with a simple message
 */
void http_send_error(int client_socket, int status_code, const char* message)
{
    char error_body[512];
    const char* default_message = http_get_status_text(status_code);

    snprintf(error_body, sizeof(error_body), "%s", message ? message : default_message);
    http_send_response(client_socket, status_code, "text/plain", error_body);
}

/**
 * Send a JSON response (always 200 OK)
 */
void http_send_json(int client_socket, const char* json_body)
{
    http_send_response(client_socket, HTTP_STATUS_OK, "application/json", json_body);
}

/**
 * Send binary data with specified content type
 */
void http_send_binary(int client_socket, const char* content_type, const void* data, size_t data_size)
{
    IMP_LOG_DBG(TAG, "Sending binary data of size %zu", data_size);
    char header[512];

    int header_len = snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "Cache-Control: no-cache, no-store, must-revalidate\r\n"
        "Pragma: no-cache\r\n"
        "Expires: 0\r\n"
        "Accept-Ranges: bytes\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "X-Content-Type-Options: nosniff\r\n"
        "Transfer-Encoding: identity\r\n"
        "\r\n",
        HTTP_STATUS_OK, http_get_status_text(HTTP_STATUS_OK),
        content_type ? content_type : "application/octet-stream",
        data_size);

    IMP_LOG_DBG(TAG, "Sending header of size %d", header_len);
    /* Send header with fast partial send handling */
    const char* hptr = header;
    size_t hremaining = header_len;
    while (hremaining > 0) {
        ssize_t sent = send(client_socket, hptr, hremaining, MSG_NOSIGNAL);
        if (sent < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue; /* No delay - retry immediately */
            IMP_LOG_ERR(TAG, "Failed to send header");
            return;
        }
        if (sent == 0) break;
        hptr += sent;
        hremaining -= sent;
    }

    IMP_LOG_DBG(TAG, "Sending %zu bytes of binary data", data_size);
    /* Send binary data with fast partial send handling */
    if (data && data_size > 0) {
        const char* dptr = (const char*)data;
        size_t dremaining = data_size;
        while (dremaining > 0) {
            ssize_t sent = send(client_socket, dptr, dremaining, MSG_NOSIGNAL);
            if (sent < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) continue; /* No delay - retry immediately */
                IMP_LOG_ERR(TAG, "Failed to send binary data");
                return;
            }
            if (sent == 0) break;
            dptr += sent;
            dremaining -= sent;
        }
        IMP_LOG_DBG(TAG, "Successfully sent all %zu bytes", data_size);
    }
}

/**
 * Send MJPEG stream header (multipart response initialization)
 * Returns 0 on success, -1 on failure
 * Note: Does NOT close the socket - streaming connection stays open
 */
int http_send_mjpeg_stream_header(int client_socket)
{
    char header[512];

    int header_len = snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: multipart/x-mixed-replace; boundary=--thingino_streamer_mjpeg_boundary\r\n"
        "Cache-Control: no-cache, no-store, must-revalidate\r\n"
        "Pragma: no-cache\r\n"
        "Expires: 0\r\n"
        "Connection: close\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "\r\n",
        HTTP_STATUS_OK, http_get_status_text(HTTP_STATUS_OK));

    return send(client_socket, header, header_len, 0) >= 0 ? 0 : -1;
}

/**
 * Send MJPEG frame boundary and header
 * Returns 0 on success, -1 on failure
 * Note: Does NOT close the socket - streaming connection stays open
 */
int http_send_mjpeg_frame_header(int client_socket, size_t jpeg_size)
{
    char header[256];

    int header_len = snprintf(header, sizeof(header),
        "--thingino_streamer_mjpeg_boundary\r\n"
        "Content-Type: image/jpeg\r\n"
        "Content-Length: %zu\r\n"
        "\r\n",
        jpeg_size);

    return send(client_socket, header, header_len, 0) >= 0 ? 0 : -1;
}

/* Core route handler implementations */
static void handle_api_overview_wrapper(int client_socket, const char* request) {
    handle_api_overview_request(client_socket);
}

static void handle_status_json(int client_socket, const char* request) {
    handle_json_request(client_socket, "status");
}

static void handle_config_json(int client_socket, const char* request) {
    handle_json_request(client_socket, "config");
}

static void handle_info_json(int client_socket, const char* request) {
    handle_json_request(client_socket, "info");
}

static void handle_health_json(int client_socket, const char* request) {
    handle_json_request(client_socket, "health");
}

static void handle_snap0_jpg(int client_socket, const char* request) {
    handle_snapshot_request(client_socket, 0);
}

static void handle_snap1_jpg(int client_socket, const char* request) {
    handle_snapshot_request(client_socket, 1);
}

static void handle_snap2_jpg(int client_socket, const char* request) {
    handle_snapshot_request(client_socket, 2);
}

static void handle_snap3_jpg(int client_socket, const char* request) {
    handle_snapshot_request(client_socket, 3);
}

static void handle_test_jpg(int client_socket, const char* request) {
    uint64_t start_time = get_monotonic_time_us();

    IMP_LOG_INFO(TAG, "Serving static test file /tmp/snap0.jpg");

    /* Open static test file */
    FILE* file = fopen("/tmp/snap0.jpg", "rb");
    if (!file) {
        IMP_LOG_ERR(TAG, "Failed to open /tmp/snap0.jpg");
        http_send_error(client_socket, HTTP_STATUS_NOT_FOUND, "Test file not found");
        return;
    }

    /* Get file size */
    fseek(file, 0, SEEK_END);
    long file_size = ftell(file);
    fseek(file, 0, SEEK_SET);

    if (file_size <= 0) {
        IMP_LOG_ERR(TAG, "Invalid file size for /tmp/snap0.jpg: %ld", file_size);
        fclose(file);
        http_send_error(client_socket, HTTP_STATUS_INTERNAL_SERVER_ERROR, "Invalid test file");
        return;
    }

    /* Allocate buffer and read file */
    unsigned char* buffer = malloc(file_size);
    if (!buffer) {
        IMP_LOG_ERR(TAG, "Failed to allocate %ld bytes for test file", file_size);
        fclose(file);
        http_send_error(client_socket, HTTP_STATUS_INTERNAL_SERVER_ERROR, "Memory allocation error");
        return;
    }

    size_t bytes_read = fread(buffer, 1, file_size, file);
    fclose(file);

    if (bytes_read != (size_t)file_size) {
        IMP_LOG_ERR(TAG, "Failed to read test file: got %zu bytes, expected %ld", bytes_read, file_size);
        free(buffer);
        http_send_error(client_socket, HTTP_STATUS_INTERNAL_SERVER_ERROR, "File read error");
        return;
    }

    uint64_t read_time = get_monotonic_time_us();
    long read_ms = (read_time - start_time) / 1000;
    IMP_LOG_INFO(TAG, "Read test file in %ld ms (%zu bytes)", read_ms, bytes_read);

    /* Send file using HTTP utility */
    http_send_binary(client_socket, "image/jpeg", buffer, bytes_read);

    uint64_t send_time = get_monotonic_time_us();
    long send_ms = (send_time - read_time) / 1000;
    long total_ms = (send_time - start_time) / 1000;

    IMP_LOG_INFO(TAG, "Sent test file in %ld ms, total %ld ms", send_ms, total_ms);

    free(buffer);
}

static void handle_stream0_mjpeg(int client_socket, const char* request) {
    handle_mjpeg_stream(client_socket, 0);
}

static void handle_stream1_mjpeg(int client_socket, const char* request) {
    handle_mjpeg_stream(client_socket, 1);
}

static void handle_stream2_mjpeg(int client_socket, const char* request) {
    handle_mjpeg_stream(client_socket, 2);
}

static void handle_stream3_mjpeg(int client_socket, const char* request) {
    handle_mjpeg_stream(client_socket, 3);
}

static void handle_stream0_h264(int client_socket, const char* request) {
    handle_mp4_request(client_socket, 0);
}

static void handle_stream1_h264(int client_socket, const char* request) {
    handle_mp4_request(client_socket, 1);
}

static void handle_stream2_h264(int client_socket, const char* request) {
    handle_mp4_request(client_socket, 2);
}

static void handle_stream3_h264(int client_socket, const char* request) {
    handle_mp4_request(client_socket, 3);
}

/* Register core HTTP routes */
static int http_register_core_routes(void)
{
    /* Define core HTTP routes */
    static const http_route_t core_routes[] = {
        /* API Overview */
        {"/", HTTP_METHOD_GET, handle_api_overview_wrapper, "http_core", "API overview page"},

        /* JSON API endpoints */
        {"/status.json", HTTP_METHOD_GET, handle_status_json, "http_core", "System status (JSON)"},
        {"/config.json", HTTP_METHOD_GET, handle_config_json, "http_core", "Configuration (JSON)"},
        {"/info.json", HTTP_METHOD_GET, handle_info_json, "http_core", "Hardware info (JSON)"},
        {"/health.json", HTTP_METHOD_GET, handle_health_json, "http_core", "Health check (JSON)"},

        /* Media endpoints */
        {"/snap0.jpg", HTTP_METHOD_GET, handle_snap0_jpg, "http_core", "Channel 0 snapshot"},
        {"/snap1.jpg", HTTP_METHOD_GET, handle_snap1_jpg, "http_core", "Channel 1 snapshot"},
        {"/snap2.jpg", HTTP_METHOD_GET, handle_snap2_jpg, "http_core", "Channel 2 snapshot"},
        {"/snap3.jpg", HTTP_METHOD_GET, handle_snap3_jpg, "http_core", "Channel 3 snapshot"},
        {"/test.jpg", HTTP_METHOD_GET, handle_test_jpg, "http_core", "Static test file"},

        /* MJPEG streams */
        {"/stream0.mjpeg", HTTP_METHOD_GET, handle_stream0_mjpeg, "http_core", "Channel 0 MJPEG stream"},
        {"/stream1.mjpeg", HTTP_METHOD_GET, handle_stream1_mjpeg, "http_core", "Channel 1 MJPEG stream"},
        {"/stream2.mjpeg", HTTP_METHOD_GET, handle_stream2_mjpeg, "http_core", "Channel 2 MJPEG stream"},
        {"/stream3.mjpeg", HTTP_METHOD_GET, handle_stream3_mjpeg, "http_core", "Channel 3 MJPEG stream"},

        /* H.264/MP4 endpoints (currently disabled) */
        {"/stream0.h264", HTTP_METHOD_GET, handle_stream0_h264, "http_core", "Channel 0 H.264 stream"},
        {"/stream1.h264", HTTP_METHOD_GET, handle_stream1_h264, "http_core", "Channel 1 H.264 stream"},
        {"/stream2.h264", HTTP_METHOD_GET, handle_stream2_h264, "http_core", "Channel 2 H.264 stream"},
        {"/stream3.h264", HTTP_METHOD_GET, handle_stream3_h264, "http_core", "Channel 3 H.264 stream"},
        {"/stream0.mp4", HTTP_METHOD_GET, handle_stream0_h264, "http_core", "Channel 0 MP4 stream"},
        {"/stream1.mp4", HTTP_METHOD_GET, handle_stream1_h264, "http_core", "Channel 1 MP4 stream"},
        {"/stream2.mp4", HTTP_METHOD_GET, handle_stream2_h264, "http_core", "Channel 2 MP4 stream"},
        {"/stream3.mp4", HTTP_METHOD_GET, handle_stream3_h264, "http_core", "Channel 3 MP4 stream"},
    };

    /* Register all core routes */
    int ret = http_router_register_routes(core_routes,
                                         sizeof(core_routes) / sizeof(core_routes[0]));
    if (ret < 0) {
        IMP_LOG_ERR(TAG, "Failed to register core HTTP routes");
        return ret;
    }

    IMP_LOG_INFO(TAG, "Registered %zu core HTTP routes",
                 sizeof(core_routes) / sizeof(core_routes[0]));
    return 0;
}

/* ========== ENDPOINT HANDLERS ========== */

/* Capture JPEG snapshot from specified channel (HAL, all platforms) */
static int capture_snapshot(int channel, unsigned char** jpeg_data, unsigned int* jpeg_size)
{
    if (channel < 0 || channel >= FS_CHN_NUM || !chn[channel].enable) {
        IMP_LOG_ERR(TAG, "Invalid or disabled channel %d for snapshot", channel);
        return -1;
    }
    IMP_LOG_INFO(TAG, "Capturing snapshot from channel %d", channel);

    int jpeg_channel = FS_CHN_NUM + channel;

    /* Optional: tune JPEG QP if supported */
    const hal_caps_t *caps = hal_caps();
    hal_enc_attr_t attr;
    if (hal_enc_get_attr(jpeg_channel, &attr) == 0) {
        IMP_LOG_INFO(TAG, "JPEG ch%d: payload=%d, %ux%u", jpeg_channel, attr.payload, attr.width, attr.height);
        if (caps && caps->has_jpeg_qp && attr.payload == HAL_PT_JPEG) {
            if (hal_enc_set_jpeg_qp(jpeg_channel, 25) == 0) {
                IMP_LOG_INFO(TAG, "Set JPEG QP to 25 for fast snapshot on channel %d", channel);
            }
        }
    }

    if (hal_enc_start(jpeg_channel) < 0) {
        IMP_LOG_ERR(TAG, "StartRecvPic failed on ch%d", jpeg_channel);
        return -1;
    }

    if (hal_stream_poll(jpeg_channel, 200) < 0) {
        IMP_LOG_ERR(TAG, "Polling JPEG stream timeout for channel %d", channel);
        hal_enc_stop(jpeg_channel);
        return -1;
    }

    hal_stream_t hs = {0};
    if (hal_stream_get(jpeg_channel, &hs, 1) < 0) {
        IMP_LOG_ERR(TAG, "HAL GetStream(%d) failed for snapshot", jpeg_channel);
        hal_enc_stop(jpeg_channel);
        return -1;
    }

    unsigned int total_size = 0;
    int packCount = hal_stream_pack_count(&hs);
    for (int i = 0; i < packCount; i++) total_size += hal_stream_pack_length(&hs, i);
    if (total_size == 0) {
        IMP_LOG_ERR(TAG, "Empty JPEG stream for channel %d", channel);
        hal_stream_release(jpeg_channel, &hs);
        hal_enc_stop(jpeg_channel);
        return -1;
    }

    *jpeg_data = (unsigned char*)malloc(total_size);
    if (!*jpeg_data) {
        IMP_LOG_ERR(TAG, "Failed to allocate %u bytes for JPEG snapshot", total_size);
        hal_stream_release(jpeg_channel, &hs);
        hal_enc_stop(jpeg_channel);
        return -1;
    }

    unsigned int offset = 0;
    for (int i = 0; i < packCount; i++) offset += hal_stream_copy_pack(&hs, i, *jpeg_data + offset);
    *jpeg_size = offset;

    hal_stream_release(jpeg_channel, &hs);
    hal_enc_stop(jpeg_channel);
    IMP_LOG_INFO(TAG, "Snapshot captured for channel %d, %u bytes", channel, *jpeg_size);
    return 0;
}

/* Handle HTTP request for snapshots */
void handle_snapshot_request(int client_socket, int channel)
{
    uint64_t start_time = get_monotonic_time_us();

    IMP_LOG_INFO(TAG, "Handling snapshot request for channel %d", channel);

    char header[512];
    int header_len;
    unsigned char* jpeg_data = NULL;
    unsigned int jpeg_size = 0;

    /* Capture snapshot directly to memory */
    if (capture_snapshot(channel, &jpeg_data, &jpeg_size) < 0) {
        IMP_LOG_ERR(TAG, "Failed to capture snapshot for channel %d", channel);
        http_send_error(client_socket, HTTP_STATUS_INTERNAL_SERVER_ERROR, "Failed to capture snapshot");
        return;
    }

    uint64_t capture_time = get_monotonic_time_us();
    long capture_ms = (capture_time - start_time) / 1000;
    IMP_LOG_INFO(TAG, "Captured snapshot for channel %d in %ld ms", channel, capture_ms);

    /* Send JPEG snapshot using HTTP utility */
    http_send_binary(client_socket, "image/jpeg", jpeg_data, jpeg_size);

    uint64_t send_time = get_monotonic_time_us();
    long send_ms = (send_time - capture_time) / 1000;
    IMP_LOG_INFO(TAG, "Sent JPEG snapshot for channel %d in %ld ms", channel, send_ms);

    /* Cleanup */
    free(jpeg_data);

    uint64_t end_time = get_monotonic_time_us();
    long total_ms = (end_time - start_time) / 1000;
    IMP_LOG_INFO(TAG, "Total snapshot request for channel %d took %ld ms", channel, total_ms);
}

/* Handle MJPEG streaming request - creates a thread for non-blocking operation */
void handle_mjpeg_stream(int client_socket, int channel)
{
    /* Validate channel */
    if (channel < 0 || channel >= FS_CHN_NUM || !chn[channel].enable) {
        http_send_error(client_socket, HTTP_STATUS_NOT_FOUND, "Channel not enabled");
        return;
    }

    /* Allocate thread data */
    mjpeg_thread_data_t* thread_data = malloc(sizeof(mjpeg_thread_data_t));
    if (!thread_data) {
        IMP_LOG_ERR(TAG, "Failed to allocate memory for MJPEG thread data");
        http_send_error(client_socket, HTTP_STATUS_INTERNAL_SERVER_ERROR, "Memory allocation error");
        return;
    }

    thread_data->client_socket = client_socket;
    thread_data->channel = channel;

    /* Create detached thread for MJPEG streaming */
    pthread_t mjpeg_thread;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

    if (pthread_create(&mjpeg_thread, &attr, mjpeg_stream_thread, thread_data) != 0) {
        IMP_LOG_ERR(TAG, "Failed to create MJPEG thread for channel %d", channel);
        http_send_error(client_socket, HTTP_STATUS_INTERNAL_SERVER_ERROR, "Thread creation error");
        free(thread_data);
        pthread_attr_destroy(&attr);
        return;
    }

    pthread_attr_destroy(&attr);
    IMP_LOG_INFO(TAG, "Created MJPEG streaming thread for channel %d", channel);

    /* Note: client_socket will be closed by the thread, don't close it here */
}

/* MJPEG streaming thread function */
static void* mjpeg_stream_thread(void* arg)
{
    mjpeg_thread_data_t* data = (mjpeg_thread_data_t*)arg;
    int client_socket = data->client_socket;
    int channel = data->channel;
    char header[512];
    int header_len;
    unsigned char* jpeg_data = NULL;
    unsigned int jpeg_size = 0;

    IMP_LOG_INFO(TAG, "MJPEG streaming thread started for channel %d", channel);

    /* Set socket options for better streaming */
    int opt = 1;
    setsockopt(client_socket, SOL_SOCKET, SO_KEEPALIVE, &opt, sizeof(opt));
    setsockopt(client_socket, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

    /* Set linger to 0 for immediate close */
    struct linger linger_opt = {1, 0};
    setsockopt(client_socket, SOL_SOCKET, SO_LINGER, &linger_opt, sizeof(linger_opt));

    /* Send MJPEG stream header using HTTP utility */
    if (http_send_mjpeg_stream_header(client_socket) < 0) {
        IMP_LOG_DBG(TAG, "Client disconnected before MJPEG stream started for channel %d", channel);
        goto cleanup;
    }

    IMP_LOG_INFO(TAG, "Started MJPEG stream for channel %d", channel);

    /* Stream MJPEG frames continuously */
    int frame_count = 0;
    int consecutive_errors = 0;

    while (http_server_running && frame_count < 3000 && consecutive_errors < 5) {
        /* Capture fresh JPEG frame */
        if (capture_snapshot(channel, &jpeg_data, &jpeg_size) < 0) {
            consecutive_errors++;
            IMP_LOG_WARN(TAG,
                         "Failed to capture frame for MJPEG stream channel %d (error %d/5)",
                         channel,
                         consecutive_errors);
            usleep(100000); /* Wait 100ms before retry */
            continue;
        }

        consecutive_errors = 0; /* Reset error count on successful capture */

        /* Send MJPEG frame header using HTTP utility */
        if (http_send_mjpeg_frame_header(client_socket, jpeg_size) < 0) {
            IMP_LOG_DBG(TAG,
                        "Client disconnected during MJPEG stream channel %d (frame %d)",
                        channel,
                        frame_count);
            free(jpeg_data);
            break;
        }

        /* Send JPEG data */
        if (send(client_socket, jpeg_data, jpeg_size, 0) < 0) {
            IMP_LOG_DBG(TAG,
                        "Client disconnected during MJPEG stream channel %d (frame %d)",
                        channel,
                        frame_count);
            free(jpeg_data);
            break;
        }

        /* Send frame separator */
        if (send(client_socket, "\r\n", 2, 0) < 0) {
            IMP_LOG_DBG(TAG,
                        "Client disconnected during MJPEG stream channel %d (frame %d)",
                        channel,
                        frame_count);
            free(jpeg_data);
            break;
        }

        free(jpeg_data);
        jpeg_data = NULL;
        frame_count++;

        /* Control frame rate - aim for ~15 FPS for MJPEG */
        usleep(66000); /* 66ms = ~15 FPS */

        /* Log progress every 100 frames */
        if (frame_count % 100 == 0) {
            IMP_LOG_DBG(TAG, "MJPEG stream channel %d: %d frames sent", channel, frame_count);
        }
    }

    if (consecutive_errors >= 5) {
        IMP_LOG_ERR(TAG, "MJPEG stream channel %d ended due to capture errors", channel);
    } else {
        IMP_LOG_INFO(TAG,
                     "MJPEG stream ended for channel %d after %d frames (client disconnect)",
                     channel,
                     frame_count);
    }

cleanup:
    close(client_socket);
    free(data);
    IMP_LOG_INFO(TAG, "MJPEG streaming thread finished for channel %d", channel);
    return NULL;
}

/* Handle HTTP request for H.264/MP4 segments */
void handle_mp4_request(int client_socket, int channel)
{
    /* Temporarily disabled to prevent interference with main video stream */
    const char* message = "H.264 segment capture temporarily disabled to prevent main video stream conflicts";

    http_send_error(client_socket, HTTP_STATUS_SERVICE_UNAVAILABLE, message);

    IMP_LOG_INFO(TAG,
                 "H.264 segment request for channel %d - feature temporarily disabled",
                 channel);
}

/* Handle JSON endpoint requests - simplified version */
void handle_json_request(int client_socket, const char* endpoint)
{
    char response[2048];
    int response_len;
    const char* json_content;

    /* Generate appropriate JSON response */
    static char json_buffer[2048];  /* Increased size for full config JSON */

    if (strcmp(endpoint, "status") == 0) {
        /* Generate dynamic endpoint list for status response */
        char dynamic_endpoints[512] = "";
        http_router_generate_endpoint_list(dynamic_endpoints, sizeof(dynamic_endpoints));

        snprintf(json_buffer, sizeof(json_buffer),
                 "{\"status\":\"ok\",\"message\":\"HTTP module active\",\"timestamp\":%lld,\"endpoints\":[%s]}",
                 (long long)time(NULL), dynamic_endpoints);
        json_content = json_buffer;
    } else if (strcmp(endpoint, "config") == 0) {
        /* Generate real config JSON */
        int len = 0;
        /* Fail if config not available - don't invent data */
        if (!g_config) {
            IMP_LOG_ERR(TAG, "Global configuration not available for config endpoint");
            http_send_json(client_socket, "{\"error\": \"Configuration not available\"}");
            return;
        }
        len += snprintf(json_buffer + len,
                        sizeof(json_buffer) - len,
                        "{\"general\":{\"log_level\":\"%s\"},",
                        g_config->general.loglevel);

        /* Add streams array */
        len += snprintf(json_buffer + len, sizeof(json_buffer) - len, "\"streams\":[");

        if (g_config && g_config->streams && g_config->stream_count > 0) {
            for (int i = 0; i < g_config->stream_count && i < 2; i++) {
                if (i > 0) {
                    len += snprintf(json_buffer + len, sizeof(json_buffer) - len, ",");
                }

                len += snprintf(
                    json_buffer + len,
                    sizeof(json_buffer) - len,
                    "{\"id\":%d,\"enabled\":%s,\"resolution\":\"%dx%d\",\"fps\":%d,\"bitrate_kbps\":%d,\"format\":\"%s\",\"rtsp_endpoint\":\"%s\"}",
                    i,
                    g_config->streams[i].enabled ? "true" : "false",
                    g_config->streams[i].width,
                    g_config->streams[i].height,
                    g_config->sensor.fps,
                    g_config->streams[i].bitrate,
                    g_config->streams[i].format,
                    g_config->streams[i].rtsp_endpoint);
            }
        } else {
            /* Config not available - fail instead of inventing data */
            IMP_LOG_ERR(TAG, "Global configuration not available for streams config");
            http_send_json(client_socket, "{\"error\": \"Configuration not available\"}");
            return;
        }

        len += snprintf(json_buffer + len, sizeof(json_buffer) - len, "],");

        /* Add JPEG configuration - fail if config not available */
        if (!g_config) {
            IMP_LOG_ERR(TAG, "Global configuration not available for JPEG config");
            http_send_json(client_socket, "{\"error\": \"Configuration not available\"}");
            return;
        }
        len += snprintf(
            json_buffer + len,
            sizeof(json_buffer) - len,
            "\"jpeg\":{\"enabled\":%s,\"quality\":%d,\"channel\":%d,\"path\":\"%s\"},",
            g_config->jpeg.enabled ? "true" : "false",
            g_config->jpeg.jpeg_quality,
            g_config->jpeg.jpeg_channel,
            g_config->jpeg.jpeg_path);

        /* Generate dynamic endpoint list */
        char dynamic_endpoints[1024] = "";
        http_router_generate_endpoint_list(dynamic_endpoints, sizeof(dynamic_endpoints));

        len += snprintf(
            json_buffer + len,
            sizeof(json_buffer) - len,
            "\"http_api\":{\"port\":%d,\"endpoints\":[%s]}}",
            g_http_module_state.config.port,
            dynamic_endpoints);

        json_content = json_buffer;
    } else if (strcmp(endpoint, "info") == 0) {
        snprintf(json_buffer, sizeof(json_buffer),
                 "{\"info\":\"thingino-streamer\",\"version\":\"modular\",\"http_module\":\"active\",\"timestamp\":%lld}",
                 (long long)time(NULL));
        json_content = json_buffer;
    } else if (strcmp(endpoint, "health") == 0) {
        /* Generate health JSON with current timestamp and uptime */
        static char health_json[512];

        /* FIXME: Uptime calculation is incorrect - reports system uptime instead of process uptime
         * This causes inflated uptime values (e.g., 69 hours instead of 12 hours for overnight run)
         * Should track process start time and calculate elapsed time from that baseline
         * Current: reads /proc/uptime (system uptime since boot)
         * Should: use process start time or application-specific uptime tracking
         */
        /* Get system uptime */
        FILE* uptime_file = fopen("/proc/uptime", "r");
        double uptime_seconds = 0.0;
        if (uptime_file) {
            fscanf(uptime_file, "%lf", &uptime_seconds);
            fclose(uptime_file);
        }

        snprintf(health_json, sizeof(health_json),
                 "{\"health\":\"ok\",\"http_server\":\"running\",\"timestamp\":%lld,\"uptime_seconds\":%lu}",
                 (long long)time(NULL), (unsigned long)uptime_seconds);
        json_content = health_json;
    } else {
        /* Unknown endpoint */
        http_send_json(client_socket, "{\"error\": \"Unknown endpoint\"}");
        return;
    }

    /* Send JSON response using HTTP utility */
    http_send_json(client_socket, json_content);

    IMP_LOG_DBG(TAG, "Served JSON endpoint: %s", endpoint);
}

/* Check if image_grab module is enabled */
int is_image_grab_module_enabled(void)
{
#ifdef ENABLE_IMAGE_GRAB
    /* Check if image_grab module is running */
    extern module_info_t image_grab_module_info;
    return (image_grab_module_info.state == MODULE_STATE_RUNNING);
#else
    return 0; /* Not compiled in */
#endif
}

/* Fallback handler for image grab endpoints when module is disabled */
static void handle_image_grab_fallback(int client_socket, const char* request)
{
    /* Create a minimal 1920x1080 PNG image (transparent strip) */
    static const unsigned char minimal_1080p_strip_png[] = {
        0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d,
        0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x07, 0x80, 0x00, 0x00, 0x04, 0x38,
        0x01, 0x00, 0x00, 0x00, 0x00, 0xc0, 0xa8, 0xfc, 0xee, 0x00, 0x00, 0x01,
        0x13, 0x49, 0x44, 0x41, 0x54, 0x78, 0xda, 0xed, 0xc1, 0x01, 0x0d, 0x00,
        0x00, 0x00, 0xc2, 0xa0, 0xf7, 0x4f, 0x6d, 0x0f, 0x07, 0x14, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xc0, 0xa7, 0x01,
        0xf8, 0xe5, 0x00, 0x01, 0xfc, 0xe1, 0x8f, 0xfe, 0x00, 0x00, 0x00, 0x00,
        0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82
    };
    static const size_t minimal_1080p_strip_png_len = 332;

    const char* response_headers =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: image/png\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "Cache-Control: no-cache\r\n"
        "X-Image-Source: fallback\r\n"
        "X-Image-Dimensions: 1920x1080\r\n"
        "\r\n";

    /* Send headers */
    char header_buffer[512];
    int header_len = snprintf(header_buffer, sizeof(header_buffer),
                             response_headers, minimal_1080p_strip_png_len);

    if (send(client_socket, header_buffer, header_len, 0) < 0) {
        IMP_LOG_ERR(TAG, "Failed to send fallback image headers");
        return;
    }

    /* Send minimal PNG data */
    if (send(client_socket, minimal_1080p_strip_png, minimal_1080p_strip_png_len, 0) < 0) {
        IMP_LOG_ERR(TAG, "Failed to send fallback image data");
        return;
    }

    IMP_LOG_DBG(TAG, "Sent fallback 1080p PNG image (%zu bytes)", minimal_1080p_strip_png_len);
}

/* Register fallback routes for image grab endpoints */
static int http_register_image_grab_fallback_routes(void)
{
    /* Define fallback routes for image grab endpoints */
    static const http_route_t fallback_routes[] = {
        {"/image0.jpg", HTTP_METHOD_GET, handle_image_grab_fallback, "http_fallback", "Channel 0 fallback (PNG as JPEG)"},
        {"/image1.jpg", HTTP_METHOD_GET, handle_image_grab_fallback, "http_fallback", "Channel 1 fallback (PNG as JPEG)"},
        {"/image0.nv12", HTTP_METHOD_GET, handle_image_grab_fallback, "http_fallback", "Channel 0 NV12 fallback (PNG)"},
        {"/image1.nv12", HTTP_METHOD_GET, handle_image_grab_fallback, "http_fallback", "Channel 1 NV12 fallback (PNG)"},
        {"/image0.yuyv422", HTTP_METHOD_GET, handle_image_grab_fallback, "http_fallback", "Channel 0 YUYV422 fallback (PNG)"},
        {"/image1.yuyv422", HTTP_METHOD_GET, handle_image_grab_fallback, "http_fallback", "Channel 1 YUYV422 fallback (PNG)"},
    };

    /* Register all fallback routes */
    int ret = http_router_register_routes(fallback_routes,
                                         sizeof(fallback_routes) / sizeof(fallback_routes[0]));
    if (ret < 0) {
        IMP_LOG_ERR(TAG, "Failed to register image grab fallback routes");
        return ret;
    }

    IMP_LOG_INFO(TAG, "Registered %zu image grab fallback routes",
                 sizeof(fallback_routes) / sizeof(fallback_routes[0]));
    return 0;
}
