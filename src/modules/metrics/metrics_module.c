/*
 * metrics_module.c - System Metrics module implementation
 * Copyright (C) 2025 Paul Philippov, Thingino Project
 * Self-contained system metrics module for monitoring and HTTP endpoints
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <json-c/json.h>
#include <sys/stat.h>

#include "../../common.h"
#ifdef ENABLE_RTSP
#include "../rtsp/rtsp_module.h"
#include "../rtsp/rtsp_server.h"
#endif
#include "metrics_module.h"

#ifdef ENABLE_HTTP
#include "../http/http_router.h"
#include "../http/http_module.h"
#endif

#define TAG "METRICS_MODULE"

/* Global metrics module state */
static metrics_module_state_t g_metrics_state = {0};

/* Forward declarations */
static void* metrics_collection_thread(void* arg);
static int setup_metrics_defaults(void);
static int allocate_history_buffers(void);
static void free_history_buffers(void);

/* Module registration - manual registration due to symbol stripping */
module_info_t metrics_module_info = {
    .name = METRICS_MODULE_NAME,
    .version = METRICS_MODULE_VERSION,
    .description = "System metrics collection and HTTP endpoints",
    .state = MODULE_STATE_UNREGISTERED,
    .module_data = &g_metrics_state,

    /* Lifecycle callbacks */
    .init = metrics_module_init,
    .start = metrics_module_start,
    .stop = metrics_module_stop,
    .cleanup = metrics_module_cleanup,

    /* Configuration callbacks */
    .config_parse = metrics_module_config_parse,
    .config_validate = metrics_module_config_validate,
    .config_free = metrics_module_config_free,
    .config_size = sizeof(metrics_module_config_t),

    /* RTSP integration - not needed for metrics */
    .rtsp_setup = NULL,
    .rtsp_frame_callback = NULL,
    .rtsp_cleanup = NULL,

    /* Statistics */
    .get_stats = metrics_module_get_stats
};

/* Auto-register module at startup */
MODULE_REGISTER(metrics_module_info);

int metrics_module_init(void* config)
{
    if (!config) {
        IMP_LOG_ERR(TAG, "Invalid metrics configuration");
        return -1;
    }

    if (g_metrics_state.initialized) {
        IMP_LOG_WARN(TAG, "Metrics module already initialized");
        return 0;
    }

    IMP_LOG_INFO(TAG, "Initializing metrics module");

    /* Copy configuration */
    memcpy(&g_metrics_state.config, config, sizeof(metrics_module_config_t));

    /* Check if metrics is enabled */
    if (!g_metrics_state.config.enabled) {
        IMP_LOG_INFO(TAG, "Metrics disabled in configuration");
        g_metrics_state.initialized = true;
        return 0;
    }

    /* Initialize mutex */
    if (pthread_mutex_init(&g_metrics_state.mutex, NULL) != 0) {
        IMP_LOG_ERR(TAG, "Failed to initialize metrics mutex");
        return -1;
    }

    /* Initialize history mutex */
    if (pthread_mutex_init(&g_metrics_state.history.mutex, NULL) != 0) {
        IMP_LOG_ERR(TAG, "Failed to initialize history mutex");
        pthread_mutex_destroy(&g_metrics_state.mutex);
        return -1;
    }

    /* Setup default values */
    if (setup_metrics_defaults() != 0) {
        IMP_LOG_ERR(TAG, "Failed to setup metrics defaults");
        pthread_mutex_destroy(&g_metrics_state.history.mutex);
        pthread_mutex_destroy(&g_metrics_state.mutex);
        return -1;
    }

    /* Allocate history buffers */
    if (allocate_history_buffers() != 0) {
        IMP_LOG_ERR(TAG, "Failed to allocate history buffers");
        pthread_mutex_destroy(&g_metrics_state.history.mutex);
        pthread_mutex_destroy(&g_metrics_state.mutex);
        return -1;
    }

    /* Initialize statistics */
    g_metrics_state.collections_count = 0;
    g_metrics_state.http_requests_count = 0;
    g_metrics_state.start_time = time(NULL);

    g_metrics_state.initialized = true;
    IMP_LOG_INFO(TAG, "Metrics module initialized successfully");

    return 0;
}

