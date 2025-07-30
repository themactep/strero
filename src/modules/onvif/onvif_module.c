/*
 * onvif_module.c - ONVIF Module Implementation
 * Modular ONVIF implementation with WS-Discovery and SOAP services
 * Copyright (C) 2025 Paul Philippov, Thingino Project
 */

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "../../config.h"
#include "../../common.h"
#include "onvif_module.h"

#ifdef ENABLE_HTTP
#include "../http/http_router.h"
#endif

#define TAG "ONVIF_MODULE"

/* Module state */
static struct {
    onvif_module_config_t config;
    bool initialized;
    bool running;
    pthread_t discovery_thread;
    volatile int discovery_running;
} g_onvif_state = {0};

/* Forward declarations */
static int setup_onvif_discovery(void);
static int setup_onvif_services(void);

/* Module registration - manual registration due to symbol stripping */
module_info_t onvif_module_info = {
    .name = "onvif",
    .version = "1.0.0",
    .description = "ONVIF protocol support with WS-Discovery and SOAP services",
    .config_size = sizeof(onvif_module_config_t),
    .init = onvif_module_init,
    .start = onvif_module_start,
    .stop = onvif_module_stop,
    .cleanup = onvif_module_cleanup,
    .config_parse = onvif_module_config_parse,
    .config_validate = NULL,
    .config_free = NULL,
    .rtsp_setup = NULL,
    .rtsp_frame_callback = NULL,
    .rtsp_cleanup = NULL,
    .get_stats = NULL
};

int onvif_module_init(void* config)
{
    if (g_onvif_state.initialized) {
        IMP_LOG_WARN(TAG, "ONVIF module already initialized");
        return 0;
    }

    IMP_LOG_INFO(TAG, "Initializing ONVIF module");

    /* Copy configuration */
    if (config) {
        memcpy(&g_onvif_state.config, config, sizeof(onvif_module_config_t));
    } else {
        /* Set defaults if no config provided */
        g_onvif_state.config.enabled = true;

        /* Authentication defaults */
        g_onvif_state.config.auth.enabled = false;
        g_onvif_state.config.auth.localhost_bypass = true;
        strcpy(g_onvif_state.config.auth.username, "admin");
        strcpy(g_onvif_state.config.auth.password, "admin");

        g_onvif_state.config.discovery_enabled = true;
        g_onvif_state.config.discovery_port = 3702;
        g_onvif_state.config.device_service_enabled = true;
        g_onvif_state.config.media_service_enabled = true;
        g_onvif_state.config.event_service_enabled = false;
        strcpy(g_onvif_state.config.device_name, "Thingino Camera");
        strcpy(g_onvif_state.config.device_location, "Embedded");
        strcpy(g_onvif_state.config.manufacturer, "Thingino");
        strcpy(g_onvif_state.config.model, "Streamer");
        strcpy(g_onvif_state.config.serial_number, "123456789");
        strcpy(g_onvif_state.config.firmware_version, "1.0.0");
        strcpy(g_onvif_state.config.hardware_id, "thingino-hw1");
    }

    /* Check if ONVIF is enabled */
    if (!g_onvif_state.config.enabled) {
        IMP_LOG_INFO(TAG, "ONVIF module disabled in configuration");
        g_onvif_state.initialized = true;
        return 0;
    }

    IMP_LOG_INFO(TAG, "ONVIF module initialized with config: enabled=%s, device=%s",
                 g_onvif_state.config.enabled ? "true" : "false",
                 g_onvif_state.config.device_name);

    /* Setup ONVIF services */
    if (setup_onvif_services() != 0) {
        IMP_LOG_ERR(TAG, "Failed to setup ONVIF services");
        return -1;
    }

    g_onvif_state.initialized = true;
    IMP_LOG_INFO(TAG, "ONVIF module initialized successfully (device=%s)",
                 g_onvif_state.config.device_name);

    return 0;
}

