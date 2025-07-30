/*
 * common.c - Common Functions and Utilities
 * This file contains common functions and utilities for Thingino Streamer
 * Copyright (C) 2025 Paul Philippov, Thingino Project
 */

#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <netdb.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <imp/imp_common.h>
#include <imp/imp_encoder.h>
#include <imp/imp_framesource.h>
#include <imp/imp_isp.h>
#include <imp/imp_osd.h>
#include <imp/imp_system.h>
#include <net/if.h>
#include <sys/types.h>
#include <sys/socket.h>

#include "common.h"
#include "config.h"

/* Shim functions to replace libalog.so dependencies */
int IMP_Log_Get_Option(void)
{
    /* Return 0 - we don't use IMP logging options since we use direct printf */
    return 0;
}

void IMP_Log_Set_Option(int op)
{
    /* No-op shim - we don't need to store options */
    (void)op;
}

void imp_log_fun(int level, int option, int output, const char* tag,
                 const char* file, int line, const char* func,
                 const char* fmt, ...)
{
    /* No-op shim - we use direct printf macros instead */
    (void)level; (void)option; (void)output; (void)tag;
    (void)file; (void)line; (void)func; (void)fmt;
}

#define TAG "COMMON"

static const IMPEncoderRcMode S_RC_METHOD = IMP_ENC_RC_MODE_CBR;

/* Global HTTP server state for endpoint compatibility */
bool http_server_running = false;

/* Global channel metrics array - used by status endpoint and metrics module */
channel_metrics_t channel_metrics[FS_CHN_NUM];
static bool channel_metrics_initialized = false;

// #define LOW_BITSTREAM
// #define SHOW_FRM_BITRATE
#ifdef SHOW_FRM_BITRATE
#define FRM_BIT_RATE_TIME 2
#define STREAM_TYPE_NUM 3
static int frmrate_sp[STREAM_TYPE_NUM] = {0};
static int statime_sp[STREAM_TYPE_NUM] = {0};
static int bitrate_sp[STREAM_TYPE_NUM] = {0};
#endif

// #define JPEG_SNAP_TIMEOUT 1000
// #define VIDEO_STREAM_TIMEOUT 1000

/* Helper functions to convert string config values to enums */
static IMPEncoderProfile string_to_payload_type(const char* format_str)
{
    if (!format_str || strlen(format_str) == 0) {
        return IMP_ENC_PROFILE_AVC_MAIN; /* Use main profile for CABAC support */
    }

    if (strcasecmp(format_str, "H264") == 0)
        return IMP_ENC_PROFILE_AVC_MAIN; /* Use main profile for RTMP/CABAC compatibility */
    if (strcasecmp(format_str, "H265") == 0 || strcasecmp(format_str, "HEVC") == 0)
        return IMP_ENC_PROFILE_HEVC_MAIN;

    IMP_LOG_WARN(TAG, "Unknown video format '%s', using H264 main profile", format_str);
    return IMP_ENC_PROFILE_AVC_MAIN;
}

struct chn_conf chn[FS_CHN_NUM] = {
    {
        .index = 0,
        .enable = 1,
        .payloadType = IMP_ENC_PROFILE_AVC_BASELINE,
        .fs_chn_attr = {
            .pixFmt = PIX_FMT_NV12,
            .outFrmRateNum = SENSOR_FRAME_RATE_NUM,
            .outFrmRateDen = SENSOR_FRAME_RATE_DEN,
            .nrVBs = 3,
            .type = FS_PHY_CHANNEL,
            .crop.enable = 0,
            .crop.top = 0,
            .crop.left = 0,
            .crop.width = SENSOR_WIDTH,
            .crop.height = SENSOR_HEIGHT,
            .scaler.enable = 1,
            .scaler.outwidth = SENSOR_WIDTH,
            .scaler.outheight = SENSOR_HEIGHT,
            .picWidth = SENSOR_WIDTH,
            .picHeight = SENSOR_HEIGHT,
        },
        .framesource_chn = {DEV_ID_FS, 0, 0},
        .imp_encoder = {DEV_ID_ENC, 0, 0},
        .osd_grp = {DEV_ID_OSD, 0, 0},
    },
    {
        .index = 1,
        .enable = 1,
        .payloadType = IMP_ENC_PROFILE_AVC_BASELINE,
        .fs_chn_attr = {
            .pixFmt = PIX_FMT_NV12,
            .outFrmRateNum = SENSOR_FRAME_RATE_NUM,
            .outFrmRateDen = SENSOR_FRAME_RATE_DEN,
            .nrVBs = 2,
            .type = FS_PHY_CHANNEL,
            .crop.enable = 0,
            .crop.top = 0,
            .crop.left = 0,
            .crop.width = SENSOR_WIDTH_SECOND,
            .crop.height = SENSOR_HEIGHT_SECOND,
            .scaler.enable = 1,
            .scaler.outwidth = SENSOR_WIDTH_SECOND,
            .scaler.outheight = SENSOR_HEIGHT_SECOND,
            .picWidth = SENSOR_WIDTH_SECOND,
            .picHeight = SENSOR_HEIGHT_SECOND,
        },
        .framesource_chn = {DEV_ID_FS, 1, 0},
        .imp_encoder = {DEV_ID_ENC, 1, 0},
        .osd_grp = {DEV_ID_OSD, 1, 0},
    },
    {
        .index = 2,
        .enable = 0,
        .payloadType = IMP_ENC_PROFILE_JPEG,
        .fs_chn_attr = {
            .pixFmt = PIX_FMT_NV12,
            .outFrmRateNum = SENSOR_FRAME_RATE_NUM,
            .outFrmRateDen = SENSOR_FRAME_RATE_DEN,
            .nrVBs = 2,
            .type = FS_PHY_CHANNEL,
            .crop.enable = 0,
            .crop.top = 0,
            .crop.left = 0,
            .crop.width = SENSOR_WIDTH_SECOND,
            .crop.height = SENSOR_HEIGHT_SECOND,
            .scaler.enable = 1,
            .scaler.outwidth = SENSOR_WIDTH_SECOND,
            .scaler.outheight = SENSOR_HEIGHT_SECOND,
            .picWidth = SENSOR_WIDTH_SECOND,
            .picHeight = SENSOR_HEIGHT_SECOND,
        },
        .framesource_chn = {DEV_ID_FS, 2, 0},
        .imp_encoder = {DEV_ID_ENC, 2, 0},
        .osd_grp = {DEV_ID_OSD, 2, 0},
    },
    {
        .index = 3,
        .enable = 1,
        .payloadType = IMP_ENC_PROFILE_AVC_BASELINE,
        .fs_chn_attr = {
            .pixFmt = PIX_FMT_NV12,
            .outFrmRateNum = SENSOR_FRAME_RATE_NUM,
            .outFrmRateDen = SENSOR_FRAME_RATE_DEN,
            .nrVBs = 2,
            .type = FS_PHY_CHANNEL,
            .crop.enable = 0,
            .crop.top = 0,
            .crop.left = 0,
            .crop.width = SENSOR_WIDTH_SECOND,
            .crop.height = SENSOR_HEIGHT_SECOND,
            .scaler.enable = 1,
            .scaler.outwidth = SENSOR_WIDTH_SECOND,
            .scaler.outheight = SENSOR_HEIGHT_SECOND,
            .picWidth = SENSOR_WIDTH_SECOND,
            .picHeight = SENSOR_HEIGHT_SECOND,
        },
        .framesource_chn = {DEV_ID_FS, 3, 0},
        .imp_encoder = {DEV_ID_ENC, 3, 0},
        .osd_grp = {DEV_ID_OSD, 3, 0},
    },
};

