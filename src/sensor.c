/*
 * sensor.c - Sensor Management Module
 * This file contains the sensor management module for Thingino Streamer
 * Copyright (C) 2025 Paul Philippov, Thingino Project
 */

#include <sys/stat.h>

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "config.h"
#include "sensor.h"

#define TAG "SENSOR"

/* Check if sensor proc filesystem is available */
int sensor_is_proc_available(void)
{
    struct stat st;
    return (stat(SENSOR_PROC_DIR, &st) == 0 && S_ISDIR(st.st_mode));
}

/* Read string value from proc file */
int sensor_read_proc_string(const char* filename, char* buffer, size_t buffer_size)
{
    char filepath[256];
    snprintf(filepath, sizeof(filepath), "%s/%s", SENSOR_PROC_DIR, filename);

    FILE* file = fopen(filepath, "r");
    if (!file) {
        IMP_LOG_ERR(TAG, "File %s not found", filepath);
        return -1;
    }

    if (fgets(buffer, buffer_size, file) == NULL) {
        fclose(file);
        return -1;
    }

    fclose(file);

    /* Trim whitespace */
    char* end = buffer + strlen(buffer) - 1;
    while (end > buffer && (*end == '\n' || *end == '\r' || *end == ' ' || *end == '\t')) {
        *end = '\0';
        end--;
    }

    IMP_LOG_DBG(TAG, "  %s: %s", filename, buffer);

    return 0;
}

/* Read integer value from proc file */
int sensor_read_proc_int(const char* filename, int default_value)
{
    char buffer[64];
    if (sensor_read_proc_string(filename, buffer, sizeof(buffer)) != 0) {
        IMP_LOG_ERR(TAG, "Using default value %d for %s", default_value, filename);
        return default_value;
    }

    char* endptr;
    int value = strtol(buffer, &endptr, 10);
    if (*endptr != '\0') {
        IMP_LOG_ERR(TAG,
                    "Failed to parse integer from %s ('%s'), using default %d",
                    filename,
                    buffer,
                    default_value);
        return default_value;
    }

    return value;
}

/* Convert hex string to unsigned int */
unsigned int sensor_read_proc_hex(const char* filename)
{
    char buffer[64];
    sensor_read_proc_string(filename, buffer, sizeof(buffer));

    char* endptr;
    unsigned int value = strtoul(buffer, &endptr, 16);
    if (*endptr != '\0') {
        IMP_LOG_ERR(TAG, "Failed to parse hex string '%s'", buffer);
        return 0;
    }

    return value;
}