int onvif_module_start(void)
{
    if (!g_onvif_state.initialized) {
        IMP_LOG_ERR(TAG, "ONVIF module not initialized");
        return -1;
    }

    if (g_onvif_state.running) {
        IMP_LOG_WARN(TAG, "ONVIF module already running");
        return 0;
    }

    if (!g_onvif_state.config.enabled) {
        IMP_LOG_INFO(TAG, "ONVIF module disabled, not starting");
        return 0;
    }

    IMP_LOG_INFO(TAG, "Starting ONVIF module");

    /* Start WS-Discovery service if enabled */
    if (g_onvif_state.config.discovery_enabled) {
        if (setup_onvif_discovery() != 0) {
            IMP_LOG_ERR(TAG, "Failed to start ONVIF discovery service");
            return -1;
        }
    }

    g_onvif_state.running = true;
    IMP_LOG_INFO(TAG, "ONVIF module started successfully");

    return 0;
}

int onvif_module_stop(void)
{
    if (!g_onvif_state.running) {
        return 0;
    }

    IMP_LOG_INFO(TAG, "Stopping ONVIF module");

    /* Stop discovery service */
    if (g_onvif_state.discovery_running) {
        g_onvif_state.discovery_running = 0;
        if (g_onvif_state.discovery_thread) {
            pthread_join(g_onvif_state.discovery_thread, NULL);
        }
    }

    g_onvif_state.running = false;
    IMP_LOG_INFO(TAG, "ONVIF module stopped");

    return 0;
}

int onvif_module_cleanup(void)
{
    onvif_module_stop();

    memset(&g_onvif_state, 0, sizeof(g_onvif_state));
    IMP_LOG_INFO(TAG, "ONVIF module cleanup complete");

    return 0;
}

static int setup_onvif_services(void)
{
    IMP_LOG_INFO(TAG, "Setting up ONVIF services");

    if (g_onvif_state.config.device_service_enabled) {
        IMP_LOG_INFO(TAG, "ONVIF Device service enabled");
    }

    if (g_onvif_state.config.media_service_enabled) {
        IMP_LOG_INFO(TAG, "ONVIF Media service enabled");
    }

    if (g_onvif_state.config.event_service_enabled) {
        IMP_LOG_INFO(TAG, "ONVIF Event service enabled");
    }

    IMP_LOG_INFO(TAG, "ONVIF services setup complete");
    return 0;
}

static int setup_onvif_discovery(void)
{
    IMP_LOG_INFO(TAG, "Setting up ONVIF WS-Discovery service");

    /* Start discovery service - implementation will be in onvif_discovery.c */
    if (onvif_start_discovery_service() != 0) {
        IMP_LOG_ERR(TAG, "Failed to start WS-Discovery service");
        return -1;
    }

    IMP_LOG_INFO(TAG, "ONVIF WS-Discovery service started on port %d",
                 g_onvif_state.config.discovery_port);
    return 0;
}

/* HTTP integration is handled directly through onvif_module_handle_request() */

/* Get module configuration for external access */
const onvif_module_config_t* onvif_module_get_config(void)
{
    return &g_onvif_state.config;
}