int metrics_module_start(void)
{
    if (!g_metrics_state.initialized) {
        IMP_LOG_ERR(TAG, "Metrics module not initialized");
        return -1;
    }

    if (!g_metrics_state.config.enabled) {
        IMP_LOG_INFO(TAG, "Metrics disabled, not starting");
        return 0;
    }

    if (g_metrics_state.running) {
        IMP_LOG_WARN(TAG, "Metrics module already running");
        return 0;
    }

    IMP_LOG_INFO(TAG, "Starting metrics module");

    /* Start collection thread */
    g_metrics_state.thread_should_exit = false;
    int ret = pthread_create(&g_metrics_state.collection_thread, NULL,
                            metrics_collection_thread, NULL);
    if (ret != 0) {
        IMP_LOG_ERR(TAG, "Failed to create metrics collection thread: %s", strerror(ret));
        return -1;
    }

    g_metrics_state.running = true;
    IMP_LOG_INFO(TAG, "Metrics module started successfully");

    return 0;
}

int metrics_module_stop(void)
{
    if (!g_metrics_state.running) {
        IMP_LOG_INFO(TAG, "Metrics module not running");
        return 0;
    }

    IMP_LOG_INFO(TAG, "Stopping metrics module");

    /* Signal thread to exit */
    g_metrics_state.thread_should_exit = true;

    /* Wait for thread to finish */
    if (pthread_join(g_metrics_state.collection_thread, NULL) != 0) {
        IMP_LOG_WARN(TAG, "Failed to join metrics collection thread");
    }

    g_metrics_state.running = false;
    IMP_LOG_INFO(TAG, "Metrics module stopped successfully");

    return 0;
}

int metrics_module_cleanup(void)
{
    if (!g_metrics_state.initialized) {
        return 0;
    }

    IMP_LOG_INFO(TAG, "Cleaning up metrics module");

    /* Stop if running */
    if (g_metrics_state.running) {
        metrics_module_stop();
    }

    /* Free history buffers */
    free_history_buffers();

    /* Cleanup mutexes */
    pthread_mutex_destroy(&g_metrics_state.history.mutex);
    pthread_mutex_destroy(&g_metrics_state.mutex);

    /* Reset state */
    memset(&g_metrics_state, 0, sizeof(metrics_module_state_t));

    IMP_LOG_INFO(TAG, "Metrics module cleanup complete");

    return 0;
}

static int setup_metrics_defaults(void)
{
    /* Initialize state variables */
    g_metrics_state.thread_should_exit = false;
    g_metrics_state.cpu_state.initialized = false;

    /* Setup history configuration */
    g_metrics_state.history.current_index = 0;
    g_metrics_state.history.max_size = g_metrics_state.config.history_size;

    return 0;
}

static int allocate_history_buffers(void)
{
    int history_size = g_metrics_state.config.history_size;

    /* Allocate process history */
    if (g_metrics_state.config.collect.process) {
        g_metrics_state.history.process_history = calloc(history_size, sizeof(process_metrics_t));
        if (!g_metrics_state.history.process_history) {
            IMP_LOG_ERR(TAG, "Failed to allocate process history buffer");
            return -1;
        }
    }

    /* Allocate stream history for each channel */
    if (g_metrics_state.config.collect.streams) {
        for (int i = 0; i < 4; i++) {
            g_metrics_state.history.stream_history[i] = calloc(history_size, sizeof(stream_metrics_t));
            if (!g_metrics_state.history.stream_history[i]) {
                IMP_LOG_ERR(TAG, "Failed to allocate stream history buffer for channel %d", i);
                return -1;
            }
        }
    }

    /* Allocate system history */
    if (g_metrics_state.config.collect.system) {
        g_metrics_state.history.system_history = calloc(history_size, sizeof(system_metrics_t));
        if (!g_metrics_state.history.system_history) {
            IMP_LOG_ERR(TAG, "Failed to allocate system history buffer");
            return -1;
        }
    }

    /* Allocate network history */
    if (g_metrics_state.config.collect.network) {
        g_metrics_state.history.network_history = calloc(history_size, sizeof(network_metrics_t));
        if (!g_metrics_state.history.network_history) {
            IMP_LOG_ERR(TAG, "Failed to allocate network history buffer");
            return -1;
        }
    }

    return 0;
}

static void free_history_buffers(void)
{
    if (g_metrics_state.history.process_history) {
        free(g_metrics_state.history.process_history);
        g_metrics_state.history.process_history = NULL;
    }

    for (int i = 0; i < 4; i++) {
        if (g_metrics_state.history.stream_history[i]) {
            free(g_metrics_state.history.stream_history[i]);
            g_metrics_state.history.stream_history[i] = NULL;
        }
    }

    if (g_metrics_state.history.system_history) {
        free(g_metrics_state.history.system_history);
        g_metrics_state.history.system_history = NULL;
    }

    if (g_metrics_state.history.network_history) {
        free(g_metrics_state.history.network_history);
        g_metrics_state.history.network_history = NULL;
    }
}