IMPSensorInfo sensor_info;

/* Low Memory Devices Optimization Functions */
static int g_low_memory_device_cached = -1; /* -1 = not checked, 0 = false, 1 = true */

int is_low_memory_device(void)
{
    /* Return cached result if already determined */
    if (g_low_memory_device_cached != -1) {
        return g_low_memory_device_cached;
    }

    FILE *fp;
    char line[256];
    unsigned long total_mem_kb = 0;

    /* Check for manual override via environment variable */
    char *force_low_mem = getenv("THINGINO_FORCE_LOW_MEMORY");
    if (force_low_mem && (strcmp(force_low_mem, "1") == 0 || strcmp(force_low_mem, "true") == 0)) {
        IMP_LOG_INFO(TAG, "Low-memory mode forced via THINGINO_FORCE_LOW_MEMORY");
        g_low_memory_device_cached = 1;
        return 1;
    }

    /* Check /proc/meminfo for total memory */
    fp = fopen("/proc/meminfo", "r");
    if (fp) {
        while (fgets(line, sizeof(line), fp)) {
            if (sscanf(line, "MemTotal: %lu kB", &total_mem_kb) == 1) {
                break;
            }
        }
        fclose(fp);
    }

    /* Consider devices with less than 96MB as low-memory */
    if (total_mem_kb > 0) {
        IMP_LOG_INFO(TAG, "System memory: %lu MB", total_mem_kb / 1024);
        if (total_mem_kb < 96 * 1024) {
            IMP_LOG_INFO(TAG, "Detected low-memory device: %lu MB total RAM", total_mem_kb / 1024);
            g_low_memory_device_cached = 1;
            return 1;
        }
    } else {
        IMP_LOG_WARN(TAG, "Could not read system memory from /proc/meminfo");
    }

    /* Check for Ingenic Xburst with low memory as fallback */
    fp = fopen("/proc/cpuinfo", "r");
    if (fp) {
        if (total_mem_kb > 0 && total_mem_kb <= 80 * 1024) {
            IMP_LOG_INFO(TAG, "Detected Ingenic device with ≤80MB RAM");
            g_low_memory_device_cached = 1;
            return 1;
        }
    }

    /* Cache the result as "not low-memory" */
    g_low_memory_device_cached = 0;
    return 0;
}

int apply_low_memory_optimizations(void)
{
    if (!is_low_memory_device()) {
        return 0;
    }

    IMP_LOG_WARN(TAG, "Applying memory optimizations for low-memory device");

    /* No resolution changes needed - RTSP module handles output resolution */

    /* Set buffer counts to absolute minimum for 64MB devices */
    chn[0].fs_chn_attr.nrVBs = 1;  /* Back to 1 - the 2x multiplier is elsewhere */
    chn[1].fs_chn_attr.nrVBs = 1;  /* Back to 1 - the 2x multiplier is elsewhere */

    /* Channels 2-3 are not used in current configuration */
    chn[2].enable = 0;
    chn[3].enable = 0;

    IMP_LOG_INFO(TAG, "Low-memory optimizations applied:");
    IMP_LOG_INFO(TAG, "  - Buffer optimization: nrVBs=1, FIFO depth=0");
    IMP_LOG_INFO(TAG, "  - Channel enabling: Based on configuration");
    IMP_LOG_INFO(TAG, "  - Channels 2-3: Not used in current setup");
    IMP_LOG_INFO(TAG, "  - Output resolution: Handled by RTSP module");
    IMP_LOG_INFO(TAG, "SUCCESS: optimized with configuration-driven channel management");

    return 1;
}