int onvif_module_config_parse(json_object* json, void* config)
{
    if (!json || !config) {
        return -1;
    }

    onvif_module_config_t* onvif_config = (onvif_module_config_t*)config;

    /* JSON root is the onvif config directly (no wrapper) */
    json_object* onvif_obj = json;

    /* Set defaults first */
    onvif_config->enabled = true;

    /* Authentication defaults */
    onvif_config->auth.enabled = false;
    onvif_config->auth.localhost_bypass = true;
    strcpy(onvif_config->auth.username, "admin");
    strcpy(onvif_config->auth.password, "admin");

    onvif_config->discovery_enabled = true;
    onvif_config->discovery_port = 3702;
    onvif_config->device_service_enabled = true;
    onvif_config->media_service_enabled = true;
    onvif_config->event_service_enabled = false;
    strcpy(onvif_config->device_name, "Thingino Camera");
    strcpy(onvif_config->device_location, "Embedded");
    strcpy(onvif_config->manufacturer, "Thingino");
    strcpy(onvif_config->model, "Streamer");
    strcpy(onvif_config->serial_number, "123456789");
    strcpy(onvif_config->firmware_version, "1.0.0");
    strcpy(onvif_config->hardware_id, "thingino-hw1");

    /* Parse authentication configuration */
    json_object* auth_obj;
    if (json_object_object_get_ex(json, "auth", &auth_obj)) {
        json_object* auth_enabled_obj;
        if (json_object_object_get_ex(auth_obj, "enabled", &auth_enabled_obj)) {
            onvif_config->auth.enabled = json_object_get_boolean(auth_enabled_obj);
        }

        json_object* localhost_bypass_obj;
        if (json_object_object_get_ex(auth_obj, "localhost_bypass", &localhost_bypass_obj)) {
            onvif_config->auth.localhost_bypass = json_object_get_boolean(localhost_bypass_obj);
        }

        json_object* username_obj;
        if (json_object_object_get_ex(auth_obj, "username", &username_obj)) {
            const char* username = json_object_get_string(username_obj);
            if (username) {
                strncpy(onvif_config->auth.username, username, sizeof(onvif_config->auth.username) - 1);
            }
        }

        json_object* password_obj;
        if (json_object_object_get_ex(auth_obj, "password", &password_obj)) {
            const char* password = json_object_get_string(password_obj);
            if (password) {
                strncpy(onvif_config->auth.password, password, sizeof(onvif_config->auth.password) - 1);
            }
        }
    }

    /* Parse ONVIF configuration fields */
    json_object* field = NULL;

    if (json_object_object_get_ex(onvif_obj, "enabled", &field)) {
        onvif_config->enabled = json_object_get_boolean(field);
    }

    if (json_object_object_get_ex(onvif_obj, "device_name", &field)) {
        const char* str = json_object_get_string(field);
        if (str) {
            strncpy(onvif_config->device_name, str, sizeof(onvif_config->device_name) - 1);
            onvif_config->device_name[sizeof(onvif_config->device_name) - 1] = '\0';
        }
    }

    if (json_object_object_get_ex(onvif_obj, "device_location", &field)) {
        const char* str = json_object_get_string(field);
        if (str) {
            strncpy(onvif_config->device_location, str, sizeof(onvif_config->device_location) - 1);
            onvif_config->device_location[sizeof(onvif_config->device_location) - 1] = '\0';
        }
    }

    if (json_object_object_get_ex(onvif_obj, "manufacturer", &field)) {
        const char* str = json_object_get_string(field);
        if (str) {
            strncpy(onvif_config->manufacturer, str, sizeof(onvif_config->manufacturer) - 1);
            onvif_config->manufacturer[sizeof(onvif_config->manufacturer) - 1] = '\0';
        }
    }

    if (json_object_object_get_ex(onvif_obj, "model", &field)) {
        const char* str = json_object_get_string(field);
        if (str) {
            strncpy(onvif_config->model, str, sizeof(onvif_config->model) - 1);
            onvif_config->model[sizeof(onvif_config->model) - 1] = '\0';
        }
    }

    if (json_object_object_get_ex(onvif_obj, "serial_number", &field)) {
        const char* str = json_object_get_string(field);
        if (str) {
            strncpy(onvif_config->serial_number, str, sizeof(onvif_config->serial_number) - 1);
            onvif_config->serial_number[sizeof(onvif_config->serial_number) - 1] = '\0';
        }
    }

    if (json_object_object_get_ex(onvif_obj, "firmware_version", &field)) {
        const char* str = json_object_get_string(field);
        if (str) {
            strncpy(onvif_config->firmware_version, str, sizeof(onvif_config->firmware_version) - 1);
            onvif_config->firmware_version[sizeof(onvif_config->firmware_version) - 1] = '\0';
        }
    }

    if (json_object_object_get_ex(onvif_obj, "hardware_id", &field)) {
        const char* str = json_object_get_string(field);
        if (str) {
            strncpy(onvif_config->hardware_id, str, sizeof(onvif_config->hardware_id) - 1);
            onvif_config->hardware_id[sizeof(onvif_config->hardware_id) - 1] = '\0';
        }
    }

    IMP_LOG_INFO(TAG, "ONVIF config loaded:");
    IMP_LOG_INFO(TAG, "  enabled: %s", onvif_config->enabled ? "true" : "false");
    IMP_LOG_INFO(TAG, "  device_name: %s", onvif_config->device_name);
    IMP_LOG_INFO(TAG, "  device_location: %s", onvif_config->device_location);
    IMP_LOG_INFO(TAG, "  manufacturer: %s", onvif_config->manufacturer);
    IMP_LOG_INFO(TAG, "  model: %s", onvif_config->model);
    IMP_LOG_INFO(TAG, "  serial_number: %s", onvif_config->serial_number);
    IMP_LOG_INFO(TAG, "  firmware_version: %s", onvif_config->firmware_version);
    IMP_LOG_INFO(TAG, "  hardware_id: %s", onvif_config->hardware_id);

    return 0;
}