int metrics_module_config_parse(json_object* json, void* config)
{
    if (!json || !config) {
        IMP_LOG_ERR(TAG, "Invalid parameters for config parsing");
        return -1;
    }

    metrics_module_config_t* metrics_config = (metrics_module_config_t*)config;

    /* Set default values */
    metrics_config->enabled = true;
    metrics_config->collection_interval_ms = 5000;
    metrics_config->history_size = 60;

    /* Default metric collection settings */
    metrics_config->collect.process = true;
    metrics_config->collect.streams = true;
    metrics_config->collect.clients = true;
    metrics_config->collect.system = true;
    metrics_config->collect.network = false;

    /* Default export formats */
    metrics_config->exporters.prometheus = true;
    metrics_config->exporters.json_status = true;
    metrics_config->exporters.json_health = true;
    metrics_config->exporters.json_info = true;

    /* Default HTTP endpoints */
    strncpy(metrics_config->endpoints.metrics_path, "/metrics", sizeof(metrics_config->endpoints.metrics_path) - 1);
    strncpy(metrics_config->endpoints.status_path, "/status.json", sizeof(metrics_config->endpoints.status_path) - 1);
    strncpy(metrics_config->endpoints.health_path, "/health.json", sizeof(metrics_config->endpoints.health_path) - 1);
    strncpy(metrics_config->endpoints.info_path, "/info.json", sizeof(metrics_config->endpoints.info_path) - 1);
    strncpy(metrics_config->endpoints.config_path, "/config.json", sizeof(metrics_config->endpoints.config_path) - 1);

    /* Default performance settings */
    metrics_config->enable_detailed_logging = false;
    metrics_config->max_clients_per_endpoint = 10;

    /* Parse JSON fields */
    json_object* obj;

    if (json_object_object_get_ex(json, "enabled", &obj)) {
        metrics_config->enabled = json_object_get_boolean(obj);
    }

    if (json_object_object_get_ex(json, "collection_interval_ms", &obj)) {
        metrics_config->collection_interval_ms = json_object_get_int(obj);
    }

    if (json_object_object_get_ex(json, "history_size", &obj)) {
        metrics_config->history_size = json_object_get_int(obj);
    }

    /* Parse collection settings */
    json_object* collect_obj;
    if (json_object_object_get_ex(json, "collect", &collect_obj)) {
        if (json_object_object_get_ex(collect_obj, "process", &obj)) {
            metrics_config->collect.process = json_object_get_boolean(obj);
        }
        if (json_object_object_get_ex(collect_obj, "streams", &obj)) {
            metrics_config->collect.streams = json_object_get_boolean(obj);
        }
        if (json_object_object_get_ex(collect_obj, "clients", &obj)) {
            metrics_config->collect.clients = json_object_get_boolean(obj);
        }
        if (json_object_object_get_ex(collect_obj, "system", &obj)) {
            metrics_config->collect.system = json_object_get_boolean(obj);
        }
        if (json_object_object_get_ex(collect_obj, "network", &obj)) {
            metrics_config->collect.network = json_object_get_boolean(obj);
        }
    }

    /* Parse exporter settings */
    json_object* exporters_obj;
    if (json_object_object_get_ex(json, "exporters", &exporters_obj)) {
        if (json_object_object_get_ex(exporters_obj, "prometheus", &obj)) {
            metrics_config->exporters.prometheus = json_object_get_boolean(obj);
        }
        if (json_object_object_get_ex(exporters_obj, "json_status", &obj)) {
            metrics_config->exporters.json_status = json_object_get_boolean(obj);
        }
        if (json_object_object_get_ex(exporters_obj, "json_health", &obj)) {
            metrics_config->exporters.json_health = json_object_get_boolean(obj);
        }
        if (json_object_object_get_ex(exporters_obj, "json_info", &obj)) {
            metrics_config->exporters.json_info = json_object_get_boolean(obj);
        }
    }

    if (json_object_object_get_ex(json, "enable_detailed_logging", &obj)) {
        metrics_config->enable_detailed_logging = json_object_get_boolean(obj);
    }

    if (json_object_object_get_ex(json, "max_clients_per_endpoint", &obj)) {
        metrics_config->max_clients_per_endpoint = json_object_get_int(obj);
    }

    IMP_LOG_INFO(TAG, "Metrics config loaded:");
    IMP_LOG_INFO(TAG, "  enabled: %s", metrics_config->enabled ? "true" : "false");
    IMP_LOG_INFO(TAG, "  collection_interval_ms: %d", metrics_config->collection_interval_ms);
    IMP_LOG_INFO(TAG, "  history_size: %d", metrics_config->history_size);
    IMP_LOG_INFO(TAG, "  collect.process: %s", metrics_config->collect.process ? "true" : "false");
    IMP_LOG_INFO(TAG, "  collect.streams: %s", metrics_config->collect.streams ? "true" : "false");
    IMP_LOG_INFO(TAG, "  collect.clients: %s", metrics_config->collect.clients ? "true" : "false");
    IMP_LOG_INFO(TAG, "  collect.system: %s", metrics_config->collect.system ? "true" : "false");
    IMP_LOG_INFO(TAG, "  collect.network: %s", metrics_config->collect.network ? "true" : "false");
    IMP_LOG_INFO(TAG, "  exporters.prometheus: %s", metrics_config->exporters.prometheus ? "true" : "false");
    IMP_LOG_INFO(TAG, "  exporters.json_status: %s", metrics_config->exporters.json_status ? "true" : "false");
    IMP_LOG_INFO(TAG, "  exporters.json_health: %s", metrics_config->exporters.json_health ? "true" : "false");
    IMP_LOG_INFO(TAG, "  enable_detailed_logging: %s", metrics_config->enable_detailed_logging ? "true" : "false");

    return 0;
}

