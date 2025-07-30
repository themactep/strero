/*
 * common.h - Common Functions and Utilities
 * This file contains common functions and utilities for Thingino Streamer
 * Copyright (C) 2025 Paul Philippov, Thingino Project
 */

#ifndef __COMMON_H__
#define __COMMON_H__

#include "sensor.h"

#include <ifaddrs.h>
#include <netdb.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <imp/imp_common.h>
#include <imp/imp_encoder.h>
#include <imp/imp_framesource.h>
#include <imp/imp_isp.h>
#include <imp/imp_osd.h>
#include <imp/imp_system.h>
#include <netinet/in.h>
#include <sys/socket.h>

/* Override default logging to use direct printf calls to stdout with newlines */
#undef IMP_LOG_ERR
#define IMP_LOG_ERR(tag, fmt, ...)  do { \
    char ts[20]; \
    sprintf(ts, "%lu", get_monotonic_time_us()); \
    printf("[E %s] %s: " fmt "\n", ts, tag, ##__VA_ARGS__); fflush(stdout); \
} while (0)
#undef IMP_LOG_WARN
#define IMP_LOG_WARN(tag, fmt, ...) do { \
    char ts[20]; \
    sprintf(ts, "%lu", get_monotonic_time_us()); \
    printf("[W %s] %s: " fmt "\n", ts, tag, ##__VA_ARGS__); fflush(stdout); \
} while (0)
#undef IMP_LOG_INFO
#define IMP_LOG_INFO(tag, fmt, ...) do { \
    char ts[20]; \
    sprintf(ts, "%lu", get_monotonic_time_us()); \
    printf("[I %s] %s: " fmt "\n", ts, tag, ##__VA_ARGS__); fflush(stdout); \
} while (0)
#undef IMP_LOG_DBG
#define IMP_LOG_DBG(tag, fmt, ...)  do { \
    char ts[20]; \
    sprintf(ts, "%lu", get_monotonic_time_us()); \
    printf("[D %s] %s: " fmt "\n", ts, tag, ##__VA_ARGS__); fflush(stdout); \
} while (0)
#undef IMP_LOG_VERB
#define IMP_LOG_VERB(tag, fmt, ...) do { \
    char ts[20]; \
    sprintf(ts, "%lu", get_monotonic_time_us()); \
    printf("[V %s] %s: " fmt "\n", ts, tag, ##__VA_ARGS__); fflush(stdout); \
} while (0)

/* Shim function declarations to replace libalog.so dependencies */
int IMP_Log_Get_Option(void);
void IMP_Log_Set_Option(int op);
void imp_log_fun(int level, int option, int output, const char* tag,
                 const char* file, int line, const char* func,
                 const char* fmt, ...);

#define SENSOR_FRAME_RATE_NUM 30
#define SENSOR_FRAME_RATE_DEN 1

#define SENSOR_WIDTH 1920
#define SENSOR_HEIGHT 1080

#define CHN0_EN 1 // RTSP Main
#define CHN1_EN 0 // RTSP Sub
#define CHN2_EN 0
#define CHN3_EN 1 // JPEG
#define CROP_EN 1

#define SENSOR_WIDTH_SECOND 640
#define SENSOR_HEIGHT_SECOND 360

#define NR_FRAMES_TO_SAVE 200
#define STREAM_BUFFER_SIZE (1 * 1024 * 1024)

#define ENC_VIDEO_CHANNEL 0
#define ENC_JPEG_CHANNEL 1

#define STREAM_FILE_PATH_PREFIX "/tmp"
#define SNAP_FILE_PATH_PREFIX "/tmp"

/* Grafana metrics configuration */
#define GRAFANA_URL_MAX_LEN 256
#define GRAFANA_METRICS_INTERVAL 5000 /* 5 seconds in milliseconds */

/* Memory management for low-memory devices */
int is_low_memory_device(void);
int apply_low_memory_optimizations(void);
int setup_memory_pools_for_low_memory(void);

/* Metrics structure for each channel */
typedef struct {
    int channel;
    int frame_count;
    int error_count;
    double fps;
    double bitrate_kbps;
    unsigned int avg_frame_size;
    int64_t last_update_time;
    pthread_mutex_t mutex;
} channel_metrics_t;

#define SLEEP_TIME 1

#define FS_CHN_NUM 4 /* MIN 1, MAX 3 */
#define IVS_CHN_ID 3

#define CH0_INDEX 0
#define CH1_INDEX 1
#define CH2_INDEX 2
#define CH3_INDEX 3

#define CHN_ENABLE 1
#define CHN_DISABLE 0

#define SUPPORT_RGB555LE 1

struct chn_conf {
    unsigned int index;
    unsigned int enable;
    IMPEncoderProfile payloadType;
    IMPFSChnAttr fs_chn_attr;
    IMPCell framesource_chn;
    IMPCell imp_encoder;
    IMPCell osd_grp;
};

typedef struct {
    uint8_t* streamAddr;
    int streamLen;
} streamInfo;

typedef struct {
    IMPEncoderEncType type;
    IMPEncoderRcMode mode;
    uint16_t frameRate;
    uint16_t gopLength;
    uint32_t targetBitrate;
    uint32_t maxBitrate;
    int16_t initQp;
    int16_t minQp;
    int16_t maxQp;
    uint32_t maxPictureSize;
} encInfo;

#define CHN_NUM ARRAY_SIZE(chn)

extern IMPSensorInfo sensor_info;
extern sensor_info_t g_sensor_info;

/* Global initialization guard */
extern bool system_init_done;

/* Channel configuration array */
extern struct chn_conf chn[FS_CHN_NUM];

int jpeg_exit();

// int get_video_stream_byfd();
// int get_jpeg_snap();

int alcodec_encyuv_init(void** h, int picWidth, int picHight, void* info);
int alcodec_encyuv_encode(void* h, IMPFrameInfo frame, streamInfo* stream);
int alcodec_encyuv_deinit(void* h);

/* Configuration application functions */
int apply_config_to_channels(void);

/* System statistics functions */
int get_process_memory_usage(void);
int get_process_cpu_usage(void);
int get_process_thread_count(void);
int get_process_fps(void);
int get_process_client_count(void);

/* Network utilities */
int get_device_ip_address(char* ip_buffer, size_t buffer_size);

/* Timing utilities */
uint64_t get_monotonic_time_us(void);

/* HTTP utilities */
int safe_send(int socket, const void* data, size_t len);
extern bool http_server_running;

/* Channel metrics functions - always available for status endpoint compatibility */
int metrics_init_endpoint(void);
void metrics_cleanup_endpoint(void);
extern channel_metrics_t channel_metrics[FS_CHN_NUM];

#endif /* __COMMON_H__ */
