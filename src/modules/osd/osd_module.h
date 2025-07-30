/*
 * osd_module.h - OSD Module Interface
 * Copyright (C) 2025 Paul Philippov, Thingino Project
 */

#ifndef __OSD_MODULE_H__
#define __OSD_MODULE_H__

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include <imp/imp_osd.h>
#include <json-c/json.h>

#include "../../module_system.h"
#include "../../config.h"
#include "schrift.h"

#define OSD_MODULE_VERSION "1.0.0"
#define OSD_MODULE_NAME "osd"

#ifndef MAX_STREAMS
#define MAX_STREAMS 2
#endif

/* OSD module configuration */
typedef struct {
    bool enabled;                     /* Enable/disable OSD module */
    int update_interval_ms;           /* Update interval in milliseconds */

    /* Per-stream OSD settings */
    struct {
        bool enabled;                 /* Enable OSD for this stream */

        /* Time display */
        struct {
            bool enabled;             /* Enable time display */
            char format[64];          /* Time format string */
            char font[256];           /* Font file path */
            int size;                 /* Font size */
            char color[16];           /* Text color */
            char background[16];      /* Background color */
            char position[16];        /* Position "X,Y" */
        } time;

        /* Logo display */
        struct {
            bool enabled;             /* Enable logo display */
            char image[256];          /* Logo image path */
            char size[16];            /* Logo size "WxH" */
            char position[16];        /* Logo position "X,Y" */
            float opacity;            /* Logo opacity 0.0-1.0 */
        } logo;

        /* Info display */
        struct {
            bool enabled;             /* Enable info display */
            char position[16];        /* Info position "X,Y" */
        } info;

        /* Motion zones visualization */
        struct {
            bool enabled;             /* Enable motion zones visualization */
            bool show_include_zones;  /* Show include zones */
            bool show_exclude_zones;  /* Show exclude zones */
            char include_color[16];   /* Include zone color "#RRGGBBAA" */
            char exclude_color[16];   /* Exclude zone color "#RRGGBBAA" */
            int line_width;           /* Zone border line width */
        } motion_zones;
    } streams[2];                     /* Support for 2 streams */
} osd_module_config_t;

/* Glyph cache entry */
typedef struct {
    int width;
    int height;
    uint8_t* bitmap; /* BGRA format */
    int advance;
    int xmin;
    int ymin;
    SFT_Glyph glyph;
} osd_glyph_t;

/* Simple glyph cache (ASCII characters only) */
#define OSD_GLYPH_CACHE_SIZE 128

/* OSD region types */
#define OSD_REGION_FONT   0  /* Timestamp region */
#define OSD_REGION_LOGO   1  /* Logo region */
#define OSD_REGION_COVER  2  /* Privacy cover region */
#define OSD_REGION_RECT   3  /* Rectangle overlay region */
#define OSD_REGION_INFO   4  /* Info display region (brightness, etc) */
#define OSD_REGION_MOTION_ZONE_0 5  /* Motion detection zone 0 */
#define OSD_REGION_MOTION_ZONE_1 6  /* Motion detection zone 1 */
#define OSD_REGION_MOTION_ZONE_2 7  /* Motion detection zone 2 */
#define OSD_REGION_MOTION_ZONE_3 8  /* Motion detection zone 3 */
#define OSD_REGION_COUNT  9  /* Total number of regions */

/* Motion zone region helpers */
#define OSD_MOTION_ZONE_REGION_START OSD_REGION_MOTION_ZONE_0
#define OSD_MAX_MOTION_ZONES 4

/* OSD context for a stream */
typedef struct osd_context {
    int group_id;
    int stream_width;
    int stream_height;
    stream_config_t* config;
    bool initialized;
    bool started;
    time_t last_update;
    pthread_mutex_t mutex;

    /* Region handles */
    IMPRgnHandle region_handles[OSD_REGION_COUNT];

    /* Font rendering context */
    SFT* sft;
    uint8_t* font_data;
    size_t font_data_size;
    int current_font_size;

    /* Color settings in BGRA format */
    uint8_t bgra_text[4];
    uint8_t bgra_bg[4];

    /* Glyph cache for performance */
    osd_glyph_t glyph_cache[OSD_GLYPH_CACHE_SIZE];

    /* Font metrics */
    int font_ascent;
    int font_descent;
    int font_line_height;
    int font_max_height;
    int font_deepest_descender;

    /* Time format validation */
    bool time_format_valid;
    char validated_time_format[64];

    /* Timestamp OSD region data */
    struct {
        IMPRgnHandle handle;
        uint8_t* data;
        int width;
        int height;
        bool enabled;
        int layer;
        IMPOSDRgnAttrData* rgnAttrData;
    } timestamp;

    /* Logo buffer */
    uint8_t* logo_buffer;

    /* Info OSD region data */
    struct {
        IMPRgnHandle handle;
        uint8_t* data;
        int width;
        int height;
        bool enabled;
        int layer;
        IMPOSDRgnAttrData* rgnAttrData;
    } info;

    /* Motion zones visualization */
    struct {
        bool enabled;
        bool show_include_zones;
        bool show_exclude_zones;
        uint32_t include_color;  /* BGRA color for include zones */
        uint32_t exclude_color;  /* BGRA color for exclude zones */
        uint32_t line_width;
    } motion_zones;
} osd_context_t;

/* Global OSD contexts array */
extern osd_context_t* g_osd_contexts[];

/* OSD functions */

/* OSD context management */

/* Initialize OSD context */
int osd_context_init(osd_context_t* ctx, int group_id, int stream_width, int stream_height, stream_config_t* config);

/* Start OSD */
int osd_context_start(osd_context_t* ctx);

