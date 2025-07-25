/*
 * metrics_module.h - System Metrics module for modular streamer
 * Copyright (C) 2025 Paul Philippov, Thingino Project
 *
 * Self-contained system metrics module for monitoring and HTTP endpoints
 */

#ifndef __METRICS_MODULE_H__
#define __METRICS_MODULE_H__

#include <pthread.h>
#include <stdint.h>
#include <stdbool.h>

#include <sys/time.h>

#include "../../module_system.h"

#define METRICS_MODULE_VERSION "1.0.0"
#define METRICS_MODULE_NAME "metrics"

/* Metrics module configuration - self-contained */
typedef struct metrics_module_config {
    bool enabled;                     /* Enable/disable metrics collection */

    /* Collection settings */
    int collection_interval_ms;       /* Metrics collection interval (default: 5000ms) */
    int history_size;                 /* Number of historical samples to keep (default: 60) */

    /* Metric types to collect */
    struct {
        bool process;                 /* Process metrics (CPU, memory, threads) */
        bool streams;                 /* Stream metrics (FPS, frames, errors) */
        bool clients;                 /* Client connection metrics */
        bool system;                  /* System-wide metrics (uptime, load) */
        bool network;                 /* Network statistics */
    } collect;

    /* Export formats */
    struct {
        bool prometheus;              /* Prometheus metrics format */
        bool json_status;             /* JSON status endpoint */
        bool json_health;             /* JSON health check endpoint */
        bool json_info;               /* JSON info endpoint */
    } exporters;

    /* HTTP endpoint configuration */
    struct {
        char metrics_path[64];        /* Prometheus metrics endpoint (default: "/metrics") */
        char status_path[64];         /* Status JSON endpoint (default: "/status.json") */
        char health_path[64];         /* Health JSON endpoint (default: "/health.json") */
        char info_path[64];           /* Info JSON endpoint (default: "/info.json") */
        char config_path[64];         /* Config JSON endpoint (default: "/config.json") */
    } endpoints;

    /* Performance settings */
    bool enable_detailed_logging;     /* Enable detailed metrics logging */
    int max_clients_per_endpoint;     /* Max concurrent clients per endpoint */
} metrics_module_config_t;

/* Process metrics structure */
typedef struct {
    int memory_usage_kb;              /* Memory usage in KB */
    int cpu_usage_percent;            /* CPU usage percentage */
    int thread_count;                 /* Number of threads */
    unsigned long timestamp;          /* Collection timestamp */
} process_metrics_t;

/* Stream metrics structure */
typedef struct {
    double fps;                       /* Frames per second */
    uint32_t frame_count;             /* Total frame count */
    uint32_t error_count;             /* Error count */
    uint32_t avg_frame_size;          /* Average frame size in bytes */
    uint32_t client_count;            /* Connected clients */
    unsigned long timestamp;          /* Collection timestamp */
} stream_metrics_t;

/* System metrics structure */
typedef struct {
    unsigned long uptime_seconds;     /* System uptime */
    double load_average[3];           /* 1, 5, 15 minute load averages */
    unsigned long memory_total_kb;    /* Total system memory */
    unsigned long memory_available_kb; /* Available system memory */
    unsigned long timestamp;          /* Collection timestamp */
} system_metrics_t;

/* Network metrics structure */
typedef struct {
    unsigned long bytes_sent;         /* Total bytes sent */
    unsigned long bytes_received;     /* Total bytes received */
    uint32_t packets_sent;            /* Total packets sent */
    uint32_t packets_received;        /* Total packets received */
    unsigned long timestamp;          /* Collection timestamp */
} network_metrics_t;

/* Metrics history buffer */
typedef struct {
    process_metrics_t* process_history;
    stream_metrics_t* stream_history[4];  /* Per-channel stream metrics */
    system_metrics_t* system_history;
    network_metrics_t* network_history;
    int current_index;
    int max_size;
    pthread_mutex_t mutex;
} metrics_history_t;