int setup_memory_pools_for_low_memory(void)
{
    if (!is_low_memory_device()) {
        return 0;
    }

    IMP_LOG_INFO(TAG, "Skipping explicit memory pools - relying on reduced buffer counts");
    IMP_LOG_INFO(TAG, "Low-memory optimizations rely on:");
    IMP_LOG_INFO(TAG, "  - Reduced nrVBs (1 buffer per channel)");
    IMP_LOG_INFO(TAG, "  - Smaller OSD pool (512KB)");
    IMP_LOG_INFO(TAG, "  - Disabled unused channels");
    IMP_LOG_INFO(TAG, "  - Let IMP system manage RMEM automatically");

    return 1;
}
sensor_info_t g_sensor_info;

static void* get_frame(void* args)
{
    int index = (int) args;
    int chnNum = chn[index].index;
    int i = 0;
    int ret = 0;
    IMPFrameInfo* frame = NULL;
    char framefilename[64];
    int fd = -1;

    if (PIX_FMT_NV12 == chn[index].fs_chn_attr.pixFmt) {
        sprintf(framefilename,
                "frame%dx%d.nv12",
                chn[index].fs_chn_attr.picWidth,
                chn[index].fs_chn_attr.picHeight);
    } else {
        sprintf(framefilename,
                "frame%dx%d.raw",
                chn[index].fs_chn_attr.picWidth,
                chn[index].fs_chn_attr.picHeight);
    }

    fd = open(framefilename, O_RDWR | O_CREAT, 0x644);
    if (fd < 0) {
        IMP_LOG_ERR(TAG, "open %s failed:%s", framefilename, strerror(errno));
        goto err_open_framefilename;
    }

    ret = IMP_FrameSource_SetFrameDepth(chnNum, chn[index].fs_chn_attr.nrVBs * 2);
    if (ret < 0) {
        IMP_LOG_ERR(TAG, "IMP_FrameSource_SetFrameDepth(%d,%d) failed", chnNum, chn[index].fs_chn_attr.nrVBs * 2);
        goto err_IMP_FrameSource_SetFrameDepth_1;
    }

    for (i = 0; i < NR_FRAMES_TO_SAVE; i++) {
        ret = IMP_FrameSource_GetFrame(chnNum, &frame);
        if (ret < 0) {
            IMP_LOG_ERR(TAG, "IMP_FrameSource_GetFrame(%d) i=%d failed", chnNum, i);
            goto err_IMP_FrameSource_GetFrame_i;
        }
        if (NR_FRAMES_TO_SAVE / 2 == i) {
            if (write(fd, (void*) frame->virAddr, frame->size) != frame->size) {
                IMP_LOG_ERR(TAG, "chnNum=%d write frame i=%d failed", chnNum, i);
                goto err_write_frame;
            }
        }
        ret = IMP_FrameSource_ReleaseFrame(chnNum, frame);
        if (ret < 0) {
            IMP_LOG_ERR(TAG, "IMP_FrameSource_ReleaseFrame(%d) i=%d failed", chnNum, i);
            goto err_IMP_FrameSource_ReleaseFrame_i;
        }
    }

    IMP_FrameSource_SetFrameDepth(chnNum, 0);

    return (void*) 0;

err_IMP_FrameSource_ReleaseFrame_i:
err_write_frame:
    IMP_FrameSource_ReleaseFrame(chnNum, frame);
err_IMP_FrameSource_GetFrame_i:
    goto err_IMP_FrameSource_SetFrameDepth_1;
    IMP_FrameSource_SetFrameDepth(chnNum, 0);
err_IMP_FrameSource_SetFrameDepth_1:
    close(fd);
err_open_framefilename:
    return (void*) -1;
}

