/*
 * module_system.c - Modular architecture system implementation
 * Plugin-based modular system for optional features
 * Copyright (C) 2025 Paul Philippov, Thingino Project
 */

#include <errno.h>
#include <libgen.h>
#include <limits.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <sys/stat.h>

#include "common.h"
#include "module_system.h"

#define TAG "MODULE"

/* Global module registry */
static module_info_t* g_modules[MAX_MODULES];
static int g_module_count = 0;
static bool g_system_initialized = false;

/* Get directory where the binary is located (for config file testing) */
static char* get_binary_dir(void)
{
    static char exe_path[PATH_MAX];
    static char* exe_dir = NULL;

    if (exe_dir) {
        return strdup(exe_dir); /* Return copy for consistent free() behavior */
    }

    /* Get path to current executable */
    ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
    if (len == -1) {
        IMP_LOG_ERR(TAG, "Failed to get executable path: %s", strerror(errno));
        return NULL;
    }
    exe_path[len] = '\0';

    /* Get directory part */
    exe_dir = dirname(exe_path);

    return strdup(exe_dir);
}

int module_system_init(void)
{
    if (g_system_initialized) {
        IMP_LOG_WARN(TAG, "Module system already initialized");
        return 0;
    }

    IMP_LOG_INFO(TAG, "Initializing module system");

    /* Clear module registry */
    memset(g_modules, 0, sizeof(g_modules));
    g_module_count = 0;
    g_system_initialized = true;

    IMP_LOG_INFO(TAG, "Module system initialized successfully");
    return 0;
}

int module_register(module_info_t* module)
{
    if (!module) {
        IMP_LOG_ERR(TAG, "Invalid module pointer");
        return -1;
    }

    if (!g_system_initialized) {
        /* Auto-initialize if not done yet */
        if (module_system_init() != 0) {
            return -1;
        }
    }

    if (g_module_count >= MAX_MODULES) {
        IMP_LOG_ERR(TAG, "Maximum number of modules (%d) reached", MAX_MODULES);
        return -1;
    }

    /* Check for duplicate names */
    for (int i = 0; i < g_module_count; i++) {
        if (strcmp(g_modules[i]->name, module->name) == 0) {
            IMP_LOG_ERR(TAG, "Module '%s' already registered", module->name);
            return -1;
        }
    }

    /* Register module */
    g_modules[g_module_count] = module;
    module->state = MODULE_STATE_REGISTERED;
    g_module_count++;

    IMP_LOG_INFO(TAG, "Registered module '%s' v%s (%s)",
                 module->name,
                 module->version ? module->version : "unknown",
                 module->description ? module->description : "no description");

    return 0;
}

int module_init_all(void* global_config)
{
    IMP_LOG_INFO(TAG, "Initializing %d registered modules", g_module_count);

    int success_count = 0;
    int error_count = 0;

    for (int i = 0; i < g_module_count; i++) {
        module_info_t* module = g_modules[i];

        if (module->state != MODULE_STATE_REGISTERED) {
            IMP_LOG_WARN(TAG, "Module '%s' not in registered state, skipping", module->name);
            continue;
        }

        if (!module->init) {
            IMP_LOG_WARN(TAG, "Module '%s' has no init function, skipping", module->name);
            continue;
        }

        IMP_LOG_INFO(TAG, "Initializing module '%s'", module->name);

        /* Load module-specific config (try dedicated file first, then fallback to global config) */
        void* module_config = NULL;
        if (module->config_parse && module->config_size > 0) {
            module_config = malloc(module->config_size);
            if (module_config) {
                /* Try to load from dedicated config file first */
                if (module_load_config_file(module->name, module_config, module->config_size, module->config_parse) != 0) {
                    /* Fallback to global config section */
                    if (global_config) {
                        IMP_LOG_INFO(TAG, "Trying fallback config from main config for module '%s'", module->name);
                        memset(module_config, 0, module->config_size);
                        if (module->config_parse(global_config, module_config) != 0) {
                            IMP_LOG_WARN(TAG, "Failed to parse fallback config for module '%s', using defaults", module->name);
                            /* Keep the zeroed config as defaults */
                        }
                    } else {
                        IMP_LOG_INFO(TAG, "No config available for module '%s', using defaults", module->name);
                        /* Keep the zeroed config as defaults */
                    }
                }
            }
        }

        /* Initialize module */
        int ret = module->init(module_config);
        if (ret == 0) {
            module->state = MODULE_STATE_INITIALIZED;
            success_count++;
            IMP_LOG_INFO(TAG, "Module '%s' initialized successfully", module->name);
        } else {
            module->state = MODULE_STATE_ERROR;
            error_count++;
            IMP_LOG_ERR(TAG, "Module '%s' initialization failed: %d", module->name, ret);

            /* Free config on error */
            if (module_config && module->config_free) {
                module->config_free(module_config);
            } else if (module_config) {
                free(module_config);
            }
        }
    }

    IMP_LOG_INFO(TAG, "Module initialization complete: %d success, %d errors",
                 success_count, error_count);

    return (error_count == 0) ? 0 : -1;
}