/* Check if ONVIF module is enabled and running */
bool onvif_module_is_enabled(void)
{
    return g_onvif_state.initialized && g_onvif_state.config.enabled;
}

bool onvif_module_is_running(void)
{
    return g_onvif_state.running;
}

#ifdef ENABLE_HTTP
/* Handler wrapper function */
static void handle_onvif_request_wrapper(int client_socket, const char* request) {
    extern streamer_config_t* g_config;
    onvif_module_handle_request(client_socket, request, g_config);
}

/* Route registration for HTTP module */
int onvif_register_routes(void)
{
    /* Define routes - ONVIF uses pattern matching for multiple endpoints */
    static const http_route_t onvif_routes[] = {
        {"/onvif/device_service", HTTP_METHOD_GET, handle_onvif_request_wrapper, "onvif", "ONVIF Device service"},
        {"/onvif/device_service", HTTP_METHOD_POST, handle_onvif_request_wrapper, "onvif", "ONVIF Device service"},
        {"/onvif/media_service", HTTP_METHOD_GET, handle_onvif_request_wrapper, "onvif", "ONVIF Media service"},
        {"/onvif/media_service", HTTP_METHOD_POST, handle_onvif_request_wrapper, "onvif", "ONVIF Media service"},
        {"/onvif/event_service", HTTP_METHOD_GET, handle_onvif_request_wrapper, "onvif", "ONVIF Event service"},
        {"/onvif/event_service", HTTP_METHOD_POST, handle_onvif_request_wrapper, "onvif", "ONVIF Event service"},
        {"/onvif/imaging_service", HTTP_METHOD_GET, handle_onvif_request_wrapper, "onvif", "ONVIF Imaging service"},
        {"/onvif/imaging_service", HTTP_METHOD_POST, handle_onvif_request_wrapper, "onvif", "ONVIF Imaging service"},
        {"/onvif/ptz_service", HTTP_METHOD_GET, handle_onvif_request_wrapper, "onvif", "ONVIF PTZ service"},
        {"/onvif/ptz_service", HTTP_METHOD_POST, handle_onvif_request_wrapper, "onvif", "ONVIF PTZ service"},
        {"/onvif/snapshot", HTTP_METHOD_GET, handle_onvif_request_wrapper, "onvif", "ONVIF snapshot"},
    };

    /* Register all routes */
    int ret = http_router_register_routes(onvif_routes,
                                         sizeof(onvif_routes) / sizeof(onvif_routes[0]));
    if (ret < 0) {
        IMP_LOG_ERR(TAG, "Failed to register HTTP routes");
        return ret;
    }

    IMP_LOG_INFO(TAG, "Registered %zu HTTP routes",
                 sizeof(onvif_routes) / sizeof(onvif_routes[0]));
    return 0;
}
#endif
