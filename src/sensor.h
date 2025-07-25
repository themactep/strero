/*
 * sensor.h - Sensor Management Module
 * This file contains the sensor management module for Thingino Streamer
 * Copyright (C) 2025 Paul Philippov, Thingino Project
 */

#ifndef __SENSOR_H__
#define __SENSOR_H__

#include <stdbool.h>
#include <stddef.h>

/* Forward declaration for config structure */
struct streamer_config;

/* Sensor information structure */
typedef struct {
    char name[64];
    char chip_id[32];
    char i2c_addr[16];
    char version[64];
    int width;
    int height;
    int min_fps;
    int max_fps;
    int i2c_bus;
    int boot;
    int mclk;
    int video_interface;
    int reset_gpio;
    unsigned int i2c_address;
    int fps;
} sensor_info_t;

/* Sensor proc filesystem path */
#define SENSOR_PROC_DIR "/proc/jz/sensor"

/* Function declarations */

/**
 * Check if sensor proc filesystem is available
 * @return 1 if available, 0 if not
 */
int sensor_is_proc_available(void);

/**
 * Read string value from sensor proc file
 * @param filename Name of the proc file to read
 * @param buffer Buffer to store the result
 * @param buffer_size Size of the buffer
 * @return 0 on success, -1 on error
 */
int sensor_read_proc_string(const char* filename, char* buffer, size_t buffer_size);

/**
 * Read integer value from sensor proc file
 * @param filename Name of the proc file to read
 * @param default_value Default value to return if reading fails
 * @return The read value or default_value on error
 */
int sensor_read_proc_int(const char* filename, int default_value);

/**
 * Parse hex value from sensor proc file to unsigned integer
 * @param filename Name of the proc file to read
 * @return The read value or -1 on error
 */
unsigned int sensor_read_proc_hex(const char* filename);

/**
 * Read comprehensive sensor information from /proc/jz/sensor/
 * @param sensor_info Pointer to sensor_info_t structure to fill
 * @return 0 on success, -1 on error
 */
int sensor_read_info_from_proc(sensor_info_t* sensor_info);

/**
 * Validate and adjust configuration based on sensor capabilities
 * @param sensor_info Sensor information structure
 * @return 0 on success, -1 on error
 */
int sensor_validate_and_adjust_config(sensor_info_t* sensor_info);

#endif /* __SENSOR_H__ */