int metrics_module_config_validate(void* config)
{
    if (!config) {
        IMP_LOG_ERR(TAG, "Invalid metrics configuration");
        return -1;
    }

    metrics_module_config_t* metrics_config = (metrics_module_config_t*)config;

    /* Validate collection interval */
    if (metrics_config->collection_interval_ms <= 0) {
        IMP_LOG_ERR(TAG, "Invalid collection interval: %d", metrics_config->collection_interval_ms);
        return -1;
    }

    /* Validate history size */
    if (metrics_config->history_size <= 0 || metrics_config->history_size > 1000) {
        IMP_LOG_ERR(TAG, "Invalid history size: %d (must be 1-1000)", metrics_config->history_size);
        return -1;
    }

    /* Validate max clients */
    if (metrics_config->max_clients_per_endpoint <= 0 || metrics_config->max_clients_per_endpoint > 100) {
        IMP_LOG_ERR(TAG, "Invalid max clients per endpoint: %d (must be 1-100)", metrics_config->max_clients_per_endpoint);
        return -1;
    }

    return 0;
}

void metrics_module_config_free(void* config)
{
    /* No dynamic memory to free in metrics config */
    (void)config;
}

int metrics_module_get_stats(void* stats_buffer, size_t buffer_size)
{
    if (!stats_buffer || buffer_size < sizeof(unsigned long) * 3) {
        IMP_LOG_ERR(TAG, "Invalid stats buffer");
        return -1;
    }

    pthread_mutex_lock(&g_metrics_state.mutex);

    unsigned long* stats = (unsigned long*)stats_buffer;
    stats[0] = g_metrics_state.collections_count;
    stats[1] = g_metrics_state.http_requests_count;
    stats[2] = time(NULL) - g_metrics_state.start_time;

    pthread_mutex_unlock(&g_metrics_state.mutex);

    return 0;
}

bool metrics_module_is_enabled(void)
{
    return g_metrics_state.initialized && g_metrics_state.config.enabled && g_metrics_state.running;
}

static void* metrics_collection_thread(void* arg)
{
    (void)arg;

    IMP_LOG_INFO(TAG, "Metrics collection thread started");

    while (!g_metrics_state.thread_should_exit) {
        pthread_mutex_lock(&g_metrics_state.mutex);

        /* Collect process metrics */
        if (g_metrics_state.config.collect.process) {
            if (metrics_get_process_metrics(&g_metrics_state.current_process) == 0) {


                if (g_metrics_state.history.process_history) {
                    int idx = g_metrics_state.history.current_index;
                    g_metrics_state.history.process_history[idx] = g_metrics_state.current_process;
                }
            }
        }

        /* Collect stream metrics */
        if (g_metrics_state.config.collect.streams) {
            for (int i = 0; i < FS_CHN_NUM; i++) {
                if (metrics_get_stream_metrics(i, &g_metrics_state.current_streams[i]) == 0) {
                    if (g_metrics_state.history.stream_history[i]) {
                        int idx = g_metrics_state.history.current_index;
                        g_metrics_state.history.stream_history[i][idx] = g_metrics_state.current_streams[i];
                    }
                }
            }
        }

        /* Collect system metrics */
        if (g_metrics_state.config.collect.system) {
            if (metrics_get_system_metrics(&g_metrics_state.current_system) == 0) {
                if (g_metrics_state.history.system_history) {
                    int idx = g_metrics_state.history.current_index;
                    g_metrics_state.history.system_history[idx] = g_metrics_state.current_system;
                }
            }
        }

        /* Update collection statistics */
        g_metrics_state.collections_count++;

        /* Advance history index */
        g_metrics_state.history.current_index = (g_metrics_state.history.current_index + 1) % g_metrics_state.history.max_size;

        /* Debug logging if enabled */
        if (g_metrics_state.config.enable_detailed_logging) {
            IMP_LOG_DBG(TAG, "Collected metrics: collections=%lu, process_cpu=%d%%, process_mem=%dKB",
                        g_metrics_state.collections_count,
                        g_metrics_state.current_process.cpu_usage_percent,
                        g_metrics_state.current_process.memory_usage_kb);
        }

        pthread_mutex_unlock(&g_metrics_state.mutex);

        /* Sleep for configured interval */
        usleep(g_metrics_state.config.collection_interval_ms * 1000);
    }

    IMP_LOG_INFO(TAG, "Metrics collection thread exiting");
    return NULL;
}