int module_start_all(void)
{
    IMP_LOG_INFO(TAG, "Starting all initialized modules");

    int success_count = 0;
    int error_count = 0;

    for (int i = 0; i < g_module_count; i++) {
        module_info_t* module = g_modules[i];

        if (module->state != MODULE_STATE_INITIALIZED) {
            continue;
        }

        if (!module->start) {
            /* No start function is OK - module might be passive */
            module->state = MODULE_STATE_RUNNING;
            success_count++;
            continue;
        }

        IMP_LOG_INFO(TAG, "Starting module '%s'", module->name);

        int ret = module->start();
        if (ret == 0) {
            module->state = MODULE_STATE_RUNNING;
            success_count++;
            IMP_LOG_INFO(TAG, "Module '%s' started successfully", module->name);
        } else {
            module->state = MODULE_STATE_ERROR;
            error_count++;
            IMP_LOG_ERR(TAG, "Module '%s' start failed: %d", module->name, ret);
        }
    }

    IMP_LOG_INFO(TAG, "Module start complete: %d success, %d errors",
                 success_count, error_count);

    return (error_count == 0) ? 0 : -1;
}

int module_stop_all(void)
{
    IMP_LOG_INFO(TAG, "Stopping all running modules");

    /* Stop modules in reverse order */
    for (int i = g_module_count - 1; i >= 0; i--) {
        module_info_t* module = g_modules[i];

        if (module->state != MODULE_STATE_RUNNING) {
            continue;
        }

        if (module->stop) {
            IMP_LOG_INFO(TAG, "Stopping module '%s'", module->name);
            module->stop();
        }

        module->state = MODULE_STATE_INITIALIZED;
    }

    return 0;
}

int module_cleanup_all(void)
{
    IMP_LOG_INFO(TAG, "Cleaning up all modules");

    /* Cleanup modules in reverse order */
    for (int i = g_module_count - 1; i >= 0; i--) {
        module_info_t* module = g_modules[i];

        if (module->cleanup) {
            IMP_LOG_INFO(TAG, "Cleaning up module '%s'", module->name);
            module->cleanup();
        }

        module->state = MODULE_STATE_UNREGISTERED;
    }

    /* Clear registry */
    memset(g_modules, 0, sizeof(g_modules));
    g_module_count = 0;
    g_system_initialized = false;

    IMP_LOG_INFO(TAG, "Module system cleanup complete");
    return 0;
}