/* Stop OSD */
int osd_context_stop(osd_context_t* ctx);

/* Cleanup OSD */
int osd_context_cleanup(osd_context_t* ctx);

/* Update OSD (called periodically) */
int osd_context_update(osd_context_t* ctx);

/* Region management */

/* Create all OSD regions */
int osd_create_regions(osd_context_t* ctx);

/* Create timestamp region */
int osd_create_timestamp_region(osd_context_t* ctx);

/* Setup OSD regions */
int osd_setup_font_region(osd_context_t* ctx);
int osd_setup_logo_region(osd_context_t* ctx);
int osd_setup_cover_region(osd_context_t* ctx);
int osd_setup_rect_region(osd_context_t* ctx);
int osd_setup_info_region(osd_context_t* ctx);
int osd_update_info_display(osd_context_t* ctx, float iso, float gb_gain, float gr_gain, bool night_mode);

/* Motion zone visualization */
int osd_setup_motion_zones(osd_context_t* ctx);
int osd_enable_motion_zones(int group_id, bool enabled);
int osd_set_motion_zone_colors(int group_id, uint32_t include_color, uint32_t exclude_color);
int osd_update_motion_zones(int group_id);

/* libschrift font rendering */

/* Initialize libschrift font rendering */
int osd_libschrift_init(osd_context_t* ctx);

/* Cleanup libschrift font rendering */
int osd_libschrift_cleanup(osd_context_t* ctx);

/* Render a single glyph and cache it */
int osd_render_glyph(osd_context_t* ctx, char c);

/* Calculate text size using libschrift */
int osd_calculate_text_size(osd_context_t* ctx, const char* text, int* width, int* height);

/* Draw text using libschrift with consistent baseline */
int osd_draw_text(osd_context_t* ctx, uint8_t* image, const char* text, int image_width, int image_height);

/* Utility functions */

/**
 * Create a text bitmap using libschrift font rendering
 *
 * This function calculates the required bitmap size for the given text,
 * allocates a BGRA bitmap buffer, and renders the text into it using
 * the configured font and colors.
 *
 * @param ctx    [in]  OSD context containing font configuration and rendering state
 * @param text   [in]  Null-terminated string to render (ASCII characters only)
 * @param bitmap [out] Pointer to allocated bitmap buffer (BGRA format, 4 bytes per pixel)
 *                     Caller must free() this buffer when done
 * @param width  [out] Width of the created bitmap in pixels
 * @param height [out] Height of the created bitmap in pixels
 *
 * @return 0 on success, -1 on failure
 *
 * @note The bitmap is allocated with malloc() and must be freed by the caller
 * @note Text color and background color are taken from ctx->bgra_text and ctx->bgra_bg
 * @note Requires ctx->sft to be initialized via osd_libschrift_init()
 */
int osd_create_text_bitmap(osd_context_t* ctx, const char* text, uint8_t** bitmap, int* width, int* height);

/**
 * Load a logo bitmap from a file
 *
 * This function reads the logo image from the specified file path
 * and allocates a buffer to hold the BGRA pixel data.
 *
 * @param path   [in]  Path to the logo image file
 * @param bitmap [out] Pointer to allocated bitmap buffer (BGRA format, 4 bytes per pixel)
 *                     Caller must free() this buffer when done
 * @param width  [in]  Expected width of the logo image in pixels
 * @param height [in]  Expected height of the logo image in pixels
 *
 * @return 0 on success, -1 on failure
 *
 * @note The bitmap is allocated with malloc() and must be freed by the caller
 */
int osd_load_logo_bitmap(const char* path, uint8_t** bitmap, int width, int height);

/* Bridge functions for streamer compatibility */

/* Initialize OSD for a specific group */
int osd_init(int group_id, int stream_width, int stream_height);

/* OSD control functions */

/* Set timestamp OSD - bridge function */
int osd_set_timestamp(int group_id, const char* timestamp);

/* Set logo OSD - bridge function */
int osd_set_logo(int group_id, int x, int y, const uint8_t* logo_data, int width, int height);

/* Set text OSD - bridge function */
int osd_set_text(int group_id, int x, int y, const char* text);

/* OSD update and cleanup functions */

/* Update OSD for a specific group */
int osd_update(int group_id);

/* Cleanup OSD for a specific group */
int osd_cleanup(int group_id);

/* Cleanup all OSD contexts */
void osd_cleanup_all(void);

/* Update timestamp with specific text */
int osd_update_timestamp(osd_context_t* ctx, const char* text);

/* Update timestamp with current time */
int osd_update_timestamp_current(osd_context_t* ctx);

/* Module interface */
extern module_info_t osd_module_info;

/* OSD module functions */
int osd_module_init(void* config);
int osd_module_start(void);
int osd_module_stop(void);
int osd_module_cleanup(void);
int osd_module_config_parse(json_object* json, void* config);
int osd_module_get_config_size(void);

/* OSD control functions */
int osd_module_set_timestamp(int stream_id, const char* timestamp);
int osd_module_set_logo(int stream_id, int x, int y, const uint8_t* logo_data, int width, int height);
int osd_module_set_text(int stream_id, int x, int y, const char* text);
int osd_module_update_info(int stream_id, float iso, float gb_gain, float gr_gain, bool night_mode);

/* RTSP integration */
struct rtsp_server;
int osd_module_set_rtsp_server(struct rtsp_server* server);
int osd_module_rtsp_frame_callback(struct rtsp_server* server, int channel, const uint8_t* frame_data, uint32_t frame_size, const struct timeval* timestamp);

/* Module registration */
int register_osd_module(void);

#endif /* __OSD_MODULE_H__ */