/* Metrics collection functions - extracted from system_stats.c */

int metrics_get_process_metrics(process_metrics_t* metrics)
{
    if (!metrics) {
        return -1;
    }

    /* Get memory usage from main process, not the metrics thread */
    FILE *file = fopen("/proc/self/status", "r");
    if (!file) {
        return -1;
    }

    char line[256];
    int rss_kb = 0, thread_count = 0;

    while (fgets(line, sizeof(line), file)) {
        if (strncmp(line, "VmRSS:", 6) == 0) {
            sscanf(line, "VmRSS: %d kB", &rss_kb);
        } else if (strncmp(line, "Threads:", 8) == 0) {
            sscanf(line, "Threads: %d", &thread_count);
        }
    }
    fclose(file);



    /* Get CPU usage */
    file = fopen("/proc/self/stat", "r");
    if (!file) {
        return -1;
    }

    unsigned long utime = 0, stime = 0;
    struct timeval current_time;
    int cpu_percent = 0;

    if (fgets(line, sizeof(line), file)) {
        /* Parse /proc/self/stat format: pid comm state ppid pgrp session tty_nr tpgid flags minflt cminflt majflt cmajflt utime stime ... */
        /* We need fields 13 (utime) and 14 (stime) - 0-indexed */
        char *tokens[20];
        char *token = strtok(line, " ");
        int field = 0;

        while (token && field < 20) {
            tokens[field] = token;
            token = strtok(NULL, " ");
            field++;
        }

        if (field > 14) {
            utime = strtoul(tokens[13], NULL, 10);  /* utime at position 13 */
            stime = strtoul(tokens[14], NULL, 10);  /* stime at position 14 */
        }
    }
    fclose(file);

    unsigned long current_time_us = (unsigned long)get_monotonic_time_us();

    if (g_metrics_state.cpu_state.initialized) {
        unsigned long total_time = (utime + stime) - (g_metrics_state.cpu_state.last_utime + g_metrics_state.cpu_state.last_stime);
        unsigned long elapsed_us = current_time_us - g_metrics_state.cpu_state.last_time_us;

        if (elapsed_us > 0) {
            long clock_ticks_per_sec = sysconf(_SC_CLK_TCK);
            cpu_percent = (total_time * 100 * 1000000) / (clock_ticks_per_sec * elapsed_us);
        }
    }

    g_metrics_state.cpu_state.last_utime = utime;
    g_metrics_state.cpu_state.last_stime = stime;
    g_metrics_state.cpu_state.last_time_us = current_time_us;
    g_metrics_state.cpu_state.initialized = true;

    /* Fill metrics structure */
    metrics->memory_usage_kb = rss_kb;
    metrics->cpu_usage_percent = cpu_percent;
    metrics->thread_count = thread_count;
    metrics->timestamp = current_time_us;

    return 0;
}



/* Stream metrics tracking per channel */
static struct {
    uint32_t frame_count;
    uint32_t error_count;
    unsigned long total_frame_size;
    unsigned long last_fps_time;
    uint32_t fps_frame_count;
    double current_fps;
    pthread_mutex_t mutex;
} stream_stats[FS_CHN_NUM];

static bool stream_stats_initialized = false;

/* Initialize stream statistics tracking */
static void init_stream_stats(void)
{
    if (stream_stats_initialized) {
        return;
    }

    for (int i = 0; i < FS_CHN_NUM; i++) {
        memset(&stream_stats[i], 0, sizeof(stream_stats[i]));
        pthread_mutex_init(&stream_stats[i].mutex, NULL);

        /* Initialize with current monotonic time */
        stream_stats[i].last_fps_time = get_monotonic_time_us();
        /* printf("METRICS DEBUG: Initialized stream_stats[%d].last_fps_time = %lu\n", i, stream_stats[i].last_fps_time); */
    }

    stream_stats_initialized = true;
}