int module_load_config_file(const char* module_name, void* config_buffer, size_t config_size,
                           int (*parse_func)(json_object* json, void* config))
{
    if (!module_name || !config_buffer || !parse_func) {
        return -1;
    }

    char config_path[MODULE_CONFIG_PATH_MAX];
    struct stat st;
    json_object* module_json = NULL;

    /* Try multiple config file locations in order of preference */

    /* 1. Try same directory as binary (for testing from network share) */
    char* binary_dir = get_binary_dir();
    if (binary_dir) {
        snprintf(config_path, sizeof(config_path), "%s/%s.json", binary_dir, module_name);
        IMP_LOG_DBG(TAG, "Trying module config: %s", config_path);

        if (stat(config_path, &st) == 0) {
            module_json = json_object_from_file(config_path);
            if (module_json) {
                IMP_LOG_INFO(TAG, "Loading config for module '%s' from %s (binary dir)", module_name, config_path);
                goto parse_config;
            }
        }
        free(binary_dir);
    }

    /* 2. Try system config directory */
    snprintf(config_path, sizeof(config_path), "%s/%s.json", MODULE_CONFIG_DIR, module_name);
    IMP_LOG_DBG(TAG, "Trying module config: %s", config_path);

    if (stat(config_path, &st) == 0) {
        module_json = json_object_from_file(config_path);
        if (module_json) {
            IMP_LOG_INFO(TAG, "Loading config for module '%s' from %s (system dir)", module_name, config_path);
            goto parse_config;
        }
    }

    /* No config file found */
    IMP_LOG_INFO(TAG, "No dedicated config file for module '%s', using defaults", module_name);
    return -1;

parse_config:

    /* Clear config buffer */
    memset(config_buffer, 0, config_size);

    /* Parse module config */
    int ret = parse_func(module_json, config_buffer);

    /* Cleanup */
    json_object_put(module_json);

    if (ret == 0) {
        IMP_LOG_INFO(TAG, "Module '%s' config loaded successfully", module_name);
    } else {
        IMP_LOG_ERR(TAG, "Failed to parse config for module '%s'", module_name);
    }

    return ret;
}

module_info_t* module_get_by_name(const char* name)
{
    if (!name) {
        return NULL;
    }

    for (int i = 0; i < g_module_count; i++) {
        if (strcmp(g_modules[i]->name, name) == 0) {
            return g_modules[i];
        }
    }

    return NULL;
}

int module_get_all(module_info_t** modules, int max_modules)
{
    if (!modules || max_modules <= 0) {
        return 0;
    }

    int count = (g_module_count < max_modules) ? g_module_count : max_modules;

    for (int i = 0; i < count; i++) {
        modules[i] = g_modules[i];
    }

    return count;
}

int module_rtsp_setup_all(rtsp_server_t* server)
{
    if (!server) {
        return -1;
    }

    IMP_LOG_INFO(TAG, "Setting up RTSP integration for all modules");

    for (int i = 0; i < g_module_count; i++) {
        module_info_t* module = g_modules[i];

        if (module->state == MODULE_STATE_RUNNING && module->rtsp_setup) {
            IMP_LOG_DBG(TAG, "Setting up RTSP for module '%s'", module->name);
            module->rtsp_setup(server);
        }
    }

    return 0;
}

int module_rtsp_frame_callback_all(rtsp_server_t* server, int channel, const uint8_t* frame_data, uint32_t frame_size, const struct timeval* timestamp)
{
    if (!server || !frame_data || frame_size == 0 || !timestamp) {
        return -1;
    }

    for (int i = 0; i < g_module_count; i++) {
        module_info_t* module = g_modules[i];

        if (module->state == MODULE_STATE_RUNNING && module->rtsp_frame_callback) {
            module->rtsp_frame_callback(server, channel, frame_data, frame_size, timestamp);
        }
    }

    return 0;
}

int module_rtsp_cleanup_all(rtsp_server_t* server)
{
    if (!server) {
        return -1;
    }

    IMP_LOG_INFO(TAG, "Cleaning up RTSP integration for all modules");

    for (int i = g_module_count - 1; i >= 0; i--) {
        module_info_t* module = g_modules[i];

        if (module->rtsp_cleanup) {
            IMP_LOG_DBG(TAG, "Cleaning up RTSP for module '%s'", module->name);
            module->rtsp_cleanup(server);
        }
    }

    return 0;
}
