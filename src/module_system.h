/*
 * module_system.h - Modular architecture system
 * Plugin-based modular system for optional features
 * Copyright (C) 2025 Paul Philippov, Thingino Project
 */

#ifndef __MODULE_SYSTEM_H__
#define __MODULE_SYSTEM_H__

#include <stdint.h>
#include <stdbool.h>
#include <json-c/json.h>
#include <sys/time.h>

#define MAX_MODULES 16
#define MAX_MODULE_NAME_LEN 32
#define MODULE_CONFIG_DIR "/etc/streamer.d"
#define MODULE_CONFIG_PATH_MAX 256

/* Forward declarations */
typedef struct rtsp_server rtsp_server_t;

/* Module lifecycle states */
typedef enum {
    MODULE_STATE_UNREGISTERED = 0,
    MODULE_STATE_REGISTERED,
    MODULE_STATE_INITIALIZED,
    MODULE_STATE_RUNNING,
    MODULE_STATE_ERROR
} module_state_t;

/* Module information structure */
typedef struct module_info {
    /* Module identification */
    char name[MAX_MODULE_NAME_LEN];
    const char* version;
    const char* description;

    /* Module state */
    module_state_t state;
    void* module_data;  /* Module-specific data */

    /* Lifecycle callbacks */
    int (*init)(void* config);
    int (*start)(void);
    int (*stop)(void);
    int (*cleanup)(void);

    /* Configuration callbacks */
    int (*config_parse)(json_object* json, void* config);
    int (*config_validate)(void* config);
    void (*config_free)(void* config);
    size_t config_size;

    /* RTSP integration hooks */
    int (*rtsp_setup)(rtsp_server_t* server);
    int (*rtsp_frame_callback)(rtsp_server_t* server, int channel, const uint8_t* frame_data, uint32_t frame_size, const struct timeval* timestamp);
    int (*rtsp_cleanup)(rtsp_server_t* server);

    /* Statistics callback */
    int (*get_stats)(void* stats_buffer, size_t buffer_size);
} module_info_t;

/* Module system functions */

/**
 * Initialize the module system
 * @return 0 on success, -1 on error
 */
int module_system_init(void);

/**
 * Register a module with the system
 * @param module Module information structure
 * @return 0 on success, -1 on error
 */
int module_register(module_info_t* module);

/**
 * Initialize all registered modules
 * @param global_config Global configuration object
 * @return 0 on success, -1 on error
 */
int module_init_all(void* global_config);

/**
 * Start all initialized modules
 * @return 0 on success, -1 on error
 */
int module_start_all(void);

/**
 * Stop all running modules
 * @return 0 on success, -1 on error
 */
int module_stop_all(void);

/**
 * Cleanup all modules and the module system
 * @return 0 on success, -1 on error
 */
int module_cleanup_all(void);

/**
 * Get module by name
 * @param name Module name
 * @return Module info pointer or NULL if not found
 */
module_info_t* module_get_by_name(const char* name);

/**
 * Get list of all registered modules
 * @param modules Array to fill with module pointers
 * @param max_modules Maximum number of modules to return
 * @return Number of modules returned
 */
int module_get_all(module_info_t** modules, int max_modules);

/**
 * Setup RTSP integration for all modules
 * @param server RTSP server instance
 * @return 0 on success, -1 on error
 */
int module_rtsp_setup_all(rtsp_server_t* server);

/**
 * Call RTSP frame callback for all modules
 * @param server RTSP server instance
 * @param channel Video channel
 * @param frame_data Frame data buffer
 * @param frame_size Frame data size
 * @param timestamp Frame timestamp
 * @return 0 on success, -1 on error
 */
int module_rtsp_frame_callback_all(rtsp_server_t* server, int channel, const uint8_t* frame_data, uint32_t frame_size, const struct timeval* timestamp);

/**
 * Cleanup RTSP integration for all modules
 * @param server RTSP server instance
 * @return 0 on success, -1 on error
 */
int module_rtsp_cleanup_all(rtsp_server_t* server);

/**
 * Load module configuration from dedicated config file
 * @param module_name Module name (e.g., "audio", "motion")
 * @param config_buffer Buffer to store parsed config
 * @param config_size Size of config buffer
 * @param parse_func Module's config parsing function
 * @return 0 on success, -1 on error
 */
int module_load_config_file(const char* module_name, void* config_buffer, size_t config_size,
                           int (*parse_func)(json_object* json, void* config));

/* Utility macros for module registration */
#define MODULE_REGISTER(module_var) \
    __attribute__((constructor)) \
    static void register_##module_var(void) { \
        module_register(&module_var); \
    }

#define MODULE_DEFINE(name, version, desc) \
    static module_info_t name##_module = { \
        .name = #name, \
        .version = version, \
        .description = desc, \
        .state = MODULE_STATE_UNREGISTERED \
    }

#endif /* __MODULE_SYSTEM_H__ */