/* Apply JSON configuration to channel array - preserves original SDK code */
int apply_config_to_channels()
{
    extern streamer_config_t* g_config;

    if (!g_config) {
        IMP_LOG_WARN(TAG, "No configuration available, using static channel settings");
        return 0;
    }

    /* Disable all channels first */
    for (int i = 0; i < FS_CHN_NUM; i++) {
        chn[i].enable = 0;
    }

    /* Apply stream 0 configuration */
    if (g_config->stream_count > 0) {
        stream_config_t* stream = &g_config->streams[0];
        chn[0].enable = stream->enabled ? 1 : 0;
        chn[0].payloadType = string_to_payload_type(stream->format);

        /* Apply resolution and framerate */
        chn[0].fs_chn_attr.picWidth = stream->width;
        chn[0].fs_chn_attr.picHeight = stream->height;
        chn[0].fs_chn_attr.outFrmRateNum = g_config->sensor.fps;
        chn[0].fs_chn_attr.outFrmRateDen = 1;

        /* Apply framesource configuration if available */
        if (stream->framesource.picture_width > 0) {
            chn[0].fs_chn_attr.picWidth = stream->framesource.picture_width;
            chn[0].fs_chn_attr.picHeight = stream->framesource.picture_height;
        }
        if (stream->framesource.frame_rate_num > 0) {
            chn[0].fs_chn_attr.outFrmRateNum = stream->framesource.frame_rate_num;
            chn[0].fs_chn_attr.outFrmRateDen = stream->framesource.frame_rate_den;
        }

        IMP_LOG_INFO(TAG, "Channel 0: enabled=%d, format='%s' -> payloadType=0x%x (%s), %dx%d@%d/%dfps",
                    chn[0].enable, stream->format, chn[0].payloadType,
                    ((chn[0].payloadType >> 24) == IMP_ENC_TYPE_HEVC) ? "H265" : "H264",
                    chn[0].fs_chn_attr.picWidth, chn[0].fs_chn_attr.picHeight,
                    chn[0].fs_chn_attr.outFrmRateNum, chn[0].fs_chn_attr.outFrmRateDen);
    }

    /* Apply stream 1 configuration */
    if (g_config->stream_count > 1) {
        stream_config_t* stream = &g_config->streams[1];
        chn[1].enable = stream->enabled ? 1 : 0;
        chn[1].payloadType = string_to_payload_type(stream->format);

        /* Apply resolution and framerate */
        chn[1].fs_chn_attr.picWidth = stream->width;
        chn[1].fs_chn_attr.picHeight = stream->height;
        /* Use sensor FPS for all channels */
        chn[1].fs_chn_attr.outFrmRateNum = g_config->sensor.fps;
        chn[1].fs_chn_attr.outFrmRateDen = 1;

        /* Apply framesource configuration if available */
        if (stream->framesource.picture_width > 0) {
            chn[1].fs_chn_attr.picWidth = stream->framesource.picture_width;
            chn[1].fs_chn_attr.picHeight = stream->framesource.picture_height;
        }
        if (stream->framesource.frame_rate_num > 0) {
            chn[1].fs_chn_attr.outFrmRateNum = stream->framesource.frame_rate_num;
            chn[1].fs_chn_attr.outFrmRateDen = stream->framesource.frame_rate_den;
        }

        /* Configure scaler for channel 1 */
        chn[1].fs_chn_attr.scaler.enable = 1;
        chn[1].fs_chn_attr.scaler.outwidth = stream->width;
        chn[1].fs_chn_attr.scaler.outheight = stream->height;

        IMP_LOG_INFO(TAG, "Channel 1: enabled=%d, format='%s' -> payloadType=0x%x (%s), %dx%d@%d/%dfps, scaler=%s",
                    chn[1].enable, stream->format, chn[1].payloadType,
                    ((chn[1].payloadType >> 24) == IMP_ENC_TYPE_HEVC) ? "H265" : "H264",
                    chn[1].fs_chn_attr.picWidth, chn[1].fs_chn_attr.picHeight,
                    chn[1].fs_chn_attr.outFrmRateNum, chn[1].fs_chn_attr.outFrmRateDen,
                    chn[1].fs_chn_attr.scaler.enable ? "enabled" : "disabled");
    }

    /* Channel 2 is NOT used as a framesource - JPEG uses channel 4 encoder that shares channel 0 framesource */
    chn[2].enable = 0; /* Always disabled - JPEG doesn't need separate framesource */

    /* Channel 3 is disabled - only use channels 0 and 1 for video streams */
    chn[3].enable = 0;

    IMP_LOG_DBG(TAG, "Updated channel configuration from JSON config: ch0=%d, ch1=%d, ch2=%d, ch3=%d",
                chn[0].enable, chn[1].enable, chn[2].enable, chn[3].enable);

    return 0;
}

int jpeg_exit(void)
{
    int i = 0;
    int ret = 0;
    int jpeg_channel = 0;
    IMPEncoderChnStat chn_stat;

    /* Clean up JPEG channels that were created for configured streams */
    extern struct streamer_config* g_config;
    if (g_config && g_config->streams) {
        for (i = 0; i < g_config->stream_count && i < FS_CHN_NUM; i++) {
            if (g_config->streams[i].enabled) {
                jpeg_channel = FS_CHN_NUM + i; /* Same numbering as in jpeg_init_channel */
                memset(&chn_stat, 0, sizeof(IMPEncoderChnStat));

                /* Check if channel is registered */
                ret = IMP_Encoder_Query(jpeg_channel, &chn_stat);
                if (ret < 0) {
                    IMP_LOG_ERR(TAG, "IMP_Encoder_Query(%d) error: %d", jpeg_channel, ret);
                    return -1;
                }

                if (chn_stat.registered) {
                    /* Unregister the channel */
                    ret = IMP_Encoder_UnRegisterChn(jpeg_channel);
                    if (ret < 0) {
                        IMP_LOG_ERR(TAG, "IMP_Encoder_UnRegisterChn(%d) error: %d", jpeg_channel, ret);
                        return -1;
                    }

                    /* Destroy channel */
                    ret = IMP_Encoder_DestroyChn(jpeg_channel);
                    if (ret < 0) {
                        IMP_LOG_ERR(TAG, "IMP_Encoder_DestroyChn(%d) error: %d", jpeg_channel, ret);
                        return -1;
                    }
                    IMP_LOG_DBG(TAG, "Cleaned up JPEG channel %d", jpeg_channel);
                }
            }
        }
    }

    return 0;
}

// int osd_exit(IMPRgnHandle* prHander, int grpNum)
// {
//     int ret;

//     ret = IMP_OSD_ShowRgn(prHander[0], grpNum, 0);
//     if (ret < 0) {
//         IMP_LOG_ERR(TAG, "IMP_OSD_ShowRgn close timeStamp error");
//     }

//     ret = IMP_OSD_ShowRgn(prHander[1], grpNum, 0);
//     if (ret < 0) {
//         IMP_LOG_ERR(TAG, "IMP_OSD_ShowRgn close Logo error");
//     }

//     ret = IMP_OSD_ShowRgn(prHander[2], grpNum, 0);
//     if (ret < 0) {
//         IMP_LOG_ERR(TAG, "IMP_OSD_ShowRgn close cover error");
//     }

//     ret = IMP_OSD_ShowRgn(prHander[3], grpNum, 0);
//     if (ret < 0) {
//         IMP_LOG_ERR(TAG, "IMP_OSD_ShowRgn close Rect error");
//     }