/* Update stream metrics when a frame is processed */
void metrics_update_stream_frame(int channel, unsigned int frame_size, bool is_error)
{
    if (!metrics_module_is_enabled() || channel < 0 || channel >= FS_CHN_NUM) {
        return;
    }

    if (!stream_stats_initialized) {
        init_stream_stats();
    }

    pthread_mutex_lock(&stream_stats[channel].mutex);

    /* Update frame count and size */
    stream_stats[channel].frame_count++;
    if (is_error) {
        stream_stats[channel].error_count++;
    } else {
        stream_stats[channel].total_frame_size += frame_size;
    }

    /* Update FPS calculation every second using monotonic time */
    /* Use 32-bit variables to avoid MIPS 64-bit issues */
    unsigned long stream_metrics_current_time_us = (unsigned long)get_monotonic_time_us();

    /* Raw clock debug disabled for performance */
    unsigned long fps_time_diff = stream_metrics_current_time_us - (unsigned long)stream_stats[channel].last_fps_time;

    stream_stats[channel].fps_frame_count++;

    /* Handle timestamp issues - if fps_time_diff is unreasonable, reset baseline */
    if (fps_time_diff > 10000000 || stream_metrics_current_time_us < stream_stats[channel].last_fps_time) {
        /* Reset timing baseline due to clock jump/wraparound */
        printf("METRICS DEBUG: Resetting timing baseline for channel %d (fps_time_diff=%lu)\n", channel, fps_time_diff);
        stream_stats[channel].last_fps_time = (unsigned long)stream_metrics_current_time_us;
        stream_stats[channel].fps_frame_count = 1;  /* Start counting from this frame */
        pthread_mutex_unlock(&stream_stats[channel].mutex);
        return;
    }

    /* Calculate FPS every 1 second (1,000,000 microseconds) */
    if (fps_time_diff >= 1000000) {
        stream_stats[channel].current_fps = (double)stream_stats[channel].fps_frame_count /
                                           ((double)fps_time_diff / 1000000.0);

        /* FPS debug logging disabled for performance */

        /* Reset for next calculation */
        stream_stats[channel].fps_frame_count = 0;
        stream_stats[channel].last_fps_time = (unsigned long)stream_metrics_current_time_us;

        /* Update the global channel_metrics array for status.json compatibility */
        extern channel_metrics_t channel_metrics[];
        if (pthread_mutex_trylock(&channel_metrics[channel].mutex) == 0) {
            channel_metrics[channel].fps = stream_stats[channel].current_fps;
            channel_metrics[channel].frame_count = stream_stats[channel].frame_count;
            channel_metrics[channel].error_count = stream_stats[channel].error_count;
            channel_metrics[channel].avg_frame_size = stream_stats[channel].frame_count > 0 ?
                (stream_stats[channel].total_frame_size / stream_stats[channel].frame_count) : 0;
            channel_metrics[channel].last_update_time = time(NULL);
            pthread_mutex_unlock(&channel_metrics[channel].mutex);
        }

        /* Update the global metrics state for Prometheus endpoint */
        extern struct chn_conf chn[];
        int client_count = 0;
#ifdef ENABLE_RTSP
        rtsp_server_t* rtsp_server = rtsp_module_get_server();
        if (rtsp_server && chn[channel].enable) {
            client_count = rtsp_server_get_client_count(rtsp_server, channel);
            if (client_count < 0) client_count = 0;
        }
#endif

        g_metrics_state.current_streams[channel].fps = stream_stats[channel].current_fps;
        g_metrics_state.current_streams[channel].frame_count = stream_stats[channel].frame_count;
        g_metrics_state.current_streams[channel].error_count = stream_stats[channel].error_count;
        g_metrics_state.current_streams[channel].avg_frame_size = stream_stats[channel].frame_count > 0 ?
            (stream_stats[channel].total_frame_size / stream_stats[channel].frame_count) : 0;
        g_metrics_state.current_streams[channel].client_count = client_count;
        g_metrics_state.current_streams[channel].timestamp = (unsigned long)stream_metrics_current_time_us;
    }

    pthread_mutex_unlock(&stream_stats[channel].mutex);
}

