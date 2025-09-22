/*
 * main.c - Streamer Main Function
 * This file contains the main function for Thingino Streamer
 * Copyright (C) 2025 Paul Philippov, Thingino Project
 */

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sys/file.h> /* For flock() */
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>

/* Lock file globals */
static int byGetFd = 0;
static int g_lock_fd = -1;
static const char* g_lock_file = "/run/streamer.lock";
static int g_soft_ps_running = 1;

/* IMP headers */
#include <imp/imp_audio.h>
#include <imp/imp_common.h>
#if defined(PLATFORM_T31)
#include <imp/imp_dmic.h>
#endif
#include <imp/imp_encoder.h>
#include <imp/imp_framesource.h>
#include <imp/imp_isp.h>
#include <imp/imp_ivs.h>
#include <imp/imp_ivs_base_move.h>

#include <imp/imp_system.h>
#include <imp/imp_utils.h>

/* Local headers */
#include "common.h"
#include "config.h"
#include "frame_manager.h"
#include "snapshot_fallback.h"
#ifdef ENABLE_RTSP
#include "modules/rtsp/rtsp_module.h"
#endif
#ifdef ENABLE_RTMP_CLIENT
#include "modules/rtmp_client/rtmp_client.h"
#include "modules/rtmp_client/rtmp_consumer.h"
#endif
#include "module_system.h"

#ifdef ENABLE_IMP_CONTROL
#include "modules/imp_control/imp_control.h"
#endif

#ifdef ENABLE_METRICS
#include "modules/metrics/metrics_module.h"
#endif

#ifdef ENABLE_OSD
#include <imp/imp_osd.h>
#include "modules/osd/osd_module.h"
#endif

#define TAG "main"

extern struct chn_conf chn[];

/* Using native IMP encoder buffering - no async queues needed */

bool check_another_instance_running();
void cleanup_lock_file(void);

static const IMPEncoderRcMode S_RC_METHOD = ENC_RC_MODE_CBR;

/* Cleanup lock file on program exit */
void cleanup_lock_file(void)
{
    if (g_lock_fd >= 0) {
        close(g_lock_fd);
        g_lock_fd = -1;
        unlink(g_lock_file); /* Remove the lock file */
        IMP_LOG_DBG(TAG, "Lock file cleaned up");
    }
}

/* Check if another instance of streamer is already running */
bool check_another_instance_running()
{
    int fd;
    struct flock fl;
    char pid_str[16];

    /* Open or create the lock file */
    fd = open(g_lock_file, O_RDWR | O_CREAT, 0644);
    if (fd < 0) {
        IMP_LOG_ERR(TAG, "Could not open or create lock file: %s", strerror(errno));
        return true; /* Assume another instance is running if we can't create the file */
    }

    /* Set up the flock structure for a write lock */
    fl.l_type = F_WRLCK;    /* Write lock */
    fl.l_whence = SEEK_SET; /* From beginning of file */
    fl.l_start = 0;         /* Start at beginning */
    fl.l_len = 0;           /* Lock entire file */
    fl.l_pid = getpid();    /* Current process ID */

    /* Try to get an exclusive lock (non-blocking) */
    if (fcntl(fd, F_SETLK, &fl) == -1) {
        if (errno == EACCES || errno == EAGAIN) {
            /* File is locked - another instance is running */
            IMP_LOG_DBG(TAG, "Lock file is locked by another process");
            close(fd);
            return true;
        }

        /* Some other error occurred */
        IMP_LOG_ERR(TAG, "Could not lock file: %s", strerror(errno));
        close(fd);
        return true; /* Assume another instance is running */
    }

    /* Truncate the file and write our PID */
    if (ftruncate(fd, 0) == -1) {
        IMP_LOG_ERR(TAG, "Could not truncate lock file: %s", strerror(errno));
        /* Continue anyway, not critical */
    }

    /* Write PID to the lock file */
    snprintf(pid_str, sizeof(pid_str), "%d\n", getpid());
    if (write(fd, pid_str, strlen(pid_str)) != (ssize_t) strlen(pid_str)) {
        IMP_LOG_ERR(TAG, "Could not write PID to lock file: %s", strerror(errno));
        /* Continue anyway, not critical */
    }

    /* Keep the file descriptor open so the lock is maintained
     * The lock will be automatically released when the process exits */

    /* Store the file descriptor globally so we can clean it up properly */
    g_lock_fd = fd;

    /* Register a cleanup function to be called on normal program termination */
    atexit(cleanup_lock_file);

    return false; /* No other instance is running */
}

int jpeg_init_channel(int jpeg_channel, int stream_index, int width, int height)
{
    int ret = 0;
    IMPFSChnAttr* imp_chn_attr_tmp;

    /* Use the frame source channel attributes from the corresponding video stream */
    imp_chn_attr_tmp = &chn[stream_index].fs_chn_attr;

#if defined(PLATFORM_T23) || defined(PLATFORM_T20)
    IMPEncoderCHNAttr channel_attr;
    memset(&channel_attr, 0, sizeof(IMPEncoderCHNAttr));
    channel_attr.encAttr.enType = PT_JPEG;
    channel_attr.encAttr.bufSize = 0; /* auto */
    channel_attr.encAttr.profile = 0; /* baseline (ignored for JPEG) */
    channel_attr.encAttr.picWidth = width;
    channel_attr.encAttr.picHeight = height;
    channel_attr.bEnableIvdc = false;
    channel_attr.rcAttr.outFrmRate.frmRateNum = imp_chn_attr_tmp->outFrmRateNum;
    channel_attr.rcAttr.outFrmRate.frmRateDen = imp_chn_attr_tmp->outFrmRateDen;
    channel_attr.rcAttr.maxGop = imp_chn_attr_tmp->outFrmRateNum;
    channel_attr.rcAttr.attrRcMode.rcMode = ENC_RC_MODE_FIXQP;
#else
    IMPEncoderChnAttr channel_attr;
    memset(&channel_attr, 0, sizeof(IMPEncoderChnAttr));
    ret = IMP_Encoder_SetDefaultParam(&channel_attr,
                                      IMP_ENC_PROFILE_JPEG,
                                      IMP_ENC_RC_MODE_FIXQP,
                                      width,
                                      height,
                                      imp_chn_attr_tmp->outFrmRateNum,
                                      imp_chn_attr_tmp->outFrmRateDen,
                                      0,
                                      0,
                                      25,
                                      0);
    if (ret < 0) {
        IMP_LOG_ERR(TAG, "IMP_Encoder_SetDefaultParam(%d) error: %d", jpeg_channel, ret);
        return -1;
    }
    IMP_LOG_DBG(TAG, "Set default JPEG parameters for channel %d: %dx%d", jpeg_channel, width, height);
#endif

    /* Create Channel */
    ret = IMP_Encoder_CreateChn(jpeg_channel, &channel_attr);
    if (ret < 0) {
        IMP_LOG_ERR(TAG, "IMP_Encoder_CreateChn(%d) error: %d", jpeg_channel, ret);
        return -1;
    }
    IMP_LOG_DBG(TAG, "Created JPEG channel %d", jpeg_channel);

    /* Register Channel to the corresponding stream group */
    ret = IMP_Encoder_RegisterChn(stream_index, jpeg_channel);
    if (ret < 0) {
        IMP_LOG_ERR(TAG, "IMP_Encoder_RegisterChn(%d, %d) error: %d", stream_index, jpeg_channel, ret);
        return -1;
    }
    IMP_LOG_DBG(TAG, "Registered JPEG channel %d to group %d", jpeg_channel, stream_index);

    return 0;
}