//     ret = IMP_OSD_UnRegisterRgn(prHander[0], grpNum);
//     if (ret < 0) {
//         IMP_LOG_ERR(TAG, "IMP_OSD_UnRegisterRgn timeStamp error");
//     }

//     ret = IMP_OSD_UnRegisterRgn(prHander[1], grpNum);
//     if (ret < 0) {
//         IMP_LOG_ERR(TAG, "IMP_OSD_UnRegisterRgn logo error");
//     }

//     ret = IMP_OSD_UnRegisterRgn(prHander[2], grpNum);
//     if (ret < 0) {
//         IMP_LOG_ERR(TAG, "IMP_OSD_UnRegisterRgn Cover error");
//     }

//     ret = IMP_OSD_UnRegisterRgn(prHander[3], grpNum);
//     if (ret < 0) {
//         IMP_LOG_ERR(TAG, "IMP_OSD_UnRegisterRgn Rect error");
//     }

//     IMP_OSD_DestroyRgn(prHander[0]);
//     IMP_OSD_DestroyRgn(prHander[1]);
//     IMP_OSD_DestroyRgn(prHander[2]);
//     IMP_OSD_DestroyRgn(prHander[3]);

//     ret = IMP_OSD_DestroyGroup(grpNum);
//     if (ret < 0) {
//         IMP_LOG_ERR(TAG, "IMP_OSD_DestroyGroup(0) error");
//         return -1;
//     }
//     free(prHander);
//     prHander = NULL;

//     return 0;
// }

// static int save_stream(int fd, IMPEncoderStream* stream)
// {
//     int ret, i, nr_pack = stream->packCount;

//     for (i = 0; i < nr_pack; i++) {
//         IMPEncoderPack* pack = &stream->pack[i];
//         if (pack->length) {
//             uint32_t remSize = stream->streamSize - pack->offset;
//             if (remSize < pack->length) {
//                 ret = write(fd, (void*) (stream->virAddr + pack->offset), remSize);
//                 if (ret != remSize) {
//                     IMP_LOG_ERR(TAG,
//                                 "stream write ret(%d) != pack[%d].remSize(%d) error:%s",
//                                 ret,
//                                 i,
//                                 remSize,
//                                 strerror(errno));
//                     return -1;
//                 }
//                 ret = write(fd, (void*) stream->virAddr, pack->length - remSize);
//                 if (ret != (pack->length - remSize)) {
//                     IMP_LOG_ERR(TAG,
//                                 "stream write ret(%d) != pack[%d].(length-remSize)(%d) error:%s",
//                                 ret,
//                                 i,
//                                 (pack->length - remSize),
//                                 strerror(errno));
//                     return -1;
//                 }
//             } else {
//                 ret = write(fd, (void*) (stream->virAddr + pack->offset), pack->length);
//                 if (ret != pack->length) {
//                     IMP_LOG_ERR(TAG,
//                                 "stream write ret(%d) != pack[%d].length(%d) error:%s",
//                                 ret,
//                                 i,
//                                 pack->length,
//                                 strerror(errno));
//                     return -1;
//                 }
//             }
//         }
//     }
//     return 0;
// }

// static int save_stream_by_name(char* stream_prefix, int idx, IMPEncoderStream* stream)
// {
//     int i = 0;
//     int ret = 0;
//     int stream_fd = -1;
//     char stream_path[128];
//     int nr_pack = stream->packCount;

//     sprintf(stream_path, "%s_%d", stream_prefix, idx);

//     IMP_LOG_DBG(TAG, "Open Stream file %s ", stream_path);
//     stream_fd = open(stream_path, O_RDWR | O_CREAT | O_TRUNC, 0777);
//     if (stream_fd < 0) {
//         IMP_LOG_ERR(TAG, "failed: %s", strerror(errno));
//         return -1;
//     }
//     IMP_LOG_DBG(TAG, "OK");

//     for (i = 0; i < nr_pack; i++) {
//         IMPEncoderPack* pack = &stream->pack[i];
//         if (pack->length) {
//             uint32_t remSize = stream->streamSize - pack->offset;
//             if (remSize < pack->length) {
//                 ret = write(stream_fd, (void*) (stream->virAddr + pack->offset), remSize);
//                 if (ret != remSize) {
//                     IMP_LOG_ERR(TAG,
//                                 "stream write ret(%d) != pack[%d].remSize(%d) error:%s",
//                                 ret,
//                                 i,
//                                 remSize,
//                                 strerror(errno));
//                     return -1;
//                 }
//                 ret = write(stream_fd, (void*) stream->virAddr, pack->length - remSize);
//                 if (ret != (pack->length - remSize)) {
//                     IMP_LOG_ERR(TAG,
//                                 "stream write ret(%d) != pack[%d].(length-remSize)(%d) error:%s",
//                                 ret,
//                                 i,
//                                 (pack->length - remSize),
//                                 strerror(errno));
//                     return -1;
//                 }
//             } else {
//                 ret = write(stream_fd, (void*) (stream->virAddr + pack->offset), pack->length);
//                 if (ret != pack->length) {
//                     IMP_LOG_ERR(TAG,
//                                 "stream write ret(%d) != pack[%d].length(%d) error:%s",
//                                 ret,
//                                 i,
//                                 pack->length,
//                                 strerror(errno));
//                     return -1;
//                 }
//             }
//         }
//     }

//     close(stream_fd);

//     return 0;
// }

// static void* get_video_stream(void* args)
// {
//     int i = 0;
//     int ret = 0;
//     int val = 0;
//     int chnNum = 0;
//     char stream_path[64];
//     IMPEncoderEncType encType;
//     int stream_fd = -1;
//     int totalSaveCnt = 0;