int metrics_get_stream_metrics(int channel, stream_metrics_t* metrics)
{
    if (!metrics || channel < 0 || channel >= FS_CHN_NUM) {
        return -1;
    }

    if (!stream_stats_initialized) {
        memset(metrics, 0, sizeof(stream_metrics_t));
        return 0;
    }

    /* Get external references */
    extern struct chn_conf chn[];

    unsigned long current_time_us = (unsigned long)get_monotonic_time_us();

    pthread_mutex_lock(&stream_stats[channel].mutex);

    /* Get current stream statistics */
    metrics->fps = stream_stats[channel].current_fps;
    metrics->frame_count = stream_stats[channel].frame_count;
    metrics->error_count = stream_stats[channel].error_count;
    metrics->avg_frame_size = stream_stats[channel].frame_count > 0 ?
        (stream_stats[channel].total_frame_size / stream_stats[channel].frame_count) : 0;

    pthread_mutex_unlock(&stream_stats[channel].mutex);

    /* Get client count from RTSP server */
    int client_count = 0;
#ifdef ENABLE_RTSP
    rtsp_server_t* rtsp_server = rtsp_module_get_server();
    if (rtsp_server && chn[channel].enable) {
        client_count = rtsp_server_get_client_count(rtsp_server, channel);
        if (client_count < 0) {
            client_count = 0;
        }
    }
#endif

    metrics->client_count = client_count;
    metrics->timestamp = current_time_us;

    return 0;
}

int metrics_get_system_metrics(system_metrics_t* metrics)
{
    if (!metrics) {
        return -1;
    }

    unsigned long current_time_us = (unsigned long)get_monotonic_time_us();

    /* Get system uptime */
    FILE *file = fopen("/proc/uptime", "r");
    double uptime = 0.0;
    if (file) {
        fscanf(file, "%lf", &uptime);
        fclose(file);
    }

    /* Get load averages */
    file = fopen("/proc/loadavg", "r");
    double load1 = 0.0, load5 = 0.0, load15 = 0.0;
    if (file) {
        fscanf(file, "%lf %lf %lf", &load1, &load5, &load15);
        fclose(file);
    }

    /* Get memory info */
    file = fopen("/proc/meminfo", "r");
    unsigned long mem_total = 0, mem_available = 0;
    if (file) {
        char line[256];
        while (fgets(line, sizeof(line), file)) {
            if (strncmp(line, "MemTotal:", 9) == 0) {
                sscanf(line, "MemTotal: %lu kB", &mem_total);
            } else if (strncmp(line, "MemAvailable:", 13) == 0) {
                sscanf(line, "MemAvailable: %lu kB", &mem_available);
            }
        }
        fclose(file);
    }

    /* Fill metrics structure */
    metrics->uptime_seconds = (unsigned long)uptime;
    metrics->load_average[0] = load1;
    metrics->load_average[1] = load5;
    metrics->load_average[2] = load15;
    metrics->memory_total_kb = mem_total;
    metrics->memory_available_kb = mem_available;
    metrics->timestamp = current_time_us;

    return 0;
}

/* HTTP endpoint handlers */

void metrics_handle_status_request(int client_socket)
{
    if (!metrics_module_is_enabled()) {
        http_send_json(client_socket, "{\"error\":\"Metrics module not enabled\"}");
        return;
    }

    pthread_mutex_lock(&g_metrics_state.mutex);

    char json_buffer[2048];
    snprintf(json_buffer, sizeof(json_buffer),
        "{"
        "\"timestamp\":%ld,"
        "\"process\":{"
            "\"cpu_percent\":%d,"
            "\"memory_kb\":%d,"
            "\"threads\":%d"
        "},"
        "\"system\":{"
            "\"uptime_seconds\":%lu,"
            "\"load_1min\":%.2f,"
            "\"load_5min\":%.2f,"
            "\"load_15min\":%.2f,"
            "\"memory_total_kb\":%lu,"
            "\"memory_available_kb\":%lu"
        "},"
        "\"collections\":%lu"
        "}",
        time(NULL),
        g_metrics_state.current_process.cpu_usage_percent,
        g_metrics_state.current_process.memory_usage_kb,
        g_metrics_state.current_process.thread_count,
        g_metrics_state.current_system.uptime_seconds,
        g_metrics_state.current_system.load_average[0],
        g_metrics_state.current_system.load_average[1],
        g_metrics_state.current_system.load_average[2],
        g_metrics_state.current_system.memory_total_kb,
        g_metrics_state.current_system.memory_available_kb,
        g_metrics_state.collections_count
    );

    pthread_mutex_unlock(&g_metrics_state.mutex);

    http_send_json(client_socket, json_buffer);
}