/* Read comprehensive sensor information from /proc/jz/sensor/ */
int sensor_read_info_from_proc(sensor_info_t* sensor_info)
{
    IMP_LOG_DBG(TAG, "Getting sensor information from %s", SENSOR_PROC_DIR);

    if (!sensor_is_proc_available()) {
        IMP_LOG_ERR(TAG, "Sensor proc filesystem is not available at %s", SENSOR_PROC_DIR);
        return -1;
    }

    memset(sensor_info, 0, sizeof(sensor_info_t));
    sensor_info->i2c_bus = 0;
    sensor_info->boot = 0;
    sensor_info->mclk = 1;
    sensor_info->video_interface = 0;
    sensor_info->reset_gpio = 91;

    /* Read all sensor information from proc files */
    if (sensor_read_proc_string("name", sensor_info->name, sizeof(sensor_info->name)) != 0) {
        IMP_LOG_ERR(TAG, "Cannot read sensor name. Is sensor driver loaded?");
        return -1;
    }

    sensor_read_proc_string("chip_id", sensor_info->chip_id, sizeof(sensor_info->chip_id));
    sensor_read_proc_string("version", sensor_info->version, sizeof(sensor_info->version));

    /* Read I2C address */
    sensor_info->i2c_address = sensor_read_proc_hex("i2c_addr");

    /* Read dimensions and FPS */
    sensor_info->width = sensor_read_proc_int("width", 1920);
    sensor_info->height = sensor_read_proc_int("height", 1080);
    sensor_info->min_fps = sensor_read_proc_int("min_fps", 5);
    sensor_info->max_fps = sensor_read_proc_int("max_fps", 30);
    sensor_info->fps = sensor_info->max_fps;

    /* Read additional parameters */
    sensor_info->i2c_bus = sensor_read_proc_int("i2c_bus", 0);
    sensor_info->boot = sensor_read_proc_int("boot", 0);
    sensor_info->mclk = sensor_read_proc_int("mclk", 1);
    sensor_info->video_interface = sensor_read_proc_int("video_interface", 0);
    sensor_info->reset_gpio = sensor_read_proc_int("reset_gpio", 91);

    /* Validate required fields */
    if (strlen(sensor_info->name) == 0) {
        IMP_LOG_ERR(TAG, "Sensor name is empty");
        return -1;
    }

    if (sensor_info->width <= 0 || sensor_info->height <= 0) {
        IMP_LOG_ERR(TAG,
                    "Invalid sensor dimensions: %dx%d",
                    sensor_info->width,
                    sensor_info->height);
        return -1;
    }

    IMP_LOG_DBG(TAG,
                "Sensor info: %s (%dx%d@%dfps, i2c:0x%x)",
                sensor_info->name,
                sensor_info->width,
                sensor_info->height,
                sensor_info->max_fps,
                sensor_info->i2c_address);

    return 0;
}

/* Validate and adjust configuration based on sensor capabilities */
int sensor_validate_and_adjust_config(sensor_info_t* sensor_info)
{
    extern streamer_config_t* g_config;

    IMP_LOG_DBG(TAG, "Validating configuration against sensor capabilities");

    bool config_changed = false;

    /* Validate sensor FPS configuration */
    if (g_config->sensor.fps > sensor_info->max_fps) {
        IMP_LOG_WARN(TAG,
                     "Sensor FPS %d exceeds sensor max %d, adjusting to %d",
                     g_config->sensor.fps,
                     sensor_info->max_fps,
                     sensor_info->max_fps);
        g_config->sensor.fps = sensor_info->max_fps;
        config_changed = true;
    }

    if (g_config->sensor.fps < sensor_info->min_fps) {
        IMP_LOG_WARN(TAG,
                     "Sensor FPS %d below sensor min %d, adjusting to %d",
                     g_config->sensor.fps,
                     sensor_info->min_fps,
                     sensor_info->min_fps);
        g_config->sensor.fps = sensor_info->min_fps;
        config_changed = true;
    }

    /* Validate all stream resolutions */
    for (int i = 0; i < g_config->stream_count; i++) {
        stream_config_t* stream = &g_config->streams[i];

        if (stream->width > sensor_info->width) {
            IMP_LOG_ERR(TAG,
                        "stream[%d] width %d exceeds sensor max %d, adjusting",
                        i,
                        stream->width,
                        sensor_info->width);
            stream->width = sensor_info->width;
            config_changed = true;
        }

        if (stream->height > sensor_info->height) {
            IMP_LOG_ERR(TAG,
                        "stream[%d] height %d exceeds sensor max %d, adjusting",
                        i,
                        stream->height,
                        sensor_info->height);
            stream->height = sensor_info->height;
            config_changed = true;
        }
    }

    /* Update sensor info to match configuration */
    sensor_info->fps = g_config->sensor.fps;

    if (config_changed) {
        IMP_LOG_INFO(TAG, "Configuration adjusted based on sensor capabilities");
    }

    IMP_LOG_INFO(TAG, "Sensor configuration: %dx%d@%dfps",
                 sensor_info->width, sensor_info->height, sensor_info->fps);
    IMP_LOG_INFO(TAG, "All streams will use sensor FPS: %dfps", g_config->sensor.fps);

    return 0;
}