//     val = (int) args;
//     chnNum = val & 0xffff;
//     encType = (val >> 16) & 0xffff;

//     ret = IMP_Encoder_StartRecvPic(chnNum);
//     if (ret < 0) {
//         IMP_LOG_ERR(TAG, "IMP_Encoder_StartRecvPic(%d) failed", chnNum);
//         return ((void*) -1);
//     }

//     sprintf(stream_path,
//             "%s/stream-%d.%s",
//             STREAM_FILE_PATH_PREFIX,
//             chnNum,
//             (encType == IMP_ENC_TYPE_AVC) ? "h264" : ((encType == IMP_ENC_TYPE_HEVC) ? "h265" : "jpeg"));

//     if (encType == IMP_ENC_TYPE_JPEG) {
//         totalSaveCnt = ((NR_FRAMES_TO_SAVE / 50) > 0) ? (NR_FRAMES_TO_SAVE / 50) : 1;
//     } else {
//         stream_fd = open(stream_path, O_RDWR | O_CREAT | O_TRUNC, 0777);
//         if (stream_fd < 0) {
//             IMP_LOG_ERR(TAG, "failed: %s", strerror(errno));
//             return ((void*) -1);
//         }
//         totalSaveCnt = NR_FRAMES_TO_SAVE;
//     }

//     for (i = 0; i < totalSaveCnt; i++) {
//         ret = IMP_Encoder_PollingStream(chnNum, VIDEO_STREAM_TIMEOUT);
//         if (ret < 0) {
//             IMP_LOG_ERR(TAG, "IMP_Encoder_PollingStream(%d) timeout", chnNum);
//             continue;
//         }

//         IMPEncoderStream stream;
//         /* Get H264 or H265 Stream */
//         ret = IMP_Encoder_GetStream(chnNum, &stream, 1);
// #ifdef SHOW_FRM_BITRATE
//         int i, len = 0;
//         for (i = 0; i < stream.packCount; i++) {
//             len += stream.pack[i].length;
//         }
//         bitrate_sp[chnNum] += len;
//         frmrate_sp[chnNum]++;

//         int64_t now = IMP_System_GetTimeStamp() / 1000;
//         if (((int) (now - statime_sp[chnNum]) / 1000) >= FRM_BIT_RATE_TIME) {
//             double fps = (double) frmrate_sp[chnNum] / ((double) (now - statime_sp[chnNum]) / 1000);
//             double kbr = (double) bitrate_sp[chnNum] * 8 / (double) (now - statime_sp[chnNum]);

//             IMP_LOG_DBG(TAG, "streamNum[%d]:FPS: %0.2f,Bitrate: %0.2f(kbps)", chnNum, fps, kbr);

//             frmrate_sp[chnNum] = 0;
//             bitrate_sp[chnNum] = 0;
//             statime_sp[chnNum] = now;
//         }
// #endif
//         if (ret < 0) {
//             IMP_LOG_ERR(TAG, "IMP_Encoder_GetStream(%d) failed", chnNum);
//             return ((void*) -1);
//         }

//         if (encType == IMP_ENC_TYPE_JPEG) {
//             ret = save_stream_by_name(stream_path, i, &stream);
//             if (ret < 0) {
//                 return ((void*) ret);
//             }
//         } else {
//             ret = save_stream(stream_fd, &stream);
//             if (ret < 0) {
//                 close(stream_fd);
//                 return ((void*) ret);
//             }
//         }
//         IMP_Encoder_ReleaseStream(chnNum, &stream);
//     }

//     close(stream_fd);

//     IMP_LOG_DBG(TAG, "Video Channel %d saved %d frames to %s ", chnNum, totalSaveCnt, stream_path);

//     ret = IMP_Encoder_StopRecvPic(chnNum);
//     if (ret < 0) {
//         IMP_LOG_ERR(TAG, "IMP_Encoder_StopRecvPic(%d) failed", chnNum);
//         return ((void*) -1);
//     }

//     return ((void*) 0);
// }

// int get_jpeg_snap()
// {
//     int i, ret;
//     char snap_path[64];

//     for (i = 0; i < FS_CHN_NUM; i++) {
//         if (chn[i].enable) {
//             ret = IMP_Encoder_StartRecvPic(FS_CHN_NUM + chn[i].index);
//             if (ret < 0) {
//                 IMP_LOG_ERR(TAG, "IMP_Encoder_StartRecvPic(%d) failed", 3 + chn[i].index);
//                 return -1;
//             }

//             sprintf(snap_path, "%s/snap-%d.jpg", SNAP_FILE_PATH_PREFIX, chn[i].index);
//             int snap_fd = open(snap_path, O_RDWR | O_CREAT | O_TRUNC, 0777);
//             if (snap_fd < 0) {
//                 IMP_LOG_ERR(TAG, "Failed to open %s: %s", snap_path, strerror(errno));
//                 return -1;
//             }

//             ret = IMP_Encoder_PollingStream(FS_CHN_NUM + chn[i].index, JPEG_SNAP_TIMEOUT);
//             if (ret < 0) {
//                 IMP_LOG_ERR(TAG, "Polling stream timeout");
//                 continue;
//             }

//             IMPEncoderStream stream;
//             /* Get JPEG Snap */
//             ret = IMP_Encoder_GetStream(FS_CHN_NUM + chn[i].index, &stream, 1);
//             if (ret < 0) {
//                 IMP_LOG_ERR(TAG, "IMP_Encoder_GetStream() failed");
//                 return -1;
//             }

//             ret = save_stream(snap_fd, &stream);
//             if (ret < 0) {
//                 close(snap_fd);
//                 return ret;
//             }