void metrics_handle_prometheus_request(int client_socket)
{
    if (!metrics_module_is_enabled()) {
        http_send_error(client_socket, HTTP_STATUS_SERVICE_UNAVAILABLE, "# Metrics module not enabled");
        return;
    }

    pthread_mutex_lock(&g_metrics_state.mutex);

    char metrics_buffer[4096];
    time_t now = time(NULL);

    char stream_metrics_buffer[2048] = "";

    /* Add stream metrics for each channel */
    for (int i = 0; i < FS_CHN_NUM; i++) {
        char channel_metrics[512];
        snprintf(channel_metrics, sizeof(channel_metrics),
            "# HELP stream_fps Frames per second for each channel\n"
            "# TYPE stream_fps gauge\n"
            "stream_fps{channel=\"%d\"} %.2f %ld\n"
            "\n"
            "# HELP stream_frame_count Total frames processed\n"
            "# TYPE stream_frame_count counter\n"
            "stream_frame_count{channel=\"%d\"} %u %ld\n"
            "\n"
            "# HELP stream_error_count Total errors encountered\n"
            "# TYPE stream_error_count counter\n"
            "stream_error_count{channel=\"%d\"} %u %ld\n"
            "\n"
            "# HELP stream_frame_size_bytes Average frame size in bytes\n"
            "# TYPE stream_frame_size_bytes gauge\n"
            "stream_frame_size_bytes{channel=\"%d\"} %u %ld\n"
            "\n"
            "# HELP stream_clients Connected clients\n"
            "# TYPE stream_clients gauge\n"
            "stream_clients{channel=\"%d\"} %u %ld\n"
            "\n",
            i, g_metrics_state.current_streams[i].fps, now,
            i, g_metrics_state.current_streams[i].frame_count, now,
            i, g_metrics_state.current_streams[i].error_count, now,
            i, g_metrics_state.current_streams[i].avg_frame_size, now,
            i, g_metrics_state.current_streams[i].client_count, now
        );
        strncat(stream_metrics_buffer, channel_metrics, sizeof(stream_metrics_buffer) - strlen(stream_metrics_buffer) - 1);
    }

    snprintf(metrics_buffer, sizeof(metrics_buffer),
        "# HELP process_cpu_percent Process CPU usage percentage\n"
        "# TYPE process_cpu_percent gauge\n"
        "process_cpu_percent %d %ld\n"
        "\n"
        "# HELP process_memory_kb Process memory usage in KB\n"
        "# TYPE process_memory_kb gauge\n"
        "process_memory_kb %d %ld\n"
        "\n"
        "# HELP process_threads Process thread count\n"
        "# TYPE process_threads gauge\n"
        "process_threads %d %ld\n"
        "\n"
        "# HELP system_uptime_seconds System uptime in seconds\n"
        "# TYPE system_uptime_seconds counter\n"
        "system_uptime_seconds %lu %ld\n"
        "\n"
        "# HELP system_load_average System load average\n"
        "# TYPE system_load_average gauge\n"
        "system_load_average{period=\"1min\"} %.2f %ld\n"
        "system_load_average{period=\"5min\"} %.2f %ld\n"
        "system_load_average{period=\"15min\"} %.2f %ld\n"
        "\n"
        "# HELP metrics_collections_total Total metrics collections\n"
        "# TYPE metrics_collections_total counter\n"
        "metrics_collections_total %lu %ld\n"
        "\n"
        "%s",
        g_metrics_state.current_process.cpu_usage_percent, now,
        g_metrics_state.current_process.memory_usage_kb, now,
        g_metrics_state.current_process.thread_count, now,
        g_metrics_state.current_system.uptime_seconds, now,
        g_metrics_state.current_system.load_average[0], now,
        g_metrics_state.current_system.load_average[1], now,
        g_metrics_state.current_system.load_average[2], now,
        g_metrics_state.collections_count, now,
        stream_metrics_buffer
    );

    pthread_mutex_unlock(&g_metrics_state.mutex);

    http_send_response(client_socket, 200, "text/plain; version=0.0.4; charset=utf-8", metrics_buffer);
}

#ifdef ENABLE_HTTP
/* Handler wrapper functions */
static void metrics_handle_prometheus_wrapper(int client_socket, const char* request) {
    metrics_handle_prometheus_request(client_socket);
}

static void metrics_handle_status_wrapper(int client_socket, const char* request) {
    metrics_handle_status_request(client_socket);
}

/* Route registration for HTTP module */
int metrics_register_routes(void)
{
    /* Define routes */
    static const http_route_t metrics_routes[] = {
        {"/metrics", HTTP_METHOD_GET, metrics_handle_prometheus_wrapper, "metrics", "Prometheus metrics"},
        {"/process.json", HTTP_METHOD_GET, metrics_handle_status_wrapper, "metrics", "Process metrics (JSON)"},
    };

    /* Register all routes */
    int ret = http_router_register_routes(metrics_routes,
                                         sizeof(metrics_routes) / sizeof(metrics_routes[0]));
    if (ret < 0) {
        IMP_LOG_ERR(TAG, "Failed to register HTTP routes");
        return ret;
    }

    IMP_LOG_INFO(TAG, "Registered %zu HTTP routes",
                 sizeof(metrics_routes) / sizeof(metrics_routes[0]));
    return 0;
}
#endif