/* Metrics module internal state */
typedef struct {
    /* Configuration */
    metrics_module_config_t config;
    bool initialized;
    bool running;

    /* Threading */
    pthread_t collection_thread;
    pthread_mutex_t mutex;
    volatile bool thread_should_exit;

    /* Metrics storage */
    metrics_history_t history;

    /* Current metrics */
    process_metrics_t current_process;
    stream_metrics_t current_streams[4];
    system_metrics_t current_system;
    network_metrics_t current_network;

    /* Statistics */
    unsigned long collections_count;
    unsigned long http_requests_count;
    unsigned long start_time;

    /* CPU calculation state */
    struct {
        unsigned long last_utime;
        unsigned long last_stime;
        unsigned long last_time_us;
        bool initialized;
    } cpu_state;
} metrics_module_state_t;

/* Metrics module functions */

/**
 * Initialize metrics module
 * @param config Metrics module configuration
 * @return 0 on success, -1 on error
 */
int metrics_module_init(void* config);

/**
 * Start metrics module
 * @return 0 on success, -1 on error
 */
int metrics_module_start(void);

/**
 * Stop metrics module
 * @return 0 on success, -1 on error
 */
int metrics_module_stop(void);

/**
 * Cleanup metrics module
 * @return 0 on success, -1 on error
 */
int metrics_module_cleanup(void);

/**
 * Parse metrics configuration from JSON
 * @param json JSON configuration object
 * @param config Metrics configuration structure to fill
 * @return 0 on success, -1 on error
 */
int metrics_module_config_parse(json_object* json, void* config);

/**
 * Validate metrics configuration
 * @param config Metrics configuration to validate
 * @return 0 if valid, -1 if invalid
 */
int metrics_module_config_validate(void* config);

/**
 * Free metrics configuration resources
 * @param config Metrics configuration to free
 */
void metrics_module_config_free(void* config);

/**
 * Get metrics module statistics
 * @param stats_buffer Buffer to fill with statistics
 * @param buffer_size Size of statistics buffer
 * @return 0 on success, -1 on error
 */
int metrics_module_get_stats(void* stats_buffer, size_t buffer_size);

/* HTTP endpoint handlers */

/**
 * Handle Prometheus metrics request
 * @param client_socket Client socket
 */
void metrics_handle_prometheus_request(int client_socket);

/**
 * Handle JSON status request
 * @param client_socket Client socket
 */
void metrics_handle_status_request(int client_socket);

/**
 * Handle JSON health request
 * @param client_socket Client socket
 */
void metrics_handle_health_request(int client_socket);

/**
 * Handle JSON info request
 * @param client_socket Client socket
 */
void metrics_handle_info_request(int client_socket);

/**
 * Handle JSON config request
 * @param client_socket Client socket
 */
void metrics_handle_config_request(int client_socket);

/* Utility functions */

/**
 * Get current process metrics
 * @param metrics Process metrics structure to fill
 * @return 0 on success, -1 on error
 */
int metrics_get_process_metrics(process_metrics_t* metrics);

/**
 * Get current stream metrics for a channel
 * @param channel Channel number
 * @param metrics Stream metrics structure to fill
 * @return 0 on success, -1 on error
 */
int metrics_get_stream_metrics(int channel, stream_metrics_t* metrics);

/**
 * Get current system metrics
 * @param metrics System metrics structure to fill
 * @return 0 on success, -1 on error
 */
int metrics_get_system_metrics(system_metrics_t* metrics);

/**
 * Check if metrics module is enabled and running
 * @return true if enabled and running, false otherwise
 */
bool metrics_module_is_enabled(void);

/**
 * Update stream metrics when a frame is processed
 * @param channel Channel number
 * @param frame_size Frame size in bytes
 * @param is_error Whether this frame had an error
 */
void metrics_update_stream_frame(int channel, unsigned int frame_size, bool is_error);

/**
 * Register HTTP routes for metrics module
 * @return 0 on success, -1 on error
 */
int metrics_register_routes(void);

#endif /* __METRICS_MODULE_H__ */