//             IMP_Encoder_ReleaseStream(FS_CHN_NUM + chn[i].index, &stream);

//             close(snap_fd);

//             ret = IMP_Encoder_StopRecvPic(FS_CHN_NUM + chn[i].index);
//             if (ret < 0) {
//                 IMP_LOG_ERR(TAG, "IMP_Encoder_StopRecvPic() failed");
//                 return -1;
//             }

//             IMP_LOG_DBG(TAG, "Saved JPEG Snap to %s ", snap_path);
//         }
//     }

//     return 0;
// }

// int get_video_stream_byfd()
// {
//     int i = 0;
//     int ret = 0;
//     int chnNum = 0;
//     int streamFd[FS_CHN_NUM], vencFd[FS_CHN_NUM], maxVencFd = 0;
//     char stream_path[FS_CHN_NUM][128];
//     fd_set readfds;
//     struct timeval selectTimeout;
//     int saveStreamCnt[FS_CHN_NUM], totalSaveStreamCnt[FS_CHN_NUM];
//     memset(streamFd, 0, sizeof(streamFd));
//     memset(vencFd, 0, sizeof(vencFd));
//     memset(stream_path, 0, sizeof(stream_path));
//     memset(saveStreamCnt, 0, sizeof(saveStreamCnt));
//     memset(totalSaveStreamCnt, 0, sizeof(totalSaveStreamCnt));

//     for (i = 0; i < FS_CHN_NUM; i++) {
//         streamFd[i] = -1;
//         vencFd[i] = -1;
//         saveStreamCnt[i] = 0;
//         if (chn[i].enable) {
//             if (chn[i].payloadType == IMP_ENC_PROFILE_JPEG) {
//                 chnNum = FS_CHN_NUM + chn[i].index;
//                 totalSaveStreamCnt[i] = (NR_FRAMES_TO_SAVE / 50 > 0) ? NR_FRAMES_TO_SAVE / 50 : NR_FRAMES_TO_SAVE;
//             } else {
//                 chnNum = chn[i].index;
//                 totalSaveStreamCnt[i] = NR_FRAMES_TO_SAVE;
//             }
//             sprintf(stream_path[i],
//                     "%s/stream-%d.%s",
//                     STREAM_FILE_PATH_PREFIX,
//                     chnNum,
//                     ((chn[i].payloadType >> 24) == IMP_ENC_TYPE_AVC)
//                         ? "h264"
//                         : (((chn[i].payloadType >> 24) == IMP_ENC_TYPE_HEVC) ? "h265" : "jpeg"));

//             if (chn[i].payloadType != IMP_ENC_PROFILE_JPEG) {
//                 streamFd[i] = open(stream_path[i], O_RDWR | O_CREAT | O_TRUNC, 0777);
//                 if (streamFd[i] < 0) {
//                     IMP_LOG_ERR(TAG, "open %s failed:%s", stream_path[i], strerror(errno));
//                     return -1;
//                 }
//             }

//             vencFd[i] = IMP_Encoder_GetFd(chnNum);
//             if (vencFd[i] < 0) {
//                 IMP_LOG_ERR(TAG, "IMP_Encoder_GetFd(%d) failed", chnNum);
//                 return -1;
//             }

//             if (maxVencFd < vencFd[i]) {
//                 maxVencFd = vencFd[i];
//             }

//             ret = IMP_Encoder_StartRecvPic(chnNum);
//             if (ret < 0) {
//                 IMP_LOG_ERR(TAG, "IMP_Encoder_StartRecvPic(%d) failed", chnNum);
//                 return -1;
//             }
//         }
//     }

//     while (1) {
//         int breakFlag = 1;
//         for (i = 0; i < FS_CHN_NUM; i++) {
//             breakFlag &= (saveStreamCnt[i] >= totalSaveStreamCnt[i]);
//         }
//         if (breakFlag) {
//             break; /* save frame enough */
//         }

//         FD_ZERO(&readfds);
//         for (i = 0; i < FS_CHN_NUM; i++) {
//             if (chn[i].enable && saveStreamCnt[i] < totalSaveStreamCnt[i]) {
//                 FD_SET(vencFd[i], &readfds);
//             }
//         }
//         selectTimeout.tv_sec = 2;
//         selectTimeout.tv_usec = 0;

//         ret = select(maxVencFd + 1, &readfds, NULL, NULL, &selectTimeout);
//         if (ret < 0) {
//             IMP_LOG_ERR(TAG, "select failed:%s", strerror(errno));
//             return -1;
//         } else if (ret == 0) {
//             continue;
//         } else {
//             for (i = 0; i < FS_CHN_NUM; i++) {
//                 if (chn[i].enable && FD_ISSET(vencFd[i], &readfds)) {
//                     IMPEncoderStream stream;

//                     if (chn[i].payloadType == IMP_ENC_PROFILE_JPEG) {
//                         chnNum = FS_CHN_NUM + chn[i].index;
//                     } else {
//                         chnNum = chn[i].index;
//                     }

//                     /* Get H264 or H265 Stream */
//                     ret = IMP_Encoder_GetStream(chnNum, &stream, 1);
//                     if (ret < 0) {
//                         IMP_LOG_ERR(TAG, "IMP_Encoder_GetStream(%d) failed", chnNum);
//                         return -1;
//                     }

//                     if (chn[i].payloadType == IMP_ENC_PROFILE_JPEG) {
//                         ret = save_stream_by_name(stream_path[i], saveStreamCnt[i], &stream);
//                         if (ret < 0) {
//                             return -1;
//                         }
//                     } else {
//                         ret = save_stream(streamFd[i], &stream);
//                         if (ret < 0) {
//                             close(streamFd[i]);
//                             return -1;
//                         }
//                     }