static int stream_start_channel(int channel)
{
    IMP_LOG_DBG(TAG, "Starting stream on channel %d", channel);

    /* Start encoder receiving pictures */
    int ret = IMP_Encoder_StartRecvPic(channel);
    if (ret < 0) {
        IMP_LOG_ERR(TAG, "IMP_Encoder_StartRecvPic(%d) failed", channel);
        return -1;
    }

    IMP_LOG_DBG(TAG, "Stream started successfully on channel %d", channel);
    return 0;
}

static int stream_stop_channel(int channel)
{
    IMP_LOG_DBG(TAG, "Stopping stream on channel %d", channel);

    /* Stop encoder receiving pictures */
    int ret = IMP_Encoder_StopRecvPic(channel);
    if (ret < 0) {
        IMP_LOG_ERR(TAG, "IMP_Encoder_StopRecvPic(%d) failed", channel);
        return -1;
    }

    IMP_LOG_DBG(TAG, "Stream stopped successfully on channel %d", channel);
    return 0;
}

int main(int argc, char *argv[])
{
    int i, ret;

    /* Check if another instance of streamer is already running */
    if (check_another_instance_running()) {
        IMP_LOG_ERR(TAG, "Another instance of streamer is already running");
        return -1;
    }
    IMP_LOG_DBG(TAG, "Starting streamer - PID: %d", getpid());

    /* Initialize channel metrics - needed for status endpoint compatibility */
    ret = metrics_init_endpoint();
    if (ret < 0) {
        IMP_LOG_WARN(TAG, "Failed to initialize channel metrics");
    }
#ifdef ENABLE_METRICS
    IMP_LOG_INFO(TAG, "Channel metrics initialized (metrics module handles process metrics)");
#else
    IMP_LOG_INFO(TAG, "Channel metrics initialized (legacy mode)");
#endif

    /* Initialize and load streamer configuration FIRST */
    if (config_init_and_load() != 0) {
        IMP_LOG_ERR(TAG, "Failed to initialize configuration");
        return -1;
    }
    IMP_LOG_DBG(TAG, "Streamer configuration loaded successfully");

    /* Initialize module system */
    if (module_system_init() != 0) {
        IMP_LOG_ERR(TAG, "Failed to initialize module system");
        return -1;
    }
    IMP_LOG_INFO(TAG, "Module system initialized");

    /* Manually register modules (constructor-based registration gets stripped) */
#ifdef ENABLE_AUDIO
    extern module_info_t audio_module_info;
    if (module_register(&audio_module_info) != 0) {
        IMP_LOG_WARN(TAG, "Failed to register audio module");
    } else {
        IMP_LOG_INFO(TAG, "Audio module registered manually");
    }
#endif

#ifdef ENABLE_ONVIF
    extern module_info_t onvif_module_info;
    if (module_register(&onvif_module_info) != 0) {
        IMP_LOG_WARN(TAG, "Failed to register ONVIF module");
    } else {
        IMP_LOG_INFO(TAG, "ONVIF module registered manually");
    }
#endif

#ifdef ENABLE_PHOTOSENSING
    extern module_info_t photosensing_module_info;
    if (module_register(&photosensing_module_info) != 0) {
        IMP_LOG_WARN(TAG, "Failed to register photosensing module");
    } else {
        IMP_LOG_INFO(TAG, "Photosensing module registered manually");
    }
#endif

#ifdef ENABLE_METRICS
    extern module_info_t metrics_module_info;
    if (module_register(&metrics_module_info) != 0) {
        IMP_LOG_WARN(TAG, "Failed to register metrics module");
    } else {
        IMP_LOG_INFO(TAG, "Metrics module registered manually");
    }
#endif

#ifdef ENABLE_MOTION
    extern module_info_t motion_module_info;
    if (module_register(&motion_module_info) != 0) {
        IMP_LOG_WARN(TAG, "Failed to register motion detection module");
    } else {
        IMP_LOG_INFO(TAG, "Motion detection module registered manually");
    }
#endif

#ifdef ENABLE_OSD
    if (register_osd_module() != 0) {
        IMP_LOG_WARN(TAG, "Failed to register OSD module");
    } else {
        IMP_LOG_INFO(TAG, "OSD module registered manually");
    }
#endif

#ifdef ENABLE_HTTP
    extern module_info_t http_module_info;
    if (module_register(&http_module_info) != 0) {
        IMP_LOG_WARN(TAG, "Failed to register HTTP module");
    } else {
        IMP_LOG_INFO(TAG, "HTTP module registered manually");
    }
#endif



#ifdef ENABLE_IMP_CONTROL
    extern module_info_t imp_control_info;
    if (module_register(&imp_control_info) != 0) {
        IMP_LOG_WARN(TAG, "Failed to register IMP control module");
    } else {
        IMP_LOG_INFO(TAG, "IMP control module registered manually");
    }
#endif

#ifdef ENABLE_RTMP
    extern module_info_t rtmp_module_info;
    if (module_register(&rtmp_module_info) != 0) {
        IMP_LOG_WARN(TAG, "Failed to register RTMP module");
    } else {
        IMP_LOG_INFO(TAG, "RTMP module registered manually");
    }
#endif

    /* Initialize all registered modules with global config */
    if (module_init_all(g_config->json_config) != 0) {
        IMP_LOG_WARN(TAG, "Some modules failed to initialize, continuing");
    }
    IMP_LOG_INFO(TAG, "Modules initialized");

    /* ONVIF discovery is now handled by the ONVIF module */

    /* Read sensor info from driver export in /proc/ */
    if (sensor_read_info_from_proc(&g_sensor_info) != 0) {
        IMP_LOG_DBG(TAG, "Failed to read sensor info from /proc/");
        return -1;
    }
    IMP_LOG_DBG(TAG, "Sensor info read successfully");

    /* Validate and adjust configuration based on sensor capabilities */
    if (sensor_validate_and_adjust_config(&g_sensor_info) != 0) {
        IMP_LOG_DBG(TAG, "Configuration validation failed");
        return -1;
    }
    IMP_LOG_DBG(TAG, "Configuration validated and adjusted based on sensor capabilities");

    /* Note: Memory optimizations will be applied AFTER JSON config to avoid being overridden */

#ifdef ENABLE_OSD
    /* Use smaller OSD pool for low-memory devices */
    int osd_pool_size = is_low_memory_device() ? 512 * 1024 : 4096 * 1024;
    ret = IMP_OSD_SetPoolSize(osd_pool_size);
    if (ret < 0) {
        IMP_LOG_WARN(TAG, "IMP_OSD_SetPoolSize failed, continuing with default");
    } else {
        IMP_LOG_INFO(TAG, "OSD memory pool set to %dKB for memory optimization", osd_pool_size / 1024);
    }
#endif

    memset(&sensor_info, 0, sizeof(IMPSensorInfo));
    strncpy(sensor_info.name, g_sensor_info.name, sizeof(sensor_info.name) - 1);
    sensor_info.cbus_type = TX_SENSOR_CONTROL_INTERFACE_I2C;
    strncpy(sensor_info.i2c.type, g_sensor_info.name, sizeof(sensor_info.i2c.type) - 1);
    sensor_info.i2c.addr = g_sensor_info.i2c_address;

    IMP_LOG_DBG(TAG, "System init start");

    IMP_LOG_INFO(TAG, "Opening ISP...");
    ret = IMP_ISP_Open();
    if (ret < 0) {
        IMP_LOG_ERR(TAG, "failed to open ISP: %d", ret);
        return -1;
    }
    IMP_LOG_INFO(TAG, "ISP opened successfully");

    IMP_LOG_INFO(TAG, "Adding sensor...");
    ret = IMP_ISP_AddSensor(&sensor_info);
    if (ret < 0) {
        IMP_LOG_ERR(TAG, "failed to AddSensor: %d", ret);
        return -1;
    }
    IMP_LOG_INFO(TAG, "Sensor added successfully");

    IMP_LOG_INFO(TAG, "Enabling sensor...");
    ret = IMP_ISP_EnableSensor();
    if (ret < 0) {
        IMP_LOG_ERR(TAG, "failed to EnableSensor: %d", ret);
        return -1;
    }
    IMP_LOG_INFO(TAG, "Sensor enabled successfully");

    /* Set up memory pools for low-memory devices BEFORE system init */
    setup_memory_pools_for_low_memory();

    /* Initialize system after ISP setup */
    IMP_LOG_INFO(TAG, "Calling IMP_System_Init()...");
    ret = IMP_System_Init();
    IMP_LOG_INFO(TAG, "IMP_System_Init() returned: %d", ret);
    if (ret < 0) {
        IMP_LOG_ERR(TAG, "IMP_System_Init failed with error: %d", ret);
        return -1;
    }
    IMP_LOG_INFO(TAG, "System initialized successfully");

    ret = IMP_ISP_EnableTuning();
    if (ret < 0) {
        IMP_LOG_ERR(TAG, "IMP_ISP_EnableTuning failed");
        return -1;
    }
    IMP_LOG_DBG(TAG, "ISP tuning enabled successfully");

    ret = IMP_ISP_Tuning_SetContrast(128);
    if (ret < 0) {
        IMP_LOG_ERR(TAG, "failed to set contrast");
        return -1;
    }
    IMP_LOG_DBG(TAG, "ISP tuning contrast set to 128");

    ret = IMP_ISP_Tuning_SetSharpness(128);
    if (ret < 0) {
        IMP_LOG_ERR(TAG, "failed to set sharpness");
        return -1;
    }
    IMP_LOG_DBG(TAG, "ISP tuning sharpness set to 128");

    ret = IMP_ISP_Tuning_SetSaturation(128);
    if (ret < 0) {
        IMP_LOG_ERR(TAG, "failed to set saturation");
        return -1;
    }
    IMP_LOG_DBG(TAG, "ISP tuning saturation set to 128");

    ret = IMP_ISP_Tuning_SetBrightness(128);
    if (ret < 0) {
        IMP_LOG_ERR(TAG, "failed to set brightness");
        return -1;
    }
    IMP_LOG_DBG(TAG, "ISP tuning brightness set to 128");

    ret = IMP_ISP_Tuning_SetISPRunningMode(IMPISP_RUNNING_MODE_DAY);
    if (ret < 0) {
        IMP_LOG_ERR(TAG, "failed to set running mode");
        return -1;
    }
    IMP_LOG_DBG(TAG, "ISP running mode set to DAY");

    /* Set sensor FPS from configuration */
    int sensor_fps = g_config ? g_config->sensor.fps : SENSOR_FRAME_RATE_NUM;

    ret = IMP_ISP_Tuning_SetSensorFPS(sensor_fps, 1);
    if (ret < 0){
        IMP_LOG_ERR(TAG, "failed to set sensor fps to %d", sensor_fps);
        return -1;
    }
    IMP_LOG_INFO(TAG, "Sensor FPS set to %d/1 (all channels use sensor FPS)", sensor_fps);

    /* Verify sensor FPS was set correctly */
    uint32_t actual_fps_num, actual_fps_den;
    ret = IMP_ISP_Tuning_GetSensorFPS(&actual_fps_num, &actual_fps_den);
    if (ret == 0) {
        IMP_LOG_INFO(TAG, "Verified sensor FPS: %d/%d (%.2f fps)",
                   actual_fps_num, actual_fps_den, (float)actual_fps_num / actual_fps_den);
    } else {
        IMP_LOG_WARN(TAG, "Failed to verify sensor FPS setting");
    }

    /* FrameSource init */

    /* Apply JSON configuration to channels before initialization */
    ret = apply_config_to_channels();
    if (ret < 0) {
        IMP_LOG_ERR(TAG, "Failed to apply configuration to channels");
        return -1;
    }
    IMP_LOG_DBG(TAG, "Configuration applied to channels successfully");

    /* Apply memory optimizations AFTER JSON config to ensure they take effect */
    apply_low_memory_optimizations();

    for (i = 0; i < FS_CHN_NUM; i++) {
        if (chn[i].enable) {
            ret = IMP_FrameSource_CreateChn(chn[i].index, &chn[i].fs_chn_attr);
            if (ret < 0) {
                IMP_LOG_ERR(TAG, "FrameSource channel %d creation failed: %d", chn[i].index, ret);
                return -1;
            }
            IMP_LOG_DBG(TAG, "FrameSource channel %d created", chn[i].index);

            int buffer_depth = chn[i].fs_chn_attr.nrVBs; /* Default from static config */
            if (i < g_config->stream_count && g_config->streams[i].framesource.buffer_count > 0) {
                /* Override with dynamic config if available */
                buffer_depth = g_config->streams[i].framesource.buffer_count;
                IMP_LOG_DBG(TAG, "Using dynamic buffer depth %d for channel %d", buffer_depth, chn[i].index);
            }

            /* Set FIFO buffer depth for proper frame buffering - CRITICAL for preventing buffer exhaustion */
            IMPFSChnFifoAttr fifo_attr;
            /* For 64MB devices, FIFO depth MUST be 0 to avoid buffer multiplication */
            if (is_low_memory_device()) {
                fifo_attr.maxdepth = 0;  /* MUST be 0 for 64MB devices - any other value causes num_buffers:2 error */
                IMP_LOG_WARN(TAG, "64MB device: Setting FIFO depth to 0");
            } else {
                fifo_attr.maxdepth = buffer_depth; /* Use configured buffer depth */
            }
            fifo_attr.type = FIFO_CACHE_PRIORITY;  /* Use cache priority for all channels - safer approach */
            IMP_LOG_DBG(TAG, "Using buffer depth %d for channel %d", buffer_depth, chn[i].index);
            ret = IMP_FrameSource_SetChnFifoAttr(chn[i].index, &fifo_attr);
            if (ret < 0) {
                IMP_LOG_ERR(TAG, "IMP_FrameSource_SetChnFifoAttr(%d) failed: %d", chn[i].index, ret);
                return -1;
            }
            IMP_LOG_INFO(TAG, "FrameSource channel %d: FIFO buffer depth set to %d frames",
                         chn[i].index, fifo_attr.maxdepth);

            ret = IMP_FrameSource_SetChnAttr(chn[i].index, &chn[i].fs_chn_attr);
            if (ret < 0) {
                IMP_LOG_ERR(TAG, "FrameSource channel %d attribute setting failed: %d", chn[i].index, ret);
                return -1;
            }
            IMP_LOG_DBG(TAG, "FrameSource channel %d attributes set", chn[i].index);
        } else {
            IMP_LOG_DBG(TAG, "FrameSource channel %d not enabled", chn[i].index);
        }
    }
    IMP_LOG_DBG(TAG, "FrameSource initialized successfully");

    for (i = 0; i < FS_CHN_NUM; i++) {
        if (chn[i].enable) {
            ret = IMP_Encoder_CreateGroup(chn[i].index);
            if (ret < 0) {
                IMP_LOG_ERR(TAG, "IMP_Encoder_CreateGroup(%d) error!", i);
                return -1;
            }
            IMP_LOG_INFO(TAG, "Encoder group %d created successfully", i);
        } else {
            IMP_LOG_INFO(TAG, "Encoder group %d not enabled", i);
        }
    }

    /* Encoder init */
    IMPFSChnAttr* imp_chn_attr_tmp;
#if defined(PLATFORM_T23) || defined(PLATFORM_T20)
    IMPEncoderCHNAttr channel_attr;
#else
    IMPEncoderChnAttr channel_attr;
#endif
    int chnNum = 0;

    for (i = 0; i < FS_CHN_NUM; i++) {
        if (chn[i].enable) {
            chnNum = chn[i].index;
            imp_chn_attr_tmp = &chn[i].fs_chn_attr;
#if defined(PLATFORM_T23) || defined(PLATFORM_T20)
            memset(&channel_attr, 0, sizeof(IMPEncoderCHNAttr));
#else
            memset(&channel_attr, 0, sizeof(IMPEncoderChnAttr));
#endif

            /* Optimize stream buffer count for 64MB performance vs memory balance */
            int stream_buffer_count = is_low_memory_device() ? 2 : 6;
            ret = IMP_Encoder_SetMaxStreamCnt(chnNum, stream_buffer_count);
            if (ret < 0) {
                IMP_LOG_ERR(TAG, "IMP_Encoder_SetMaxStreamCnt(%d, %d) failed: %d",
                           chnNum, stream_buffer_count, ret);
                return -1;
            }
            IMP_LOG_INFO(TAG, "Channel %d: Set %d stream buffers for %s device",
                         chnNum, stream_buffer_count,
                         is_low_memory_device() ? "low-memory" : "normal");

            /* Reduce stream buffer size for low-memory devices */
            uint32_t stream_buf_size = is_low_memory_device() ? 256 * 1024 : 512 * 1024;
#if !(defined(PLATFORM_T23) || defined(PLATFORM_T20))
            ret = IMP_Encoder_SetStreamBufSize(chnNum, stream_buf_size);
            if (ret < 0) {
                IMP_LOG_ERR(TAG, "IMP_Encoder_SetStreamBufSize(%d, %u) failed: %d",
                           chnNum, stream_buf_size, ret);
                return -1;
            }
            IMP_LOG_INFO(TAG, "Channel %d: Set %dKB stream buffer size for %s device",
                         chnNum, stream_buf_size / 1024,
                         is_low_memory_device() ? "low-memory" : "normal");
#endif

            IMP_LOG_INFO(TAG, "Channel %d: Set %d stream buffers, %u bytes each", chnNum, stream_buffer_count, stream_buf_size);

            /* Use pre-calculated values for specific resolutions */
            unsigned int uTargetBitRate;
            int width = imp_chn_attr_tmp->picWidth;
            int height = imp_chn_attr_tmp->picHeight;

            if (width == 1920 && height == 1080) {
                uTargetBitRate = 2000;
            } else if (width == 1280 && height == 720) {
                uTargetBitRate = 1600;
            } else {
                float ratio = ((float)(width * height)) / (1280.0 * 720.0);
                if (ratio < 0.1) ratio = 0.1;
                if (ratio > 3.0) ratio = 3.0;
                uTargetBitRate = 2000 * ratio;
            }

            extern struct streamer_config* g_config;
            if (!g_config) {
                IMP_LOG_ERR(TAG, "Global configuration not available for encoder setup");
                return -1;
            }

            /* Use sensor FPS for encoder */
            uint32_t target_fps_num = g_config->sensor.fps;
            uint32_t target_fps_den = 1;
            uint32_t gop_length = g_config->sensor.fps; /* 1 second keyframe interval for streaming */

            /* Set CABAC entropy mode for Main Profile (better compression for RTMP) */
#if !(defined(PLATFORM_T23) || defined(PLATFORM_T20))
            IMPEncoderEncType enc_type = (chn[i].payloadType >> 24);
            if (enc_type == IMP_ENC_TYPE_AVC) {
                ret = IMP_Encoder_SetChnEntropyMode(chnNum, IMP_ENC_ENTROPY_MODE_CABAC);
                if (ret < 0) {
                    IMP_LOG_WARN(TAG, "Failed to set CABAC entropy mode for channel %d, using default", chnNum);
                }
            }
#endif

#if defined(PLATFORM_T23) || defined(PLATFORM_T20)
            /* Build channel_attr manually for T23 */
            channel_attr.encAttr.enType = chn[i].payloadType;
            channel_attr.encAttr.bufSize = 0; /* auto */
            channel_attr.encAttr.profile = (chn[i].payloadType == PT_H264) ? 1 : 0; /* MP for H.264, 0 otherwise */
            channel_attr.encAttr.picWidth = imp_chn_attr_tmp->picWidth;
            channel_attr.encAttr.picHeight = imp_chn_attr_tmp->picHeight;
            channel_attr.rcAttr.outFrmRate.frmRateNum = target_fps_num;
            channel_attr.rcAttr.outFrmRate.frmRateDen = target_fps_den;
            channel_attr.rcAttr.maxGop = gop_length;
            channel_attr.rcAttr.attrRcMode.rcMode = S_RC_METHOD;
#else
            ret = IMP_Encoder_SetDefaultParam(&channel_attr,
                                              chn[i].payloadType,
                                              S_RC_METHOD,
                                              imp_chn_attr_tmp->picWidth,
                                              imp_chn_attr_tmp->picHeight,
                                              target_fps_num,
                                              target_fps_den,
                                              gop_length,
                                              2,
                                              (S_RC_METHOD == IMP_ENC_RC_MODE_FIXQP) ? 25 : -1, /* Lower QP = better quality */
                                              uTargetBitRate);
            if (ret < 0) {
                IMP_LOG_ERR(TAG, "IMP_Encoder_SetDefaultParam(%d) error!", chnNum);
                return -1;
            }
#endif

            /* Log encoder configuration for debugging */
            IMP_LOG_INFO(TAG, "Encoder channel %d configured: %dx%d@%d/%dfps, GOP=%d, bitrate=%dkbps, mode=%s",
                        chnNum, imp_chn_attr_tmp->picWidth, imp_chn_attr_tmp->picHeight,
                        target_fps_num, target_fps_den, gop_length, uTargetBitRate,
#if defined(PLATFORM_T23) || defined(PLATFORM_T20)
                        (S_RC_METHOD == ENC_RC_MODE_CBR) ? "CBR" :
                        (S_RC_METHOD == ENC_RC_MODE_VBR) ? "VBR" : "FIXQP"
#else
                        (S_RC_METHOD == IMP_ENC_RC_MODE_CBR) ? "CBR" :
                        (S_RC_METHOD == IMP_ENC_RC_MODE_VBR) ? "VBR" : "FIXQP"
#endif
                        );
#ifdef LOW_BITSTREAM
            IMPEncoderRcAttr* rcAttr = &channel_attr.rcAttr;
            uTargetBitRate /= 2;

            switch (rcAttr->attrRcMode.rcMode) {
            case IMP_ENC_RC_MODE_FIXQP:
                rcAttr->attrRcMode.attrFixQp.iInitialQP = 38;
                break;
            case IMP_ENC_RC_MODE_CBR:
                rcAttr->attrRcMode.attrCbr.uTargetBitRate = uTargetBitRate;
                rcAttr->attrRcMode.attrCbr.iInitialQP = -1;
                rcAttr->attrRcMode.attrCbr.iMinQP = 34;
                rcAttr->attrRcMode.attrCbr.iMaxQP = 51;
                rcAttr->attrRcMode.attrCbr.iIPDelta = -1;
                rcAttr->attrRcMode.attrCbr.iPBDelta = -1;
                rcAttr->attrRcMode.attrCbr.eRcOptions = IMP_ENC_RC_SCN_CHG_RES | IMP_ENC_RC_OPT_SC_PREVENTION;
                rcAttr->attrRcMode.attrCbr.uMaxPictureSize = uTargetBitRate / 2; /* Allow larger keyframes for 1080p */
                break;
            case IMP_ENC_RC_MODE_VBR:
                rcAttr->attrRcMode.attrVbr.uTargetBitRate = uTargetBitRate;
                rcAttr->attrRcMode.attrVbr.uMaxBitRate = uTargetBitRate * 4 / 3;
                rcAttr->attrRcMode.attrVbr.iInitialQP = -1;
                rcAttr->attrRcMode.attrVbr.iMinQP = 34;
                rcAttr->attrRcMode.attrVbr.iMaxQP = 51;
                rcAttr->attrRcMode.attrVbr.iIPDelta = -1;
                rcAttr->attrRcMode.attrVbr.iPBDelta = -1;
                rcAttr->attrRcMode.attrVbr.eRcOptions = IMP_ENC_RC_SCN_CHG_RES | IMP_ENC_RC_OPT_SC_PREVENTION;
                rcAttr->attrRcMode.attrVbr.uMaxPictureSize = uTargetBitRate * 4 / 3;
                break;
            case IMP_ENC_RC_MODE_CAPPED_VBR:
                rcAttr->attrRcMode.attrCappedVbr.uTargetBitRate = uTargetBitRate;
                rcAttr->attrRcMode.attrCappedVbr.uMaxBitRate = uTargetBitRate * 4 / 3;
                rcAttr->attrRcMode.attrCappedVbr.iInitialQP = -1;
                rcAttr->attrRcMode.attrCappedVbr.iMinQP = 34;
                rcAttr->attrRcMode.attrCappedVbr.iMaxQP = 51;
                rcAttr->attrRcMode.attrCappedVbr.iIPDelta = -1;
                rcAttr->attrRcMode.attrCappedVbr.iPBDelta = -1;
                rcAttr->attrRcMode.attrCappedVbr.eRcOptions = IMP_ENC_RC_SCN_CHG_RES | IMP_ENC_RC_OPT_SC_PREVENTION;
                rcAttr->attrRcMode.attrCappedVbr.uMaxPictureSize = uTargetBitRate * 4 / 3;
                rcAttr->attrRcMode.attrCappedVbr.uMaxPSNR = 42;
                break;
            case IMP_ENC_RC_MODE_CAPPED_QUALITY:
                rcAttr->attrRcMode.attrCappedQuality.uTargetBitRate = uTargetBitRate;
                rcAttr->attrRcMode.attrCappedQuality.uMaxBitRate = uTargetBitRate * 4 / 3;
                rcAttr->attrRcMode.attrCappedQuality.iInitialQP = -1;
                rcAttr->attrRcMode.attrCappedQuality.iMinQP = 34;
                rcAttr->attrRcMode.attrCappedQuality.iMaxQP = 51;
                rcAttr->attrRcMode.attrCappedQuality.iIPDelta = -1;
                rcAttr->attrRcMode.attrCappedQuality.iPBDelta = -1;
                rcAttr->attrRcMode.attrCappedQuality.eRcOptions = IMP_ENC_RC_SCN_CHG_RES | IMP_ENC_RC_OPT_SC_PREVENTION;
                rcAttr->attrRcMode.attrCappedQuality.uMaxPictureSize = uTargetBitRate * 4 / 3;
                rcAttr->attrRcMode.attrCappedQuality.uMaxPSNR = 42;
                break;
            case IMP_ENC_RC_MODE_INVALID:
                IMP_LOG_ERR(TAG,
                            "unsupported rcmode:%d, we only support fixqp, cbr vbr and capped vbr",
                            rcAttr->attrRcMode.rcMode);
                return -1;
            }
#endif
            ret = IMP_Encoder_CreateChn(chnNum, &channel_attr);
            if (ret < 0) {
                IMP_LOG_ERR(TAG, "Encoder channel %d creation failed: %d", chnNum, ret);
                return -1;
            }

            /* Configure GOP size explicitly to ensure keyframes are generated (T31 only) */
#if !(defined(PLATFORM_T23) || defined(PLATFORM_T20))
            extern int IMP_Encoder_SetChnGopLength(int encChn, int iGopLength);
            ret = IMP_Encoder_SetChnGopLength(chnNum, gop_length);
            if (ret < 0) {
                IMP_LOG_WARN(TAG, "Failed to set GOP length for channel %d: %d", chnNum, ret);
            }
#else
            /* T23: no API, continue */
#endif

            /* Request immediate IDR frame to ensure keyframes are generated */
            extern int IMP_Encoder_RequestIDR(int encChn);
            ret = IMP_Encoder_RequestIDR(chnNum);
            if (ret < 0) {
                IMP_LOG_WARN(TAG, "Failed to request IDR frame for channel %d: %d", chnNum, ret);
            }

            ret = IMP_Encoder_RegisterChn(chn[i].index, chnNum);
            if (ret < 0) {
                IMP_LOG_ERR(TAG, "Encoder channel %d registration to group %d failed: %d", chn[i].index, chnNum, ret);
                return -1;
            }
        }
    }
    IMP_LOG_INFO(TAG, "Encoder initialized successfully");

    /* JPEG encoder init - create JPEG channels for existing video streams */
    for (int i = 0; i < g_config->stream_count && i < FS_CHN_NUM; i++) {
        if (g_config->streams[i].enabled) {
            int jpeg_channel = FS_CHN_NUM + i; /* JPEG channels start at FS_CHN_NUM + stream_index */
            IMP_LOG_INFO(TAG, "Initializing JPEG encoder for channel %d (stream %d)", jpeg_channel, i);
            ret = jpeg_init_channel(jpeg_channel, i, g_config->streams[i].width, g_config->streams[i].height);
            if (ret < 0) {
                IMP_LOG_ERR(TAG, "JPEG encoder channel %d init failed", jpeg_channel);
                return -1;
            }
            IMP_LOG_INFO(TAG, "JPEG encoder channel %d initialized successfully", jpeg_channel);
        } else {
            IMP_LOG_INFO(TAG, "Stream %d disabled, skipping JPEG encoder initialization", i);
        }
    }

#ifdef ENABLE_OSD
    /* Initialize OSD for enabled channels */
    IMP_LOG_INFO(TAG, "Initializing OSD");
    for (i = 0; i < FS_CHN_NUM; i++) {
        if (chn[i].enable) {
            IMP_LOG_INFO(TAG, "Initializing OSD for enabled channel %d (size: %dx%d)",
                         i, chn[i].fs_chn_attr.picWidth, chn[i].fs_chn_attr.picHeight);

            /* Initialize OSD for this channel */
            ret = osd_init(i, chn[i].fs_chn_attr.picWidth, chn[i].fs_chn_attr.picHeight);
            if (ret < 0) {
                IMP_LOG_WARN(TAG, "OSD initialization failed for channel %d, continuing without OSD", i);
            } else {
                IMP_LOG_INFO(TAG, "OSD initialized successfully for channel %d", i);
            }
        } else {
            IMP_LOG_INFO(TAG, "Channel %d not enabled, skipping OSD initialization", i);
        }
    }

    /* Initialize OSD group cells */
    IMP_LOG_INFO(TAG, "Initializing OSD group cells");
    for (i = 0; i < FS_CHN_NUM; i++) {
        chn[i].osd_grp.deviceID = DEV_ID_OSD;
        chn[i].osd_grp.groupID = i;
        chn[i].osd_grp.outputID = 0;
        IMP_LOG_INFO(TAG, "OSD group cell %d: deviceID=%d, groupID=%d, outputID=%d",
                     i, chn[i].osd_grp.deviceID, chn[i].osd_grp.groupID, chn[i].osd_grp.outputID);
    }
    IMP_LOG_INFO(TAG, "OSD group cells initialized");
#endif

    /* Bind FrameSource -> OSD -> Encoder */
    for (i = 0; i < FS_CHN_NUM; i++) {
        if (chn[i].enable) {
#ifdef ENABLE_OSD
            IMP_LOG_INFO(TAG, "Binding FrameSource -> OSD -> Encoder for channel %d", i);

            /* Check if OSD was successfully initialized for this channel */
            extern osd_context_t* g_osd_contexts[MAX_STREAMS];
            bool osd_available = (i < MAX_STREAMS && g_osd_contexts[i] && g_osd_contexts[i]->initialized);

            IMP_LOG_INFO(TAG, "Channel %d OSD check - i=%d, MAX_STREAMS=%d, g_osd_contexts[%d]=%p",
                         i, i, MAX_STREAMS, i, g_osd_contexts[i]);
            if (i < MAX_STREAMS && g_osd_contexts[i]) {
                IMP_LOG_INFO(TAG, "Channel %d OSD context exists, initialized=%s",
                             i, g_osd_contexts[i]->initialized ? "true" : "false");
            }

            if (osd_available) {
                /* Bind with OSD: FrameSource -> OSD -> Encoder */
                IMP_LOG_INFO(TAG, "OSD available for channel %d, binding with OSD", i);
                ret = IMP_System_Bind(&chn[i].framesource_chn, &chn[i].osd_grp);
                if (ret < 0) {
                    IMP_LOG_ERR(TAG, "Bind FrameSource channel%d and OSD failed: %d", i, ret);
                    return -1;
                }

                ret = IMP_System_Bind(&chn[i].osd_grp, &chn[i].imp_encoder);
                if (ret < 0) {
                    IMP_LOG_ERR(TAG, "Bind OSD and Encoder channel%d failed: %d", i, ret);
                    return -1;
                }
                IMP_LOG_INFO(TAG, "Bound FrameSource -> OSD -> Encoder for channel %d", i);
            } else {
                /* Bind directly without OSD: FrameSource -> Encoder */
                IMP_LOG_INFO(TAG, "OSD not available for channel %d, binding directly", i);
                ret = IMP_System_Bind(&chn[i].framesource_chn, &chn[i].imp_encoder);
                if (ret < 0) {
                    IMP_LOG_ERR(TAG, "Bind FrameSource and Encoder channel%d failed: %d", i, ret);
                    return -1;
                }
                IMP_LOG_INFO(TAG, "Bound FrameSource -> Encoder for channel %d", i);
            }
#else
            /* Bind directly without OSD: FrameSource -> Encoder */
            IMP_LOG_INFO(TAG, "Binding FrameSource -> Encoder for channel %d (OSD disabled)", i);
            ret = IMP_System_Bind(&chn[i].framesource_chn, &chn[i].imp_encoder);
            if (ret < 0) {
                IMP_LOG_ERR(TAG, "Bind FrameSource and Encoder channel%d failed", i);
                return -1;
            }
            IMP_LOG_INFO(TAG, "Bound FrameSource -> Encoder for channel %d", i);
#endif
        } else {
            IMP_LOG_INFO(TAG, "Channel %d not enabled, skipping bind", i);
        }
    }

    /* Stream On */
    for (int i = 0; i < FS_CHN_NUM; i++) {
        if (chn[i].enable) {
            IMP_LOG_INFO(TAG, "Enabling FrameSource channel %d", i);
            IMP_LOG_INFO(TAG, "Channel %d: nrVBs=%d, resolution=%dx%d",
                         i, chn[i].fs_chn_attr.nrVBs,
                         chn[i].fs_chn_attr.picWidth, chn[i].fs_chn_attr.picHeight);

            /* Get and log FIFO attributes to verify buffer settings */
            IMPFSChnFifoAttr current_fifo_attr;
            ret = IMP_FrameSource_GetChnFifoAttr(i, &current_fifo_attr);
            if (ret == 0) {
                IMP_LOG_INFO(TAG, "Channel %d: FIFO maxdepth=%d, type=%d",
                             i, current_fifo_attr.maxdepth, current_fifo_attr.type);
            } else {
                IMP_LOG_WARN(TAG, "Channel %d: Could not get FIFO attributes", i);
            }

            /* Calculate expected memory usage */
            int frame_size = chn[i].fs_chn_attr.picWidth * chn[i].fs_chn_attr.picHeight * 3 / 2;
            int expected_vbm = frame_size * chn[i].fs_chn_attr.nrVBs; /* VBM should use nrVBs * frame_size */
            IMP_LOG_INFO(TAG, "Channel %d: Expected VBM allocation ~%d bytes (%.1fMB) for %d buffers",
                         i, expected_vbm, expected_vbm / (1024.0 * 1024.0), chn[i].fs_chn_attr.nrVBs);

            ret = IMP_FrameSource_EnableChn(i);
            if (ret < 0) {
                IMP_LOG_ERR(TAG, "IMP_FrameSource_EnableChn(%d) error: %d", i, ret);
                IMP_LOG_ERR(TAG, "Channel %d details: %dx%d, nrVBs=%d, pixFmt=%d",
                           i, chn[i].fs_chn_attr.picWidth, chn[i].fs_chn_attr.picHeight,
                           chn[i].fs_chn_attr.nrVBs, chn[i].fs_chn_attr.pixFmt);

                /* Try to get more detailed error information */
                IMP_LOG_ERR(TAG, "Possible causes:");
                IMP_LOG_ERR(TAG, "  1. Memory allocation failure (check dmesg)");
                IMP_LOG_ERR(TAG, "  2. Invalid channel configuration");
                IMP_LOG_ERR(TAG, "  3. Hardware resource conflict");
                IMP_LOG_ERR(TAG, "  4. ISP/Sensor not properly initialized");
                return -1;
            }
            IMP_LOG_INFO(TAG, "FrameSource channel %d enabled successfully", i);
        }
    }
    IMP_LOG_INFO(TAG, "FrameSource stream started successfully");

    /* STEP 3: OSD updates handled by OSD module via RTSP frame callback */
    IMP_LOG_INFO(TAG, "OSD updates will be handled by OSD module");

#ifdef ENABLE_RTSP
    /* RTSP server is now handled by RTSP module */
    IMP_LOG_INFO(TAG, "RTSP functionality handled by RTSP module");
#else
    IMP_LOG_INFO(TAG, "RTSP module disabled - no streaming server");
#endif

    /* RTSP integration will be set up after modules are started */

    /* Start all initialized modules */
    if (module_start_all() != 0) {
        IMP_LOG_WARN(TAG, "Some modules failed to start, continuing");
    }
    IMP_LOG_INFO(TAG, "Modules started");

    /* Initialize and start snapshot fallback system */
    if (snapshot_fallback_init(&g_config->snapshot_fallback) == 0) {
        if (snapshot_fallback_start() == 0) {
            IMP_LOG_INFO(TAG, "Snapshot fallback system started");
        } else {
            IMP_LOG_INFO(TAG, "Snapshot fallback not needed (HTTP available)");
        }
    } else {
        IMP_LOG_WARN(TAG, "Failed to initialize snapshot fallback system");
    }

    /* Setup RTSP integration for all modules - now that RTSP module is started */
#ifdef ENABLE_RTSP
    rtsp_server_t* rtsp_server = rtsp_module_get_server();
    if (rtsp_server) {
        IMP_LOG_INFO(TAG, "Setting up RTSP integration for modules");
        if (module_rtsp_setup_all(rtsp_server) != 0) {
            IMP_LOG_WARN(TAG, "Some modules failed RTSP setup, continuing");
        }

#ifdef ENABLE_OSD
        /* Set RTSP server reference for OSD module */
        if (osd_module_set_rtsp_server(rtsp_server) != 0) {
            IMP_LOG_WARN(TAG, "Failed to set RTSP server for OSD module");
        } else {
            IMP_LOG_INFO(TAG, "OSD module RTSP integration configured");
        }
#endif
    } else {
        IMP_LOG_WARN(TAG, "RTSP server not available for module integration");
    }
#endif

    IMP_LOG_INFO(TAG, "Initialization complete, starting frame manager");

    /* Initialize and start the frame manager */
    if (frame_manager_init() < 0) {
        IMP_LOG_ERR(TAG, "Failed to initialize frame manager");
        goto exit_cleanup;
    }

    /* Register RTSP consumer if RTSP is enabled */
#ifdef ENABLE_RTSP
    if (rtsp_server) {
        extern int rtsp_consumer_init(rtsp_server_t* server);
        if (rtsp_consumer_init(rtsp_server) < 0) {
            IMP_LOG_ERR(TAG, "Failed to initialize RTSP consumer");
        } else {
            IMP_LOG_INFO(TAG, "RTSP consumer registered with frame manager");
        }
    }
#endif

    /* Register RTMP consumer if RTMP client is enabled */
#ifdef ENABLE_RTMP_CLIENT
    {
        rtmp_client_t* rtmp_client = rtmp_client_module_get_client();
        if (rtmp_client) {
            IMP_LOG_INFO(TAG, "RTMP client found, registering consumer with frame manager");
            if (rtmp_consumer_init(rtmp_client) < 0) {
                IMP_LOG_ERR(TAG, "Failed to initialize RTMP consumer");
            } else {
                IMP_LOG_INFO(TAG, "RTMP consumer registered with frame manager");
            }
        } else {
            IMP_LOG_WARN(TAG, "RTMP client not available, skipping consumer registration");
        }
    }
#endif

    /* Start frame processing */
    if (frame_manager_start() < 0) {
        IMP_LOG_ERR(TAG, "Failed to start frame manager");
        goto exit_cleanup;
    }

    IMP_LOG_INFO(TAG, "Frame manager started - using modular architecture");
    while(1) {
        /* Frame manager handles all processing in background thread */
        sleep(1);
    }

exit_cleanup:
    /* Cleanup frame manager */
    IMP_LOG_INFO(TAG, "Stopping frame manager...");
    frame_manager_stop();
    frame_manager_cleanup();

    /* Exit sequence as follow */

    /* Step.a Stream Off */
    for (i = 0; i < FS_CHN_NUM; i++) {
        if (chn[i].enable) {
            ret = IMP_FrameSource_DisableChn(chn[i].index);
            if (ret < 0) {
                IMP_LOG_ERR(TAG, "IMP_FrameSource_DisableChn(%d) error: %d", ret, chn[i].index);
                return -1;
            }
        }
    }
    IMP_LOG_INFO(TAG, "FrameSource stream stopped successfully");

#ifdef ENABLE_OSD
    /* Step.b UnBind OSD -> Encoder -> FrameSource */
    for (i = 0; i < FS_CHN_NUM; i++) {
        if (chn[i].enable) {
            /* Create OSD cell for unbinding */
            IMPCell osd_cell = {DEV_ID_OSD, i, 0};

            /* UnBind OSD -> Encoder */
            ret = IMP_System_UnBind(&osd_cell, &chn[i].imp_encoder);
            if (ret < 0) {
                IMP_LOG_ERR(TAG, "UnBind OSD channel%d and Encoder failed", i);
            }
            IMP_LOG_INFO(TAG, "Unbound OSD -> Encoder for channel %d", i);

            /* UnBind FrameSource -> OSD */
            ret = IMP_System_UnBind(&chn[i].framesource_chn, &osd_cell);
            if (ret < 0) {
                IMP_LOG_ERR(TAG, "UnBind FrameSource channel%d and OSD failed", i);
            }
            IMP_LOG_INFO(TAG, "Unbound FrameSource -> OSD for channel %d", i);
        } else {
            IMP_LOG_INFO(TAG, "Channel %d not enabled, skipping unbind", i);
        }
    }

    /* Cleanup OSD */
    IMP_LOG_INFO(TAG, "Cleaning up OSD");
    osd_cleanup_all();
#endif

    ret = jpeg_exit();
    if (ret < 0) {
        IMP_LOG_ERR(TAG, "Encoder jpeg exit failed");
        return -1;
    }
    IMP_LOG_INFO(TAG, "JPEG encoder exited successfully");

    /* Step.c Encoder exit */
    IMPEncoderCHNStat chn_stat;
    for (i = 0; i < FS_CHN_NUM; i++) {
        if (chn[i].enable) {
            chnNum = chn[i].index;
            memset(&chn_stat, 0, sizeof(IMPEncoderCHNStat));
            ret = IMP_Encoder_Query(chnNum, &chn_stat);
            if (ret < 0) {
                IMP_LOG_ERR(TAG, "IMP_Encoder_Query(%d) error: %d", chnNum, ret);
                return -1;
            }

            if (chn_stat.registered) {
                ret = IMP_Encoder_UnRegisterChn(chnNum);
                if (ret < 0) {
                    IMP_LOG_ERR(TAG, "IMP_Encoder_UnRegisterChn(%d) error: %d", chnNum, ret);
                    return -1;
                }

                ret = IMP_Encoder_DestroyChn(chnNum);
                if (ret < 0) {
                    IMP_LOG_ERR(TAG, "IMP_Encoder_DestroyChn(%d) error: %d", chnNum, ret);
                    return -1;
                }

                ret = IMP_Encoder_DestroyGroup(chnNum);
                if (ret < 0) {
                    IMP_LOG_ERR(TAG, "IMP_Encoder_DestroyGroup(%d) error: %d", chnNum, ret);
                    return -1;
                }
            }
        }
    }
    IMP_LOG_INFO(TAG, "Encoder exited successfully");

    /* Step.d FrameSource exit */
    for (i = 0; i < FS_CHN_NUM; i++) {
        if (chn[i].enable) {
            /* Destroy channel */
            ret = IMP_FrameSource_DestroyChn(chn[i].index);
            if (ret < 0) {
                IMP_LOG_ERR(TAG, "IMP_FrameSource_DestroyChn(%d) error: %d", chn[i].index, ret);
                return -1;
            }
        }
    }
    IMP_LOG_INFO(TAG, "FrameSource exited successfully");

    /* No async processing cleanup needed - using native IMP buffering */

    /* Cleanup snapshot fallback system */
    snapshot_fallback_cleanup();
    IMP_LOG_INFO(TAG, "Snapshot fallback cleaned up");

    /* Cleanup all modules */
    module_cleanup_all();
    IMP_LOG_INFO(TAG, "Modules cleaned up");

    /* Step.e System exit */
    IMP_System_Exit();

    /* No explicit memory pools to clean up for low-memory devices */

    ret = IMP_ISP_DisableSensor();
    if (ret < 0) {
        IMP_LOG_ERR(TAG, "Failed to disable sensor");
        return -1;
    }

    ret = IMP_ISP_DelSensor(&sensor_info);
    if (ret < 0) {
        IMP_LOG_ERR(TAG, "Failed to delete sensor");
        return -1;
    }

    ret = IMP_ISP_DisableTuning();
    if (ret < 0) {
        IMP_LOG_ERR(TAG, "Failed to disable ISP tuning");
        return -1;
    }

    if (IMP_ISP_Close()) {
        IMP_LOG_ERR(TAG, "Failed to open ISP");
        return -1;
    }

    IMP_LOG_INFO(TAG, "System exited successfully");
    return 0;
}