//                     IMP_Encoder_ReleaseStream(chnNum, &stream);
//                     saveStreamCnt[i]++;
//                 }
//             }
//         }
//     }

//     for (i = 0; i < FS_CHN_NUM; i++) {
//         if (chn[i].enable) {
//             if (chn[i].payloadType == IMP_ENC_PROFILE_JPEG) {
//                 chnNum = FS_CHN_NUM + chn[i].index;
//             } else {
//                 chnNum = chn[i].index;
//             }
//             IMP_Encoder_StopRecvPic(chnNum);
//             close(streamFd[i]);
//         }
//     }

//     return 0;
// }

/* Network utilities */

/**
 * Get device IP address by scanning network interfaces
 * @param ip_buffer Buffer to store the IP address string
 * @param buffer_size Size of the buffer
 * @return 0 on success, -1 on failure
 */
int get_device_ip_address(char* ip_buffer, size_t buffer_size)
{
    struct ifaddrs *ifaddr, *ifa;
    int family, s;
    char host[NI_MAXHOST];

    if (!ip_buffer || buffer_size == 0) {
        return -1;
    }

    /* Initialize with default */
    strncpy(ip_buffer, "0.0.0.0", buffer_size - 1);
    ip_buffer[buffer_size - 1] = '\0';

    if (getifaddrs(&ifaddr) == -1) {
        IMP_LOG_WARN(TAG, "Failed to get network interfaces");
        return -1;
    }

    /* Walk through linked list, maintaining head pointer so we can free list later */
    for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == NULL)
            continue;

        family = ifa->ifa_addr->sa_family;

        /* Skip loopback and non-IPv4 addresses */
        if (family != AF_INET ||
            (ifa->ifa_flags & IFF_LOOPBACK) ||
            strcmp(ifa->ifa_name, "lo") == 0 ||
            strncmp(ifa->ifa_name, "docker", 6) == 0)
            continue;

        s = getnameinfo(ifa->ifa_addr, sizeof(struct sockaddr_in),
                        host, NI_MAXHOST, NULL, 0, NI_NUMERICHOST);
        if (s != 0) {
            continue;
        }

        /* Found a valid IPv4 address */
        strncpy(ip_buffer, host, buffer_size - 1);
        ip_buffer[buffer_size - 1] = '\0';
        freeifaddrs(ifaddr);
        return 0;
    }

    freeifaddrs(ifaddr);

    /* If no IP was found, keep default and return success */
    IMP_LOG_INFO(TAG, "No valid network interface found, using default IP");
    return 0;
}

/* Timing utilities */

/**
 * Get current monotonic time in microseconds
 * Note: Ingenic MIPS is a 32-bit architecture, so we need to use 32-bit arithmetic
 * to avoid overflow issues.
 * @return Current monotonic time in microseconds since system boot
 */
uint64_t get_monotonic_time_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);

    /* Use simple 32-bit arithmetic to build the 64-bit result */
    unsigned long sec = ts.tv_sec;
    unsigned long usec = ts.tv_nsec / 1000;

    /* Build result using only 32-bit operations */
    uint64_t result = sec;
    result = result * 1000000UL;  /* Convert seconds to microseconds */
    result = result + usec;       /* Add microseconds */

    return result;
}

/* Safe send function that handles client disconnections and partial sends */
int safe_send(int socket, const void* data, size_t len)
{
    const char* ptr = (const char*)data;
    size_t remaining = len;
    size_t total_sent = 0;

    while (remaining > 0) {
        ssize_t sent = send(socket, ptr, remaining, MSG_NOSIGNAL);
        if (sent < 0) {
            if (errno == EPIPE || errno == ECONNRESET || errno == ENOTCONN) {
                /* Client disconnected - this is normal */
                IMP_LOG_DBG(TAG, "Client disconnected during send (sent %zu/%zu bytes)", total_sent, len);
                return -1;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                /* Socket buffer full, try again */
                IMP_LOG_DBG(TAG, "Socket buffer full, retrying (sent %zu/%zu bytes)", total_sent, len);
                usleep(1000); /* Wait 1ms */
                continue;
            }
            /* Other error */
            IMP_LOG_WARN(TAG, "Send error after %zu/%zu bytes: %s", total_sent, len, strerror(errno));
            return -1;
        }

        if (sent == 0) {
            IMP_LOG_WARN(TAG, "send() returned 0, connection may be closed (sent %zu/%zu bytes)", total_sent, len);
            return -1;
        }

        ptr += sent;
        remaining -= sent;
        total_sent += sent;
    }

    IMP_LOG_DBG(TAG, "Successfully sent all %zu bytes", len);
    return 0; /* All data sent successfully */
}

/* Initialize channel metrics - always needed for status endpoint compatibility */
int metrics_init_endpoint(void)
{
    if (channel_metrics_initialized) {
        return 0;
    }

    /* Initialize channel metrics */
    for (int i = 0; i < FS_CHN_NUM; i++) {
        memset(&channel_metrics[i], 0, sizeof(channel_metrics_t));
        channel_metrics[i].channel = i;
        pthread_mutex_init(&channel_metrics[i].mutex, NULL);
    }

    channel_metrics_initialized = true;
    return 0;
}

void metrics_cleanup_endpoint(void)
{
    if (!channel_metrics_initialized) {
        return;
    }

    /* Cleanup mutexes */
    for (int i = 0; i < FS_CHN_NUM; i++) {
        pthread_mutex_destroy(&channel_metrics[i].mutex);
    }

    channel_metrics_initialized = false;
}
