/*
 * osd_module.c - OSD Module Wrapper
 * Copyright (C) 2025 Paul Philippov, Thingino Project
 *
 * This module wraps the existing OSD functionality to integrate with the modular system
 */

#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <schrift.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <json-c/json.h>
#include <sys/time.h>

#include "../../common.h"
#include "../../config.h"
#include "../rtsp/rtsp_server.h"

#include "osd_module.h"

#define TAG "OSD_MODULE"

#define OSD_DEFAULT_FONT_FILE "/usr/share/fonts/default.ttf"

/* Module state */
static struct {
    bool initialized;
    bool running;
    osd_module_config_t config;
    rtsp_server_t* rtsp_server;  /* Reference to RTSP server for client count */

    /* Independent timer thread for OSD updates */
    pthread_t timer_thread;
    bool timer_thread_running;
    pthread_mutex_t timer_mutex;
} g_osd_module_state = {
    .timer_mutex = PTHREAD_MUTEX_INITIALIZER
};

/* Forward declarations */
static int osd_module_init_streams(void);
static int osd_start_timer_thread(void);
static int osd_stop_timer_thread(void);
static void* osd_timer_thread(void* arg);

/* Global variables for OSD thread control */
static pthread_mutex_t osd_thread_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t osd_thread_cond = PTHREAD_COND_INITIALIZER;

/* External reference to RTSP server */
extern rtsp_server_t* rtsp_server;

// FIXME: make padding logarithmic
int padding_top = 4;
int padding_left = 8;

/* Global variables */
osd_context_t* g_osd_contexts[MAX_STREAMS] = {NULL};

/* Parse position string "X,Y" and handle negative values for right/bottom alignment */
static void parse_position(const char* position_str,
                           int stream_width,
                           int stream_height,
                           int text_width,
                           int text_height,
                           int* x,
                           int* y)
{
    /* Default position */
    int pos_x = 20;
    int pos_y = 20;

    if (position_str && position_str[0]) {
        sscanf(position_str, "%d,%d", &pos_x, &pos_y);
    }

    /* Handle negative values for right/bottom alignment
     * Negative value = distance from right/bottom edge to right/bottom edge of element
     * For -10: position should be 10 pixels from right edge
     * So: start_pos = screen_width + pos_x - element_width */
    if (pos_x < 0) {
        *x = stream_width + pos_x - text_width;
    } else {
        *x = pos_x;
    }

    if (pos_y < 0) {
        *y = stream_height + pos_y - text_height;
    } else {
        *y = pos_y;
    }

    /* Ensure position is within bounds */
    if (*x < 0) {
        *x = 0;
    }

    if (*y < 0) {
        *y = 0;
    }

    if (*x + text_width > stream_width) {
        *x = stream_width - text_width;
    }

    if (*y + text_height > stream_height) {
        *y = stream_height - text_height;
    }
}

/* Validate and cache time format during initialization */
static int osd_validate_time_format(osd_context_t* ctx)
{
    if (!ctx || !ctx->config->osd.time.format) {
        IMP_LOG_ERR(TAG, "No time format specified for group %d (config or format is NULL)", ctx ? ctx->group_id : -1);
        return -1;
    }

    const char* format = ctx->config->osd.time.format;

    if (strlen(format) == 0) {
        IMP_LOG_ERR(TAG, "Empty time format string for group %d", ctx->group_id);
        return -1;
    }

    if (strlen(format) >= sizeof(ctx->validated_time_format)) {
        IMP_LOG_ERR(TAG, "Time format too long for group %d: '%s' (max %zu chars)",
                   ctx->group_id, format, sizeof(ctx->validated_time_format) - 1);
        return -1;
    }

    time_t now;
    char test_buffer[64];
    time(&now);
    struct tm* tm_info = localtime(&now);

    if (strftime(test_buffer, sizeof(test_buffer), format, tm_info) == 0) {
        IMP_LOG_ERR(TAG, "strftime failed with format '%s' for group %d", format, ctx->group_id);
        return -1;
    }

    strncpy(ctx->validated_time_format, format, sizeof(ctx->validated_time_format) - 1);
    ctx->validated_time_format[sizeof(ctx->validated_time_format) - 1] = '\0';
    ctx->time_format_valid = true;

    IMP_LOG_INFO(TAG, "Time format validated for group %d: '%s' -> '%s'",
                 ctx->group_id, format, test_buffer);
    return 0;
}

/* Initialize OSD context */
int osd_context_init(osd_context_t* ctx, int group_id, int stream_width, int stream_height, stream_config_t* config)
{
    if (!ctx || !config) {
        IMP_LOG_ERR(TAG, "Invalid parameters: ctx=%p, config=%p", ctx, config);
        return -1;
    }

    IMP_LOG_INFO(TAG, "Initializing OSD context for group %d (%dx%d)", group_id, stream_width, stream_height);
    IMP_LOG_INFO(TAG, "OSD config - enabled=%s, time.enabled=%s, time.format='%s'",
                 config->osd.enabled ? "true" : "false",
                 config->osd.time.enabled ? "true" : "false",
                 config->osd.time.format ? config->osd.time.format : "NULL");

    /* Initialize context */
    memset(ctx, 0, sizeof(osd_context_t));
    ctx->group_id = group_id;
    ctx->stream_width = stream_width;
    ctx->stream_height = stream_height;
    ctx->config = config;
    ctx->initialized = false;
    ctx->started = false;

    IMP_LOG_INFO(TAG, "Context initialized with group_id=%d, dimensions=%dx%d",
                 ctx->group_id, ctx->stream_width, ctx->stream_height);

    /* Initialize all region handles to invalid */
    for (int i = 0; i < OSD_REGION_COUNT; i++) {
        ctx->region_handles[i] = INVHANDLE;
    }
    IMP_LOG_INFO(TAG, "All %d region handles initialized to INVHANDLE (%d)", OSD_REGION_COUNT, INVHANDLE);

    /* Initialize motion zones visualization with defaults */
    ctx->motion_zones.enabled = false;
    ctx->motion_zones.show_include_zones = true;
    ctx->motion_zones.show_exclude_zones = true;
    ctx->motion_zones.include_color = 0x00FF0080; /* Green with 50% alpha (BGRA) */
    ctx->motion_zones.exclude_color = 0x0000FF80; /* Red with 50% alpha (BGRA) */
    ctx->motion_zones.line_width = 2;

    /* Initialize mutex */
    if (pthread_mutex_init(&ctx->mutex, NULL) != 0) {
        IMP_LOG_ERR(TAG, "Failed to initialize mutex for group %d", group_id);
        return -1;
    }
    IMP_LOG_INFO(TAG, "Mutex initialized successfully for group %d", group_id);

    /* Store in global array */
    if (group_id >= 0 && group_id < MAX_STREAMS) {
        g_osd_contexts[group_id] = ctx;
        IMP_LOG_INFO(TAG, "Stored OSD context for group %d in global array at index %d", group_id, group_id);
    } else {
        IMP_LOG_ERR(TAG, "Invalid group_id %d for global array storage (MAX_STREAMS=%d)", group_id, MAX_STREAMS);
        pthread_mutex_destroy(&ctx->mutex);
        return -1;
    }

    /* Create OSD group */
    IMP_LOG_INFO(TAG, "Creating OSD group %d", group_id);
    int ret = IMP_OSD_CreateGroup(group_id);
    if (ret < 0) {
        IMP_LOG_ERR(TAG, "IMP_OSD_CreateGroup(%d) failed: %d", group_id, ret);
        g_osd_contexts[group_id] = NULL;
        pthread_mutex_destroy(&ctx->mutex);
        return -1;
    }
    IMP_LOG_INFO(TAG, "OSD group %d created successfully", group_id);

    /* Initialize libschrift for text rendering */
    IMP_LOG_INFO(TAG, "Initializing libschrift for group %d", group_id);
    ret = osd_libschrift_init(ctx);
    if (ret != 0) {
        IMP_LOG_ERR(TAG, "Failed to initialize libschrift for group %d", group_id);
        IMP_OSD_DestroyGroup(group_id);
        g_osd_contexts[group_id] = NULL;
        pthread_mutex_destroy(&ctx->mutex);
        return -1;
    }
    IMP_LOG_INFO(TAG, "libschrift initialized successfully for group %d", group_id);

    /* Validate time format */
    IMP_LOG_INFO(TAG, "Validating time format for group %d", group_id);
    if (osd_validate_time_format(ctx) != 0) {
        IMP_LOG_ERR(TAG, "Failed to validate time format for group %d", group_id);
        osd_libschrift_cleanup(ctx);
        IMP_OSD_DestroyGroup(group_id);
        g_osd_contexts[group_id] = NULL;
        pthread_mutex_destroy(&ctx->mutex);
        return -1;
    }
    IMP_LOG_INFO(TAG, "Time format validated successfully for group %d", group_id);

    /* Create OSD regions */
    IMP_LOG_INFO(TAG, "Creating OSD regions for group %d", group_id);
    ret = osd_create_regions(ctx);
    if (ret != 0) {
        IMP_LOG_ERR(TAG, "Failed to create OSD regions for group %d", group_id);
        osd_libschrift_cleanup(ctx);
        IMP_OSD_DestroyGroup(group_id);
        g_osd_contexts[group_id] = NULL;
        pthread_mutex_destroy(&ctx->mutex);
        return -1;
    }
    IMP_LOG_INFO(TAG, "OSD regions created successfully for group %d", group_id);

    /* Start OSD */
    IMP_LOG_INFO(TAG, "Starting OSD for group %d", group_id);
    ret = IMP_OSD_Start(group_id);
    if (ret < 0) {
        IMP_LOG_ERR(TAG, "IMP_OSD_Start(%d) failed: %d", group_id, ret);
        osd_libschrift_cleanup(ctx);
        IMP_OSD_DestroyGroup(group_id);
        g_osd_contexts[group_id] = NULL;
        pthread_mutex_destroy(&ctx->mutex);
        return -1;
    }
    IMP_LOG_INFO(TAG, "OSD started successfully for group %d", group_id);

    ctx->started = true;
    ctx->initialized = true;

    IMP_LOG_INFO(TAG, "OSD initialized and started for Group %d (%dx%d) - COMPLETE SUCCESS", group_id, stream_width, stream_height);
    return 0;
}

/* Create all OSD regions */
int osd_create_regions(osd_context_t* ctx)
{
    if (!ctx) {
        IMP_LOG_ERR(TAG, "Invalid context in osd_create_regions");
        return -1;
    }

    IMP_LOG_INFO(TAG, "Creating all OSD regions for Group %d", ctx->group_id);
    int ret;

    IMP_LOG_INFO(TAG, "Creating font region for Group %d", ctx->group_id);
    ctx->region_handles[OSD_REGION_FONT] = IMP_OSD_CreateRgn(NULL);
    IMP_LOG_INFO(TAG, "Font region created with handle %d for Group %d",
                 ctx->region_handles[OSD_REGION_FONT], ctx->group_id);
    if (ctx->region_handles[OSD_REGION_FONT] == INVHANDLE) {
        IMP_LOG_ERR(TAG, "Failed to create TimeStamp region for Group %d (INVHANDLE=%d)", ctx->group_id, INVHANDLE);
        return -1;
    }

    IMP_LOG_INFO(TAG, "Creating logo region for Group %d", ctx->group_id);
    ctx->region_handles[OSD_REGION_LOGO] = IMP_OSD_CreateRgn(NULL);
    IMP_LOG_INFO(TAG, "Logo region created with handle %d for Group %d",
                 ctx->region_handles[OSD_REGION_LOGO], ctx->group_id);
    if (ctx->region_handles[OSD_REGION_LOGO] == INVHANDLE) {
        IMP_LOG_ERR(TAG, "Failed to create Logo region for Group %d", ctx->group_id);
        return -1;
    }

    IMP_LOG_INFO(TAG, "Creating cover region for Group %d", ctx->group_id);
    ctx->region_handles[OSD_REGION_COVER] = IMP_OSD_CreateRgn(NULL);
    IMP_LOG_INFO(TAG, " Cover region created with handle %d for Group %d",
                 ctx->region_handles[OSD_REGION_COVER], ctx->group_id);
    if (ctx->region_handles[OSD_REGION_COVER] == INVHANDLE) {
        IMP_LOG_ERR(TAG, "Failed to create Cover region for Group %d", ctx->group_id);
        return -1;
    }

    IMP_LOG_INFO(TAG, "Creating rect region for Group %d", ctx->group_id);
    ctx->region_handles[OSD_REGION_RECT] = IMP_OSD_CreateRgn(NULL);
    IMP_LOG_INFO(TAG, "Rect region created with handle %d for Group %d",
                 ctx->region_handles[OSD_REGION_RECT], ctx->group_id);
    if (ctx->region_handles[OSD_REGION_RECT] == INVHANDLE) {
        IMP_LOG_ERR(TAG, "Failed to create Rect region for Group %d", ctx->group_id);
        return -1;
    }

    IMP_LOG_INFO(TAG, "Creating info region for Group %d", ctx->group_id);
    ctx->region_handles[OSD_REGION_INFO] = IMP_OSD_CreateRgn(NULL);
    IMP_LOG_INFO(TAG, "Info region created with handle %d for Group %d",
                 ctx->region_handles[OSD_REGION_INFO], ctx->group_id);
    if (ctx->region_handles[OSD_REGION_INFO] == INVHANDLE) {
        IMP_LOG_ERR(TAG, "Failed to create Info region for Group %d", ctx->group_id);
        return -1;
    }

    IMP_LOG_INFO(TAG, "Creating %d motion zone regions for Group %d", OSD_MAX_MOTION_ZONES, ctx->group_id);
    for (int i = 0; i < OSD_MAX_MOTION_ZONES; i++) {
        int region_index = OSD_MOTION_ZONE_REGION_START + i;
        ctx->region_handles[region_index] = IMP_OSD_CreateRgn(NULL);
        IMP_LOG_INFO(TAG, "Motion zone region %d created with handle %d for Group %d",
                     i, ctx->region_handles[region_index], ctx->group_id);
        if (ctx->region_handles[region_index] == INVHANDLE) {
            IMP_LOG_ERR(TAG, "Failed to create Motion zone region %d for Group %d", i, ctx->group_id);
            return -1;
        }
    }

    /* Register all regions to group */
    IMP_LOG_INFO(TAG, "Registering all %d regions to Group %d", OSD_REGION_COUNT, ctx->group_id);
    for (int i = 0; i < OSD_REGION_COUNT; i++) {
        IMP_LOG_INFO(TAG, "Registering region %d (handle=%d) to Group %d",
                     i, ctx->region_handles[i], ctx->group_id);
        ret = IMP_OSD_RegisterRgn(ctx->region_handles[i], ctx->group_id, NULL);
        if (ret < 0) {
            IMP_LOG_ERR(TAG, "IMP_OSD_RegisterRgn failed for region %d, handle=%d, group=%d, ret=%d",
                         i, ctx->region_handles[i], ctx->group_id, ret);
            return -1;
        }
        IMP_LOG_INFO(TAG, "Region %d registered successfully to Group %d", i, ctx->group_id);
    }

    // IMP_LOG_INFO(TAG, "Setting up cover region for Group %d", ctx->group_id);
    if (osd_setup_cover_region(ctx) != 0) {
        IMP_LOG_ERR(TAG, "Failed to setup cover region for Group %d", ctx->group_id);
        return -1;
    }
    // IMP_LOG_INFO(TAG, "Cover region setup completed for Group %d", ctx->group_id);

    // IMP_LOG_INFO(TAG, "Setting up rect region for Group %d", ctx->group_id);
    if (osd_setup_rect_region(ctx) != 0) {
        IMP_LOG_ERR(TAG, "Failed to setup rect region for Group %d", ctx->group_id);
        return -1;
    }
    // IMP_LOG_INFO(TAG, "Rect region setup completed for Group %d", ctx->group_id);

    // IMP_LOG_INFO(TAG, "Setting up info region for Group %d", ctx->group_id);
    if (osd_setup_info_region(ctx) != 0) {
        IMP_LOG_WARN(TAG, "Failed to setup info region for Group %d, continuing without info display", ctx->group_id);
    } else {
        IMP_LOG_INFO(TAG, "Info region setup completed for Group %d", ctx->group_id);
    }

    // IMP_LOG_INFO(TAG, "Setting up logo region for Group %d", ctx->group_id);
    if (osd_setup_logo_region(ctx) != 0) {
        IMP_LOG_WARN(TAG, "Failed to setup logo region for Group %d, continuing without logo", ctx->group_id);
    } else {
        IMP_LOG_INFO(TAG, "Logo region setup completed for Group %d", ctx->group_id);
    }

    // IMP_LOG_INFO(TAG, "Creating timestamp region for Group %d", ctx->group_id);
    if (osd_create_timestamp_region(ctx) != 0) {
        IMP_LOG_WARN(TAG, "Failed to create timestamp region for Group %d, continuing without timestamp", ctx->group_id);
    } else {
        IMP_LOG_INFO(TAG, "Timestamp region created successfully for Group %d", ctx->group_id);
    }

    // IMP_LOG_INFO(TAG, "Setting up motion zones region for Group %d", ctx->group_id);
    if (osd_setup_motion_zones(ctx) != 0) {
        IMP_LOG_WARN(TAG, "Failed to setup motion zones region for Group %d, continuing without motion zones", ctx->group_id);
    } else {
        IMP_LOG_INFO(TAG, "Motion zones region setup completed for Group %d", ctx->group_id);
    }

    // IMP_LOG_INFO(TAG, "All OSD regions created and configured for Group %d - SUCCESS", ctx->group_id);
    return 0;
}

/* Create timestamp region */
int osd_create_timestamp_region(osd_context_t* ctx)
{
    if (!ctx) {
        IMP_LOG_ERR(TAG, "Invalid OSD context for timestamp region creation");
        return -1;
    }

    IMP_LOG_INFO(TAG, "Creating timestamp region for Group %d", ctx->group_id);

    /* Create region */
    ctx->timestamp.handle = IMP_OSD_CreateRgn(NULL);
    IMP_LOG_INFO(TAG, "IMP_OSD_CreateRgn returned handle: %d for Group %d timestamp", ctx->timestamp.handle, ctx->group_id);

    if (ctx->timestamp.handle == INVHANDLE) {
        IMP_LOG_ERR(TAG, "Failed to create timestamp region for group %d (INVHANDLE=%d)", ctx->group_id, INVHANDLE);
        return -1;
    }

    IMP_LOG_INFO(TAG, "Timestamp region created with handle %d for Group %d",
                    ctx->timestamp.handle, ctx->group_id);

    /* Register region with group */
    IMP_LOG_INFO(TAG, "Registering timestamp region %d with group %d", ctx->timestamp.handle, ctx->group_id);
    int ret = IMP_OSD_RegisterRgn(ctx->timestamp.handle, ctx->group_id, NULL);
    if (ret != 0) {
        IMP_LOG_ERR(TAG, "Failed to register timestamp region %d with group %d: %d",
                     ctx->timestamp.handle, ctx->group_id, ret);
        IMP_OSD_DestroyRgn(ctx->timestamp.handle);
        ctx->timestamp.handle = INVHANDLE;
        return -1;
    }

    IMP_LOG_INFO(TAG, "Timestamp region %d registered with Group %d successfully",
                 ctx->timestamp.handle, ctx->group_id);

    return 0;
}

/* Create text bitmap using libschrift */
int osd_create_text_bitmap(osd_context_t* ctx, const char* text, uint8_t** bitmap, int* width, int* height)
{
    // FIXME: split to separate calls for better error handling? (e.g. free bitmap if allocation fails)
    if (!text || !bitmap || !width || !height) {
        IMP_LOG_ERR(TAG, "Invalid parameters for creating text bitmap");
        return -1;
    }

    if (!ctx) {
        IMP_LOG_ERR(TAG, "Invalid OSD context");
        return -1;
    }

    if (!ctx->sft) {
        IMP_LOG_ERR(TAG, "libschrift not available - cannot render text");
        return -1;
    }

    /* Calculate text size */
    if (osd_calculate_text_size(ctx, text, width, height) != 0) {
        IMP_LOG_ERR(TAG, "Failed to calculate text size with libschrift");
        return -1;
    }

    /* Allocate bitmap */
    int bitmap_size = (*width) * (*height) * 4; /* BGRA format */
    *bitmap = (uint8_t*) malloc(bitmap_size);
    if (!*bitmap) {
        IMP_LOG_ERR(TAG, "Failed to allocate bitmap memory");
        return -1;
    }

    /* Apply background color if configured */
    if (ctx->bgra_bg[3] > 0) {
        /* Background has alpha, fill with background color */
        uint32_t bg_pixel = (ctx->bgra_bg[3] << 24) | (ctx->bgra_bg[2] << 16) | (ctx->bgra_bg[1] << 8) | ctx->bgra_bg[0];
        uint32_t* pixel_ptr = (uint32_t*)*bitmap;
        for (int i = 0; i < (*width) * (*height); i++) {
            pixel_ptr[i] = bg_pixel;
        }
    } else {
        /* Transparent background */
        memset(*bitmap, 0, bitmap_size);
    }

    /* Draw text using libschrift */
    if (osd_draw_text(ctx, *bitmap, text, *width, *height) != 0) {
        IMP_LOG_ERR(TAG, "Failed to draw text with libschrift");
        free(*bitmap);
        *bitmap = NULL;
        return -1;
    }

    /* Text rendered successfully */
    return 0;
}

/* Pre-calculate font metrics for consistent text height */
static void osd_precalculate_font_metrics(osd_context_t* ctx)
{
    if (!ctx || !ctx->sft) {
        return;
    }

    /* Initialize metrics */
    ctx->font_max_height = 0;
    ctx->font_deepest_descender = 0;  /* Most negative ymin */
    bool first_glyph = true;

    /* Ensure we have representative glyphs for proper metrics calculation */
    const char* metric_chars = "AgjyqpQWH@0123456789";
    for (const char* c = metric_chars; *c; c++) {
        osd_render_glyph(ctx, *c);
    }

    /* Check all cached glyphs to find the extremes */
    for (int i = 0; i < OSD_GLYPH_CACHE_SIZE; i++) {
        osd_glyph_t* glyph = &ctx->glyph_cache[i];

        /* Skip empty cache entries */
        if (glyph->width == 0 && glyph->height == 0) {
            continue;
        }

        if (first_glyph) {
            ctx->font_deepest_descender = glyph->ymin;
            first_glyph = false;
        } else {
            /* Find deepest descender (most negative ymin) */
            if (glyph->ymin < ctx->font_deepest_descender) {
                ctx->font_deepest_descender = glyph->ymin;
            }
        }

        /* Update max height */
        if (glyph->height > ctx->font_max_height) {
            ctx->font_max_height = glyph->height;
        }
    }

    SFT_LMetrics lmetrics;
    if (sft_lmetrics(ctx->sft, &lmetrics) != 0) {
        IMP_LOG_ERR(TAG, "Failed to get font line metrics for size calculation");
        return;
    }
}

/* Initialize libschrift font rendering */
int osd_libschrift_init(osd_context_t* ctx)
{
    if (!ctx) {
        IMP_LOG_ERR(TAG, "Invalid OSD context for libschrift initialization");
        return -1;
    }

    /* Load font file from configuration */
    const char* font_path = ctx->config->osd.time.font;
    FILE* font_file = fopen(font_path, "rb");
    if (!font_file) {
        IMP_LOG_ERR(TAG, "Failed to open font file: %s", font_path);
        return -1;
    }

    /* Get file size */
    fseek(font_file, 0, SEEK_END);
    ctx->font_data_size = ftell(font_file);
    fseek(font_file, 0, SEEK_SET);

    /* Allocate and read font data */
    ctx->font_data = malloc(ctx->font_data_size);
    if (!ctx->font_data) {
        IMP_LOG_ERR(TAG, "Failed to allocate font data memory");
        fclose(font_file);
        return -1;
    }

    if (fread(ctx->font_data, 1, ctx->font_data_size, font_file) != ctx->font_data_size) {
        IMP_LOG_ERR(TAG, "Failed to read font data");
        free(ctx->font_data);
        ctx->font_data = NULL;
        fclose(font_file);
        return -1;
    }
    fclose(font_file);

    /* Initialize SFT context */
    ctx->sft = malloc(sizeof(SFT));
    if (!ctx->sft) {
        IMP_LOG_ERR(TAG, "Failed to allocate SFT context");
        free(ctx->font_data);
        ctx->font_data = NULL;
        return -1;
    }

    ctx->sft->font = sft_loadmem(ctx->font_data, ctx->font_data_size);

    /* Simple, fast font rendering */
    ctx->sft->xScale = (double) ctx->config->osd.time.size;
    ctx->sft->yScale = (double) ctx->config->osd.time.size;
    ctx->sft->xOffset = 0.0;
    ctx->sft->yOffset = 0.0;
    ctx->sft->flags = SFT_DOWNWARD_Y;

    IMP_LOG_INFO(TAG, "libschrift initialized with simple, fast rendering");

    if (!ctx->sft->font) {
        IMP_LOG_ERR(TAG, "Failed to load font from memory");
        free(ctx->sft);
        free(ctx->font_data);
        ctx->sft = NULL;
        ctx->font_data = NULL;
        return -1;
    }

    /* Parse text color from string format "#RRGGBBAA" */
    uint32_t font_color = 0xFFFFFFFF; /* Default white */
    const char* color_str = ctx->config->osd.time.color;
    if (color_str && color_str[0] == '#' && strlen(color_str) == 9) {
        font_color = (uint32_t) strtoul(color_str + 1, NULL, 16);
        IMP_LOG_DBG(TAG, "Parsed font color '%s' -> 0x%08X", color_str, font_color);
    } else {
        IMP_LOG_ERR(TAG, "Using default font color (invalid format: '%s')", color_str ? color_str : "NULL");
    }

    /* Convert RGBA to BGRA for text */
    ctx->bgra_text[0] = (font_color >> 8) & 0xFF;  /* B */
    ctx->bgra_text[1] = (font_color >> 16) & 0xFF; /* G */
    ctx->bgra_text[2] = (font_color >> 24) & 0xFF; /* R */
    ctx->bgra_text[3] = font_color & 0xFF;         /* A */

    /* Parse background color from string format "#RRGGBBAA" */
    uint32_t bg_color = 0x00000000; /* Default transparent */
    const char* bg_str = ctx->config->osd.time.background;
    if (bg_str && bg_str[0] == '#' && strlen(bg_str) == 9) {
        bg_color = (uint32_t) strtoul(bg_str + 1, NULL, 16);
        IMP_LOG_DBG(TAG, "Parsed background color '%s' -> 0x%08X", bg_str, bg_color);
    } else {
        IMP_LOG_ERR(TAG, "Using default background color (invalid format: '%s')", bg_str ? bg_str : "NULL");
    }

    /* Convert RGBA to BGRA for background */
    ctx->bgra_bg[0] = (bg_color >> 8) & 0xFF;  /* B */
    ctx->bgra_bg[1] = (bg_color >> 16) & 0xFF; /* G */
    ctx->bgra_bg[2] = (bg_color >> 24) & 0xFF; /* R */
    ctx->bgra_bg[3] = bg_color & 0xFF;         /* A */

    IMP_LOG_INFO(
        TAG,
        "OSD colors: font=%s -> 0x%08X (BGRA: %d,%d,%d,%d), bg=%s -> 0x%08X (BGRA: %d,%d,%d,%d)",
        color_str,
        font_color,
        ctx->bgra_text[0],
        ctx->bgra_text[1],
        ctx->bgra_text[2],
        ctx->bgra_text[3],
        bg_str,
        bg_color,
        ctx->bgra_bg[0],
        ctx->bgra_bg[1],
        ctx->bgra_bg[2],
        ctx->bgra_bg[3]);

    /* Initialize glyph cache */
    memset(ctx->glyph_cache, 0, sizeof(ctx->glyph_cache));

    /* Pre-calculate font metrics for consistent text height */
    osd_precalculate_font_metrics(ctx);

    return 0;
}

/* Render a single glyph and cache it */
int osd_render_glyph(osd_context_t* ctx, char c)
{
    if (!ctx || !ctx->sft || c < 0 || c >= OSD_GLYPH_CACHE_SIZE) {
        return -1;
    }

    /* Check if already cached */
    if (ctx->glyph_cache[c].bitmap) {
        return 0;
    }

    SFT_LMetrics lmetrics;
    SFT_GMetrics gmetrics;
    SFT_Glyph glyph;
    SFT_Image image_buffer;

    /* Get glyph metrics */
    if (sft_lmetrics(ctx->sft, &lmetrics) != 0) {
        IMP_LOG_ERR(TAG, "Failed to get font metrics for '%c'", c);
        return -1;
    }

    if (sft_lookup(ctx->sft, c, &glyph) != 0) {
        IMP_LOG_ERR(TAG, "Failed to lookup glyph: %c", c);
        return -1;
    }

    if (sft_gmetrics(ctx->sft, glyph, &gmetrics) != 0) {
        IMP_LOG_ERR(TAG, "Failed to get glyph metrics for: %c", c);
        return -1;
    }

    /* Prepare image buffer */
    image_buffer.width = gmetrics.minWidth;
    image_buffer.height = gmetrics.minHeight;

    image_buffer.pixels = malloc(image_buffer.width * image_buffer.height);
    if (!image_buffer.pixels) {
        IMP_LOG_ERR(TAG, "Failed to allocate image buffer for '%c'", c);
        return -1;
    }

    /* Render glyph */
    if (sft_render(ctx->sft, glyph, image_buffer) != 0) {
        IMP_LOG_ERR(TAG, "Failed to render glyph: '%c'", c);
        free(image_buffer.pixels);
        return -1;
    }

    /* Cache glyph data */
    osd_glyph_t* cached_glyph = &ctx->glyph_cache[c];
    cached_glyph->width = image_buffer.width;
    cached_glyph->height = image_buffer.height;
    cached_glyph->advance = gmetrics.advanceWidth;
    cached_glyph->xmin = gmetrics.leftSideBearing;
    cached_glyph->ymin = gmetrics.yOffset;
    cached_glyph->glyph = glyph;

    /* Convert to BGRA format */
    int bitmap_size = cached_glyph->width * cached_glyph->height * 4;
    cached_glyph->bitmap = malloc(bitmap_size);
    if (!cached_glyph->bitmap) {
        IMP_LOG_ERR(TAG, "Failed to allocate bitmap memory");
        free(image_buffer.pixels);
        return -1;
    }

    /* Convert grayscale to BGRA - simple and fast */
    for (int y = 0; y < cached_glyph->height; y++) {
        for (int x = 0; x < cached_glyph->width; x++) {
            int pixel_index = y * cached_glyph->width + x;
            uint8_t alpha = ((uint8_t*) image_buffer.pixels)[pixel_index];

            if (alpha > 0) {
                cached_glyph->bitmap[pixel_index * 4 + 0] = ctx->bgra_text[0]; /* B */
                cached_glyph->bitmap[pixel_index * 4 + 1] = ctx->bgra_text[1]; /* G */
                cached_glyph->bitmap[pixel_index * 4 + 2] = ctx->bgra_text[2]; /* R */
                cached_glyph->bitmap[pixel_index * 4 + 3] = alpha;             /* A */
            } else {
                cached_glyph->bitmap[pixel_index * 4 + 0] = 0;
                cached_glyph->bitmap[pixel_index * 4 + 1] = 0;
                cached_glyph->bitmap[pixel_index * 4 + 2] = 0;
                cached_glyph->bitmap[pixel_index * 4 + 3] = 0;
            }
        }
    }

    free(image_buffer.pixels);
    return 0;
}

/* Calculate text size using libschrift */
int osd_calculate_text_size(osd_context_t* ctx, const char* text, int* width, int* height)
{
    if (!ctx || !text || !width || !height) {
        IMP_LOG_ERR(TAG, "Invalid parameters for text size calculation");
        return -1;
    }

    if (!ctx->sft) {
        IMP_LOG_ERR(TAG, "libschrift not available - cannot calculate text size");
        return -1;
    }

    /* Get font metrics for proper height calculation */
    SFT_LMetrics lmetrics;
    if (sft_lmetrics(ctx->sft, &lmetrics) != 0) {
        IMP_LOG_ERR(TAG, "Failed to get font line metrics for size calculation");
        return -1;
    }

    *width = 0;
    *height = 0;

    for (const char* c = text; *c; c++) {
        if (*c < 0 || *c >= OSD_GLYPH_CACHE_SIZE) {
            continue;
        }

        /* Ensure glyph is cached */
        if (osd_render_glyph(ctx, *c) != 0) {
            continue;
        }

        osd_glyph_t* glyph = &ctx->glyph_cache[*c];
        *width += glyph->advance;
    }

    /* Use pre-calculated font metrics for consistent height */
    *height = ctx->font_max_height;

    *height += padding_top * 2;
    *width += padding_left * 2;

    /* Ensure width is even for BGRA alignment */
    if (*width % 2 != 0) {
        (*width)++;
    }

    /* Check if bitmap size is reasonable for embedded device */
    int bitmap_size = (*width) * (*height) * 4; /* BGRA = 4 bytes per pixel */
    const int MAX_BITMAP_SIZE = 500000;         /* 500KB limit for embedded device */
    if (bitmap_size > MAX_BITMAP_SIZE) {
        IMP_LOG_WARN(TAG,
                     "Text bitmap too large (%d bytes, %dx%d), may cause IPU buffer overflow",
                     bitmap_size,
                     *width,
                     *height);
    }

    // IMP_LOG_INFO(TAG,
    //              "Text size calculated: '%s' -> %dx%d (font_size=%d, bitmap: %d bytes)",
    //              text,
    //              *width,
    //              *height,
    //              (int) ctx->sft->yScale,
    //              bitmap_size);
    return 0;
}

/* Draw text using libschrift with consistent baseline */
int osd_draw_text(osd_context_t* ctx, uint8_t* image, const char* text, int image_width, int image_height)
{
    if (!ctx || !image || !text) {
        IMP_LOG_ERR(TAG, "Invalid parameters for text drawing");
        return -1;
    }

    if (!ctx->sft) {
        IMP_LOG_ERR(TAG, "SFT context is NULL in draw_text");
        return -1;
    }

    /* Get font metrics for proper baseline positioning */
    SFT_LMetrics lmetrics;
    if (sft_lmetrics(ctx->sft, &lmetrics) != 0) {
        IMP_LOG_ERR(TAG, "Failed to get font line metrics");
        return -1;
    }

    /* Calculate actual text width directly without padding */
    int actual_width = 0;
    for (const char* c = text; *c; c++) {
        if (*c < 0 || *c >= OSD_GLYPH_CACHE_SIZE) {
            continue;
        }
        if (osd_render_glyph(ctx, *c) != 0) {
            continue;
        }
        osd_glyph_t* glyph = &ctx->glyph_cache[*c];
        actual_width += glyph->advance;
    }

    /* Parse position for alignment */
    char position[64];
    strcpy(position, ctx->config->osd.time.position);
    char* comma = strchr(position, ',');
    int pos_x_val = 0, pos_y_val = 0;
    sscanf(position, "%d", &pos_x_val);
    if (comma) sscanf(comma + 1, "%d", &pos_y_val);

    /* Set pen_x for left or right alignment */
    int pen_x = padding_left;
    if (pos_x_val < 0) {
        pen_x = image_width - actual_width - padding_left;
    }

    /* Set baseline_y for top or bottom alignment */
    int baseline_y = (int)ceil(lmetrics.ascender);
    if (pos_y_val < 0) {
        baseline_y = image_height + (int)floor(lmetrics.descender);
    }

    // IMP_LOG_INFO(TAG,
    //              "Font metrics: ascender=%.1f, descender=%.1f, bitmap height=%d, baseline_y=%d",
    //              lmetrics.ascender,
    //              lmetrics.descender,
    //              image_height,
    //              baseline_y);

    // IMP_LOG_INFO(TAG,
    //              "Rendering text '%s' with consistent baseline at y=%d in %dx%d bitmap (pen_x=%d, actual_width=%d)",
    //              text,
    //              baseline_y,
    //              image_width,
    //              image_height,
    //              pen_x,
    //              actual_width);

    for (const char* c = text; *c; c++) {
        if (*c < 0 || *c >= OSD_GLYPH_CACHE_SIZE) {
            continue;
        }

        /* Ensure glyph is cached */
        if (osd_render_glyph(ctx, *c) != 0) {
            continue;
        }

        osd_glyph_t* glyph = &ctx->glyph_cache[*c];

        /* Position glyph relative to consistent baseline with pixel-perfect alignment */
        /* All characters use the same baseline_y, individual glyph ymin determines offset from baseline */
        int x = pen_x + glyph->xmin;
        int y = baseline_y + glyph->ymin; /* ymin is offset from baseline (negative for chars above baseline) */

        /* Draw the main glyph on top */
        for (int j = 0; j < glyph->height; j++) {
            for (int i = 0; i < glyph->width; i++) {
                int src_index = (j * glyph->width + i) * 4;
                if (glyph->bitmap[src_index + 3] > 0) { /* Check alpha */
                    int dst_x = x + i;
                    int dst_y = y + j;

                    /* Bounds check */
                    if (dst_x >= 0 && dst_x < image_width && dst_y >= 0 && dst_y < image_height) {
                        int dst_index = (dst_y * image_width + dst_x) * 4;
                        image[dst_index + 0] = glyph->bitmap[src_index + 0]; /* B */
                        image[dst_index + 1] = glyph->bitmap[src_index + 1]; /* G */
                        image[dst_index + 2] = glyph->bitmap[src_index + 2]; /* R */
                        image[dst_index + 3] = glyph->bitmap[src_index + 3]; /* A */
                    }
                }
            }
        }

        pen_x += glyph->advance;
    }

    return 0;
}

/* Cleanup libschrift resources */
int osd_libschrift_cleanup(osd_context_t* ctx)
{
    if (!ctx) {
        IMP_LOG_ERR(TAG, "Invalid OSD context for libschrift cleanup");
        return -1;
    }

    /* Free glyph cache */
    for (int i = 0; i < OSD_GLYPH_CACHE_SIZE; i++) {
        if (ctx->glyph_cache[i].bitmap) {
            free(ctx->glyph_cache[i].bitmap);
            ctx->glyph_cache[i].bitmap = NULL;
        }
    }

    /* Free SFT resources */
    if (ctx->sft) {
        if (ctx->sft->font) {
            sft_freefont(ctx->sft->font);
        }
        free(ctx->sft);
        ctx->sft = NULL;
    }

    /* Free font data */
    if (ctx->font_data) {
        free(ctx->font_data);
        ctx->font_data = NULL;
    }

    return 0;
}

/* Start OSD */
int osd_context_start(osd_context_t* ctx)
{
    if (!ctx || !ctx->initialized) {
        IMP_LOG_ERR(TAG, "Invalid OSD context for start");
        return -1;
    }

    if (ctx->started) {
        IMP_LOG_DBG(TAG, "OSD already started for group %d", ctx->group_id);
        return 0;
    }

    /* This shouldn't happen with the new system, but handle it gracefully */
    int ret = IMP_OSD_Start(ctx->group_id);
    if (ret != 0) {
        IMP_LOG_ERR(TAG, "Failed to start OSD group %d: %d", ctx->group_id, ret);
        return -1;
    }

    ctx->started = true;
    IMP_LOG_INFO(TAG, "OSD started for group %d", ctx->group_id);
    return 0;
}

/* Create text bitmap and update OSD region */
int osd_update_timestamp(osd_context_t* ctx, const char* text)
{
    if (!ctx || !text) {
        IMP_LOG_ERR(TAG, "Invalid parameters in osd_update_timestamp: ctx=%p, text=%p", ctx, text);
        return -1;
    }

    // IMP_LOG_INFO(TAG, "Updating timestamp for Group %d with text: '%s'", ctx->group_id, text);
    // IMP_LOG_INFO(TAG, "Timestamp handle: %d, group_id: %d", ctx->timestamp.handle, ctx->group_id);

    pthread_mutex_lock(&ctx->mutex);

    /* Calculate exact size for this specific text */
    int text_width, text_height;
    // IMP_LOG_INFO(TAG, "Calculating text size for Group %d", ctx->group_id);
    if (osd_calculate_text_size(ctx, text, &text_width, &text_height) != 0) {
        IMP_LOG_ERR(TAG, "Failed to calculate text size for Group %d", ctx->group_id);
        pthread_mutex_unlock(&ctx->mutex);
        return -1;
    }
    // IMP_LOG_INFO(TAG, "Text size calculated for Group %d: %dx%d", ctx->group_id, text_width, text_height);

    /* Allocate buffer for this specific text */
    int bitmap_size = text_width * text_height * 4;
    // IMP_LOG_INFO(TAG, "Allocating bitmap buffer for Group %d: %d bytes", ctx->group_id, bitmap_size);
    uint8_t* bitmap_data = (uint8_t*)malloc(bitmap_size);
    if (!bitmap_data) {
        IMP_LOG_ERR(TAG, "Failed to allocate bitmap buffer for Group %d (%d bytes)", ctx->group_id, bitmap_size);
        pthread_mutex_unlock(&ctx->mutex);
        return -1;
    }
    // IMP_LOG_INFO(TAG, "Bitmap buffer allocated successfully for Group %d", ctx->group_id);

    /* Fill background */
    IMP_LOG_INFO(TAG, "Filling background for Group %d (alpha=%d)", ctx->group_id, ctx->bgra_bg[3]);
    if (ctx->bgra_bg[3] > 0) {
        for (int i = 0; i < text_width * text_height; i++) {
            int pixel_offset = i * 4;
            bitmap_data[pixel_offset + 0] = ctx->bgra_bg[0];
            bitmap_data[pixel_offset + 1] = ctx->bgra_bg[1];
            bitmap_data[pixel_offset + 2] = ctx->bgra_bg[2];
            bitmap_data[pixel_offset + 3] = ctx->bgra_bg[3];
        }
    } else {
        memset(bitmap_data, 0, text_width * text_height * 4);
    }
    IMP_LOG_INFO(TAG, "Background filled for Group %d", ctx->group_id);

    /* Draw text into buffer */
    IMP_LOG_INFO(TAG, "Drawing text into buffer for Group %d", ctx->group_id);
    if (osd_draw_text(ctx, bitmap_data, text, text_width, text_height) != 0) {
        IMP_LOG_ERR(TAG, "Failed to draw text for Group %d", ctx->group_id);
        free(bitmap_data);
        pthread_mutex_unlock(&ctx->mutex);
        return -1;
    }
    IMP_LOG_INFO(TAG, "Text drawn successfully for Group %d", ctx->group_id);

    /* Free previous bitmap if exists */
    if (ctx->timestamp.data) {
        IMP_LOG_INFO(TAG, "Freeing previous bitmap data for Group %d", ctx->group_id);
        free(ctx->timestamp.data);
    }

    /* Update timestamp data */
    ctx->timestamp.data = bitmap_data;
    ctx->timestamp.width = text_width;
    ctx->timestamp.height = text_height;
    IMP_LOG_INFO(TAG, "Timestamp data updated for Group %d: %dx%d", ctx->group_id, text_width, text_height);

    /* Parse position from configuration */
    int pos_x, pos_y;
    IMP_LOG_INFO(TAG, "Parsing position for Group %d from config: '%s'", ctx->group_id, ctx->config->osd.time.position);
    parse_position(ctx->config->osd.time.position,
                   ctx->stream_width,
                   ctx->stream_height,
                   text_width,
                   text_height,
                   &pos_x,
                   &pos_y);
    IMP_LOG_INFO(TAG, "Position calculated for Group %d: (%d,%d)", ctx->group_id, pos_x, pos_y);

    /* Update region attributes */
    IMPOSDRgnAttr rgnAttr;
    memset(&rgnAttr, 0, sizeof(IMPOSDRgnAttr));
    rgnAttr.type = OSD_REG_PIC;

    /* Set region attributes */
    rgnAttr.rect.p0.x = pos_x;
    rgnAttr.rect.p0.y = pos_y;
    rgnAttr.rect.p1.x = pos_x + text_width - 1;
    rgnAttr.rect.p1.y = pos_y + text_height - 1;
    rgnAttr.fmt = PIX_FMT_BGRA;
    rgnAttr.data.picData.pData = ctx->timestamp.data;

    // IMP_LOG_INFO(TAG, "Setting region attributes for Group %d: rect=(%d,%d)-(%d,%d), handle=%d",
    //              ctx->group_id, rgnAttr.rect.p0.x, rgnAttr.rect.p0.y, rgnAttr.rect.p1.x, rgnAttr.rect.p1.y, ctx->timestamp.handle);

    /* Update the region attributes */
    int ret = IMP_OSD_SetRgnAttr(ctx->timestamp.handle, &rgnAttr);
    if (ret != 0) {
        IMP_LOG_ERR(TAG, "Failed to set region attributes for Group %d: %d (handle=%d)", ctx->group_id, ret, ctx->timestamp.handle);
        pthread_mutex_unlock(&ctx->mutex);
        return -1;
    }
    IMP_LOG_INFO(TAG, "Region attributes set successfully for Group %d", ctx->group_id);

    /* Show the region */
    IMP_LOG_INFO(TAG, "Showing timestamp region for Group %d (handle=%d)", ctx->group_id, ctx->timestamp.handle);
    ret = IMP_OSD_ShowRgn(ctx->timestamp.handle, ctx->group_id, 1);
    if (ret != 0) {
        IMP_LOG_ERR(TAG, "Failed to show timestamp region for Group %d: %d (handle=%d)", ctx->group_id, ret, ctx->timestamp.handle);
        pthread_mutex_unlock(&ctx->mutex);
        return -1;
    }
    IMP_LOG_INFO(TAG, "Timestamp region shown successfully for Group %d", ctx->group_id);

    ctx->last_update = time(NULL);
    pthread_mutex_unlock(&ctx->mutex);
    IMP_LOG_INFO(TAG, "Timestamp update completed successfully for Group %d", ctx->group_id);
    return 0;
}

/* Reload font configuration if font size changed */
int osd_reload_font_if_changed(osd_context_t* ctx)
{
    if (!ctx || !ctx->sft) {
        return 0;
    }

    /* Check if font size changed */
    int current_font_size = (int) ctx->sft->yScale;
    int config_font_size = ctx->config->osd.time.size;

    if (current_font_size != config_font_size) {
        IMP_LOG_INFO(TAG,
                     "Font size changed from %d to %d, reloading font for group %d",
                     current_font_size,
                     config_font_size,
                     ctx->group_id);

        /* Update font scales */
        ctx->sft->xScale = (double) config_font_size;
        ctx->sft->yScale = (double) config_font_size;

        /* Clear glyph cache to force re-rendering with new size */
        memset(ctx->glyph_cache, 0, sizeof(ctx->glyph_cache));

        /* Recalculate font metrics */
        osd_precalculate_font_metrics(ctx);

        IMP_LOG_INFO(TAG,
                     "Font reloaded with new size %d for group %d",
                     config_font_size,
                     ctx->group_id);
        return 1; /* Font changed */
    }

    return 0; /* No change */
}

/* Setup font (timestamp) region */
int osd_setup_font_region(osd_context_t* ctx)
{
    if (!ctx) {
        IMP_LOG_ERR(TAG, "Invalid context in osd_setup_font_region");
        return -1;
    }

    // IMP_LOG_INFO(TAG, "Setting up font region for Group %d, handle=%d", ctx->group_id, ctx->region_handles[OSD_REGION_FONT]);

    IMPOSDRgnAttr rAttrFont;
    memset(&rAttrFont, 0, sizeof(IMPOSDRgnAttr));
    rAttrFont.type = OSD_REG_PIC;
    /* Minimal placeholder rectangle - will be overridden by osd_update_timestamp() */
    rAttrFont.rect.p0.x = 0;
    rAttrFont.rect.p0.y = 0;
    rAttrFont.rect.p1.x = 1;
    rAttrFont.rect.p1.y = 1;
#ifdef SUPPORT_RGB555LE
    rAttrFont.fmt = PIX_FMT_RGB555LE;
#else
    rAttrFont.fmt = PIX_FMT_BGRA;
#endif
    rAttrFont.data.picData.pData = NULL;

    int ret = IMP_OSD_SetRgnAttr(ctx->region_handles[OSD_REGION_FONT], &rAttrFont);
    if (ret < 0) {
        IMP_LOG_ERR(TAG, "IMP_OSD_SetRgnAttr TimeStamp error for Group %d, handle=%d, ret=%d!",
                     ctx->group_id, ctx->region_handles[OSD_REGION_FONT], ret);
        return -1;
    }

    IMP_LOG_INFO(TAG, "Font region setup complete for Group %d, handle=%d",
                 ctx->group_id, ctx->region_handles[OSD_REGION_FONT]);
    return 0;
}

/* Load logo bitmap from file */
int osd_load_logo_bitmap(const char* path, uint8_t** bitmap, int width, int height)
{
    size_t expected_size = (size_t)width * height * 4;
    FILE* f = fopen(path, "rb");
    if (!f) {
        IMP_LOG_ERR(TAG, "Failed to open logo file: %s", path);
        return -1;
    }

    fseek(f, 0, SEEK_END);
    size_t file_size = ftell(f);
    rewind(f);

    if (file_size != expected_size) {
        IMP_LOG_ERR(TAG, "Logo file size %zu does not match expected %zu (%dx%d BGRA)", file_size, expected_size, width, height);
        fclose(f);
        return -1;
    }

    *bitmap = malloc(expected_size);
    if (!*bitmap) {
        IMP_LOG_ERR(TAG, "Failed to allocate logo buffer");
        fclose(f);
        return -1;
    }

    if (fread(*bitmap, 1, expected_size, f) != expected_size) {
        IMP_LOG_ERR(TAG, "Failed to read logo data");
        free(*bitmap);
        fclose(f);
        return -1;
    }

    fclose(f);
    return 0;
}

/* Setup logo region */
int osd_setup_logo_region(osd_context_t* ctx)
{
    if (!ctx) {
        return -1;
    }

    if (!ctx->config->osd.logo.enabled) {
        IMP_LOG_DBG(TAG, "Logo disabled in config for group %d", ctx->group_id);
        return 0;
    }

    /* Parse size */
    int picw, pich;
    if (sscanf(ctx->config->osd.logo.size, "%dx%d", &picw, &pich) != 2) {
        IMP_LOG_ERR(TAG, "Invalid logo size format: %s", ctx->config->osd.logo.size);
        return -1;
    }

    /* Load logo data */
    uint8_t* logo_data;
    if (osd_load_logo_bitmap(ctx->config->osd.logo.image, &logo_data, picw, pich) != 0) {
        IMP_LOG_ERR(TAG, "Failed to load logo from %s, disabling logo", ctx->config->osd.logo.image);
        return 0;
    }
    ctx->logo_buffer = logo_data;

    /* Parse position */
    int pos_x, pos_y;
    parse_position(ctx->config->osd.logo.position, ctx->stream_width, ctx->stream_height, picw, pich, &pos_x, &pos_y);

    IMPOSDRgnAttr rAttrLogo;
    memset(&rAttrLogo, 0, sizeof(IMPOSDRgnAttr));
    rAttrLogo.type = OSD_REG_PIC;
    rAttrLogo.rect.p0.x = pos_x;
    rAttrLogo.rect.p0.y = pos_y;
    rAttrLogo.rect.p1.x = pos_x + picw - 1;
    rAttrLogo.rect.p1.y = pos_y + pich - 1;
    rAttrLogo.fmt = PIX_FMT_BGRA;
    rAttrLogo.data.picData.pData = logo_data;

    int ret = IMP_OSD_SetRgnAttr(ctx->region_handles[OSD_REGION_LOGO], &rAttrLogo);
    if (ret < 0) {
        IMP_LOG_ERR(TAG, "IMP_OSD_SetRgnAttr Logo error!");
        return -1;
    }

    IMPOSDGrpRgnAttr grAttrLogo;
    if (IMP_OSD_GetGrpRgnAttr(ctx->region_handles[OSD_REGION_LOGO], ctx->group_id, &grAttrLogo)
        < 0) {
        IMP_LOG_ERR(TAG, "IMP_OSD_GetGrpRgnAttr Logo error!");
        return -1;
    }

    memset(&grAttrLogo, 0, sizeof(IMPOSDGrpRgnAttr));
    grAttrLogo.show = 1;
    grAttrLogo.gAlphaEn = 1;
    grAttrLogo.fgAlhpa = (int)(ctx->config->osd.logo.opacity * 255);
    grAttrLogo.layer = 2;

    if (IMP_OSD_SetGrpRgnAttr(ctx->region_handles[OSD_REGION_LOGO], ctx->group_id, &grAttrLogo)
        < 0) {
        IMP_LOG_ERR(TAG, "IMP_OSD_SetGrpRgnAttr Logo error!");
        return -1;
    }

    IMP_LOG_INFO(TAG, "Logo setup complete for group %d: %dx%d at (%d,%d) from %s, opacity %.2f",
                 ctx->group_id, picw, pich, pos_x, pos_y, ctx->config->osd.logo.image, ctx->config->osd.logo.opacity);

    return 0;
}

/* Setup cover region */
int osd_setup_cover_region(osd_context_t* ctx)
{
    if (!ctx) {
        return -1;
    }

    IMPOSDRgnAttr rAttrCover;
    memset(&rAttrCover, 0, sizeof(IMPOSDRgnAttr));
    rAttrCover.type = OSD_REG_COVER;
    rAttrCover.rect.p0.x = ctx->stream_width / 2 - 100;
    rAttrCover.rect.p0.y = ctx->stream_height / 2 - 100;
    rAttrCover.rect.p1.x = rAttrCover.rect.p0.x + ctx->stream_width / 2 - 1 + 50;
    rAttrCover.rect.p1.y = rAttrCover.rect.p0.y + ctx->stream_height / 2 - 1 + 50;
    rAttrCover.fmt = PIX_FMT_BGRA;
    rAttrCover.data.coverData.color = OSD_BLACK;

    int ret = IMP_OSD_SetRgnAttr(ctx->region_handles[OSD_REGION_COVER], &rAttrCover);
    if (ret < 0) {
        IMP_LOG_ERR(TAG, "IMP_OSD_SetRgnAttr Cover error!");
        return -1;
    }

    IMPOSDGrpRgnAttr grAttrCover;
    if (IMP_OSD_GetGrpRgnAttr(ctx->region_handles[OSD_REGION_COVER], ctx->group_id, &grAttrCover)
        < 0) {
        IMP_LOG_ERR(TAG, "IMP_OSD_GetGrpRgnAttr Cover error!");
        return -1;
    }

    memset(&grAttrCover, 0, sizeof(IMPOSDGrpRgnAttr));
    grAttrCover.show = 0;
    grAttrCover.layer = 1;

    if (IMP_OSD_SetGrpRgnAttr(ctx->region_handles[OSD_REGION_COVER], ctx->group_id, &grAttrCover)
        < 0) {
        IMP_LOG_ERR(TAG, "IMP_OSD_SetGrpRgnAttr Cover error!");
        return -1;
    }

    return 0;
}

/* Setup rect region */
int osd_setup_rect_region(osd_context_t* ctx)
{
    if (!ctx) {
        return -1;
    }

    IMPOSDRgnAttr rAttrRect;
    memset(&rAttrRect, 0, sizeof(IMPOSDRgnAttr));
    rAttrRect.type = OSD_REG_RECT;
    rAttrRect.rect.p0.x = ctx->stream_width / 2 + 100;
    rAttrRect.rect.p0.y = ctx->stream_height / 2 + 100;
    rAttrRect.rect.p1.x = rAttrRect.rect.p0.x + ctx->stream_width / 2 - 1 - 100;
    rAttrRect.rect.p1.y = rAttrRect.rect.p0.y + ctx->stream_height / 2 - 1 - 100;
    rAttrRect.fmt = PIX_FMT_BGRA;
    /* Note: rectData structure may vary by SDK version - using basic setup */

    int ret = IMP_OSD_SetRgnAttr(ctx->region_handles[OSD_REGION_RECT], &rAttrRect);
    if (ret < 0) {
        IMP_LOG_ERR(TAG, "IMP_OSD_SetRgnAttr Rect error!");
        return -1;
    }

    IMPOSDGrpRgnAttr grAttrRect;
    if (IMP_OSD_GetGrpRgnAttr(ctx->region_handles[OSD_REGION_RECT], ctx->group_id, &grAttrRect)
        < 0) {
        IMP_LOG_ERR(TAG, "IMP_OSD_GetGrpRgnAttr Rect error!");
        return -1;
    }

    memset(&grAttrRect, 0, sizeof(IMPOSDGrpRgnAttr));
    grAttrRect.show = 0;
    grAttrRect.layer = 0;

    if (IMP_OSD_SetGrpRgnAttr(ctx->region_handles[OSD_REGION_RECT], ctx->group_id, &grAttrRect)
        < 0) {
        IMP_LOG_ERR(TAG, "IMP_OSD_SetGrpRgnAttr Rect error!");
        return -1;
    }

    return 0;
}

/* Setup info region */
int osd_setup_info_region(osd_context_t* ctx)
{
    if (!ctx) {
        return -1;
    }

    IMPOSDRgnAttr rAttrInfo;
    memset(&rAttrInfo, 0, sizeof(IMPOSDRgnAttr));
    rAttrInfo.type = OSD_REG_PIC;
    rAttrInfo.rect.p0.x = 0;
    rAttrInfo.rect.p0.y = 0;
    rAttrInfo.rect.p1.x = 1;
    rAttrInfo.rect.p1.y = 1;
#ifdef SUPPORT_RGB555LE
    rAttrInfo.fmt = PIX_FMT_RGB555LE;
#else
    rAttrInfo.fmt = PIX_FMT_BGRA;
#endif
    rAttrInfo.data.picData.pData = NULL;

    int ret = IMP_OSD_SetRgnAttr(ctx->region_handles[OSD_REGION_INFO], &rAttrInfo);
    if (ret < 0) {
        IMP_LOG_ERR(TAG, "IMP_OSD_SetRgnAttr Info error!");
        return -1;
    }

    IMPOSDGrpRgnAttr grAttrInfo;
    memset(&grAttrInfo, 0, sizeof(IMPOSDGrpRgnAttr));
    grAttrInfo.show = 0;
    grAttrInfo.layer = 3;
    grAttrInfo.scalex = 1;
    grAttrInfo.scaley = 1;
    grAttrInfo.gAlphaEn = 1;
    grAttrInfo.fgAlhpa = 255;

    ret = IMP_OSD_SetGrpRgnAttr(ctx->region_handles[OSD_REGION_INFO], ctx->group_id, &grAttrInfo);
    if (ret < 0) {
        IMP_LOG_ERR(TAG, "IMP_OSD_SetGrpRgnAttr Info error!");
        return -1;
    }

    return 0;
}

/* Update info display with photosensitive parameters */
int osd_update_info_display(osd_context_t* ctx, float iso, float gb_gain, float gr_gain, bool night_mode)
{
    if (!ctx) {
        return -1;
    }

    char info_text[256];
    snprintf(info_text, sizeof(info_text),
             "ISO: %.0f | GB: %.1f | GR: %.1f | Mode: %s",
             iso, gb_gain, gr_gain, night_mode ? "NIGHT" : "DAY");

    pthread_mutex_lock(&ctx->mutex);

    int text_width, text_height;
    if (osd_calculate_text_size(ctx, info_text, &text_width, &text_height) != 0) {
        pthread_mutex_unlock(&ctx->mutex);
        return -1;
    }

    uint8_t* bitmap_data = (uint8_t*)malloc(text_width * text_height * 4);
    if (!bitmap_data) {
        pthread_mutex_unlock(&ctx->mutex);
        return -1;
    }

    memset(bitmap_data, 0, text_width * text_height * 4);

    if (osd_draw_text(ctx, bitmap_data, info_text, text_width, text_height) != 0) {
        free(bitmap_data);
        pthread_mutex_unlock(&ctx->mutex);
        return -1;
    }

    if (ctx->info.data) {
        free(ctx->info.data);
    }
    ctx->info.data = bitmap_data;
    ctx->info.width = text_width;
    ctx->info.height = text_height;

    /* Position at bottom left */
    int pos_x = 10;
    int pos_y = ctx->stream_height - text_height - 10;

    IMPOSDRgnAttr rgnAttr;
    memset(&rgnAttr, 0, sizeof(IMPOSDRgnAttr));
    rgnAttr.type = OSD_REG_PIC;
    rgnAttr.rect.p0.x = pos_x;
    rgnAttr.rect.p0.y = pos_y;
    rgnAttr.rect.p1.x = pos_x + text_width - 1;
    rgnAttr.rect.p1.y = pos_y + text_height - 1;
    rgnAttr.fmt = PIX_FMT_BGRA;
    rgnAttr.data.picData.pData = ctx->info.data;

    int ret = IMP_OSD_SetRgnAttr(ctx->region_handles[OSD_REGION_INFO], &rgnAttr);
    if (ret != 0) {
        pthread_mutex_unlock(&ctx->mutex);
        return -1;
    }

    ret = IMP_OSD_ShowRgn(ctx->region_handles[OSD_REGION_INFO], ctx->group_id, 1);
    if (ret != 0) {
        pthread_mutex_unlock(&ctx->mutex);
        return -1;
    }

    ctx->info.enabled = true;
    pthread_mutex_unlock(&ctx->mutex);
    return 0;
}

/* Update OSD (called periodically) */
int osd_context_update(osd_context_t* ctx)
{
    if (!ctx || !ctx->started) {
        IMP_LOG_DBG(TAG,
                    "OSD context update skipped - ctx=%p, started=%d",
                    ctx,
                    ctx ? ctx->started : 0);
        return 0;
    }

    IMP_LOG_DBG(TAG,
                "OSD context update for group %d - time_enabled=%d",
                ctx->group_id,
                ctx->config->osd.time.enabled);

    /* Check if font configuration changed */
    osd_reload_font_if_changed(ctx);

    /* Update timestamp if enabled */
    if (ctx->config->osd.time.enabled) {
        // IMP_LOG_DBG(TAG, "Updating timestamp for group %d (enabled)", ctx->group_id);
        osd_update_timestamp_current(ctx);
    } else {
        // IMP_LOG_DBG(TAG, "Timestamp disabled for group %d, skipping update", ctx->group_id);
    }

    return 0;
}

/* Stop OSD */
int osd_context_stop(osd_context_t* ctx)
{
    if (!ctx || !ctx->started) {
        return 0;
    }

    int ret = IMP_OSD_Stop(ctx->group_id);
    if (ret != 0) {
        IMP_LOG_ERR(TAG, "Failed to stop OSD group %d: %d", ctx->group_id, ret);
        return -1;
    }

    ctx->started = false;

    IMP_LOG_INFO(TAG, "OSD stopped for group %d", ctx->group_id);

    return 0;
}

/* Cleanup OSD */
int osd_context_cleanup(osd_context_t* ctx)
{
    if (!ctx) {
        IMP_LOG_ERR(TAG, "Invalid OSD context for cleanup");
        return -1;
    }

    /* Remove from global array */
    if (ctx->group_id >= 0 && ctx->group_id < MAX_STREAMS) {
        g_osd_contexts[ctx->group_id] = NULL;
    }

    int ret;

    /* Stop if running */
    if (ctx->started) {
        osd_context_stop(ctx);
    }

    /* Step 1: Hide all regions */
    for (int i = 0; i < OSD_REGION_COUNT; i++) {
        if (ctx->region_handles[i] != INVHANDLE) {
            ret = IMP_OSD_ShowRgn(ctx->region_handles[i], ctx->group_id, 0);
            if (ret < 0) {
                IMP_LOG_ERR(TAG, "IMP_OSD_ShowRgn close region %d error", i);
            }
        }
    }

    /* Step 2: Unregister all regions */
    for (int i = 0; i < OSD_REGION_COUNT; i++) {
        if (ctx->region_handles[i] != INVHANDLE) {
            ret = IMP_OSD_UnRegisterRgn(ctx->region_handles[i], ctx->group_id);
            if (ret < 0) {
                IMP_LOG_ERR(TAG, "IMP_OSD_UnRegisterRgn region %d error", i);
            }
        }
    }

    /* Step 3: Destroy all regions */
    for (int i = 0; i < OSD_REGION_COUNT; i++) {
        if (ctx->region_handles[i] != INVHANDLE) {
            IMP_OSD_DestroyRgn(ctx->region_handles[i]);
            ctx->region_handles[i] = INVHANDLE;
        }
    }

    /* Step 4: Destroy group */
    ret = IMP_OSD_DestroyGroup(ctx->group_id);
    if (ret < 0) {
        IMP_LOG_ERR(TAG, "IMP_OSD_DestroyGroup(%d) error", ctx->group_id);
    }

    /* Free logo buffer */
    if (ctx->logo_buffer) {
        free(ctx->logo_buffer);
        ctx->logo_buffer = NULL;
    }

    /* Destroy mutex */
    pthread_mutex_destroy(&ctx->mutex);

    /* Cleanup libschrift resources */
    osd_libschrift_cleanup(ctx);

    ctx->initialized = false;
    ctx->started = false;

    IMP_LOG_INFO(TAG, "OSD cleanup completed for group %d", ctx->group_id);
    return 0;
}

/* ========== BRIDGE FUNCTIONS FOR STREAMER COMPATIBILITY ========== */

/* Global OSD contexts array */
//osd_context_t* g_osd_contexts[MAX_STREAMS] = {NULL};

/* Apply module configuration to stream configuration */
static void osd_apply_module_config_to_stream(stream_config_t* stream_config, int stream_index)
{
    if (!stream_config || stream_index < 0 || stream_index >= 2) {
        return;
    }

    if (!g_osd_module_state.initialized) {
        IMP_LOG_WARN(TAG, "OSD module not initialized, cannot apply config");
        return;
    }

    const osd_module_config_t* module_config = &g_osd_module_state.config;

    /* Apply module config to stream config */
    stream_config->osd.enabled = module_config->streams[stream_index].enabled;

    /* Time settings */
    stream_config->osd.time.enabled = module_config->streams[stream_index].time.enabled;
    strncpy(stream_config->osd.time.format, module_config->streams[stream_index].time.format,
            sizeof(stream_config->osd.time.format) - 1);
    strncpy(stream_config->osd.time.font, module_config->streams[stream_index].time.font,
            sizeof(stream_config->osd.time.font) - 1);
    stream_config->osd.time.size = module_config->streams[stream_index].time.size;
    strncpy(stream_config->osd.time.color, module_config->streams[stream_index].time.color,
            sizeof(stream_config->osd.time.color) - 1);
    strncpy(stream_config->osd.time.background, module_config->streams[stream_index].time.background,
            sizeof(stream_config->osd.time.background) - 1);
    strncpy(stream_config->osd.time.position, module_config->streams[stream_index].time.position,
            sizeof(stream_config->osd.time.position) - 1);

    /* Logo settings */
    stream_config->osd.logo.enabled = module_config->streams[stream_index].logo.enabled;
    strncpy(stream_config->osd.logo.image, module_config->streams[stream_index].logo.image,
            sizeof(stream_config->osd.logo.image) - 1);
    strncpy(stream_config->osd.logo.size, module_config->streams[stream_index].logo.size,
            sizeof(stream_config->osd.logo.size) - 1);
    strncpy(stream_config->osd.logo.position, module_config->streams[stream_index].logo.position,
            sizeof(stream_config->osd.logo.position) - 1);
    stream_config->osd.logo.opacity = module_config->streams[stream_index].logo.opacity;

    /* Note: Info settings not supported in legacy stream config structure */

    IMP_LOG_INFO(TAG, "Applied module config to stream %d: enabled=%s, time_enabled=%s, format='%s'",
                stream_index,
                stream_config->osd.enabled ? "true" : "false",
                stream_config->osd.time.enabled ? "true" : "false",
                stream_config->osd.time.format);
}

/* Apply motion zones configuration from module config to context */
static void osd_apply_motion_zones_config(osd_context_t* ctx, int stream_index)
{
    if (!ctx || stream_index < 0 || stream_index >= 2) {
        return;
    }

    /* Get motion zones config from module config */
    osd_module_config_t* module_config = &g_osd_module_state.config;

    ctx->motion_zones.enabled = module_config->streams[stream_index].motion_zones.enabled;
    ctx->motion_zones.show_include_zones = module_config->streams[stream_index].motion_zones.show_include_zones;
    ctx->motion_zones.show_exclude_zones = module_config->streams[stream_index].motion_zones.show_exclude_zones;
    ctx->motion_zones.line_width = module_config->streams[stream_index].motion_zones.line_width;

    /* Parse color strings directly to BGRA format */
    if (strlen(module_config->streams[stream_index].motion_zones.include_color) > 0) {
        const char* color_str = module_config->streams[stream_index].motion_zones.include_color;
        if (color_str[0] == '#' && strlen(color_str) == 9) {
            uint32_t rgba_color = (uint32_t)strtoul(color_str + 1, NULL, 16);
            /* Convert RGBA to BGRA format */
            ctx->motion_zones.include_color = ((rgba_color & 0xFF000000)) |        /* A */
                                             ((rgba_color & 0x00FF0000) >> 16) |   /* R -> B */
                                             ((rgba_color & 0x0000FF00)) |         /* G */
                                             ((rgba_color & 0x000000FF) << 16);    /* B -> R */
        }
    }
    if (strlen(module_config->streams[stream_index].motion_zones.exclude_color) > 0) {
        const char* color_str = module_config->streams[stream_index].motion_zones.exclude_color;
        if (color_str[0] == '#' && strlen(color_str) == 9) {
            uint32_t rgba_color = (uint32_t)strtoul(color_str + 1, NULL, 16);
            /* Convert RGBA to BGRA format */
            ctx->motion_zones.exclude_color = ((rgba_color & 0xFF000000)) |        /* A */
                                             ((rgba_color & 0x00FF0000) >> 16) |   /* R -> B */
                                             ((rgba_color & 0x0000FF00)) |         /* G */
                                             ((rgba_color & 0x000000FF) << 16);    /* B -> R */
        }
    }

    IMP_LOG_INFO(TAG, "Applied motion zones config to stream %d: enabled=%s, include_color=0x%08X, exclude_color=0x%08X",
                stream_index,
                ctx->motion_zones.enabled ? "true" : "false",
                ctx->motion_zones.include_color,
                ctx->motion_zones.exclude_color);
}

/* Initialize OSD for a specific group - bridge function */
int osd_init(int group_id, int stream_width, int stream_height)
{
    IMP_LOG_INFO(TAG, "Initializing OSD for Group %d (%dx%d)", group_id, stream_width, stream_height);

    if (group_id < 0 || group_id >= FS_CHN_NUM) {
        IMP_LOG_ERR(TAG, "Invalid Group ID: %d (FS_CHN_NUM=%d)", group_id, FS_CHN_NUM);
        return -1;
    }

    /* Check if this channel is enabled */
    extern struct chn_conf chn[FS_CHN_NUM];
    if (!chn[group_id].enable) {
        IMP_LOG_INFO(TAG, "Channel %d is not enabled, skipping OSD initialization", group_id);
        return 0;
    }
    IMP_LOG_INFO(TAG, "Channel %d is enabled", group_id);

    /* Skip OSD for channel 3 as it might be a duplicate/auxiliary channel */
    if (group_id >= 3) {
        IMP_LOG_INFO(TAG, "Skipping OSD for auxiliary channel %d", group_id);
        return 0;
    }

    /* Use global configuration */
    extern struct streamer_config* g_config;
    if (!g_config) {
        IMP_LOG_ERR(TAG, "Global OSD configuration is not available");
        return -1;
    }
    IMP_LOG_INFO(TAG, "Global config available, stream_count=%d", g_config->stream_count);

    /* Map group_id to stream configuration */
    stream_config_t* stream_config = NULL;
    if (group_id < g_config->stream_count) {
        stream_config = &g_config->streams[group_id];
        IMP_LOG_INFO(TAG, "Using stream%d configuration for channel %d", group_id, group_id);
    } else {
        /* For channels beyond stream count, use the last available stream config */
        int last_stream = g_config->stream_count - 1;
        if (last_stream >= 0) {
            stream_config = &g_config->streams[last_stream];
            IMP_LOG_INFO(TAG, "Using stream%d configuration for channel %d (fallback)", last_stream, group_id);
        } else {
            IMP_LOG_ERR(TAG, "No stream configuration available for channel %d", group_id);
            return -1;
        }
    }

    /* Check if OSD module is enabled and this stream is enabled */
    IMP_LOG_INFO(TAG, "OSD module state - initialized=%s, enabled=%s",
                 g_osd_module_state.initialized ? "true" : "false",
                 g_osd_module_state.config.enabled ? "true" : "false");
    if (!g_osd_module_state.initialized || !g_osd_module_state.config.enabled) {
        IMP_LOG_INFO(TAG, "OSD module disabled, skipping channel %d", group_id);
        return 0;
    }

    /* Check if this specific stream is enabled in module config */
    if (group_id >= 2) {
        IMP_LOG_INFO(TAG, "Channel %d >= 2, skipping OSD", group_id);
        return 0;
    }

    bool stream_enabled = g_osd_module_state.config.streams[group_id].enabled;
    IMP_LOG_INFO(TAG, "Stream %d enabled in module config: %s", group_id, stream_enabled ? "true" : "false");
    if (!stream_enabled) {
        IMP_LOG_INFO(TAG, "OSD disabled for channel %d in module config", group_id);
        return 0;
    }

    /* Apply module configuration to stream configuration */
    IMP_LOG_INFO(TAG, "Applying module configuration to stream %d", group_id);
    osd_apply_module_config_to_stream(stream_config, group_id);

    /* Allocate OSD context */
    IMP_LOG_INFO(TAG, "Allocating OSD context for group %d", group_id);
    osd_context_t* ctx = (osd_context_t*)malloc(sizeof(osd_context_t));
    if (!ctx) {
        IMP_LOG_ERR(TAG, "Failed to allocate OSD context for group %d", group_id);
        return -1;
    }
    IMP_LOG_INFO(TAG, "OSD context allocated successfully for group %d", group_id);

    /* Initialize OSD context */
    IMP_LOG_INFO(TAG, "Initializing OSD context for group %d", group_id);
    int ret = osd_context_init(ctx, group_id, stream_width, stream_height, stream_config);
    if (ret != 0) {
        IMP_LOG_ERR(TAG, "Failed to initialize OSD context for group %d", group_id);
        free(ctx);
        return -1;
    }

    /* Apply motion zones configuration */
    IMP_LOG_INFO(TAG, "Applying motion zones configuration for group %d", group_id);
    osd_apply_motion_zones_config(ctx, group_id);

    /* Motion zones will be updated by motion module after it completes initialization */
    if (ctx->motion_zones.enabled) {
        IMP_LOG_INFO(TAG, "Motion zones enabled in configuration, waiting for motion module to initialize zones for group %d", group_id);
    }

    IMP_LOG_INFO(TAG, "OSD initialized successfully for Group %d - COMPLETE SUCCESS", group_id);
    return 0;
}

/* Set timestamp OSD - bridge function */
int osd_set_timestamp(int group_id, const char* timestamp)
{
    // IMP_LOG_DBG(TAG, "Setting Timestamp OSD for Group %d: %s", group_id, timestamp);

    if (group_id < 0 || group_id >= FS_CHN_NUM || !g_osd_contexts[group_id]) {
        IMP_LOG_ERR(TAG, "Invalid Group ID or uninitialized OSD context: %d", group_id);
        return -1;
    }

    osd_context_t* ctx = g_osd_contexts[group_id];

    /* Check if timestamp is enabled in configuration */
    if (!ctx->config->osd.time.enabled) {
        IMP_LOG_DBG(TAG, "Timestamp OSD disabled in config for Group %d", group_id);
        return 0; /* Not an error - just skip */
    }

    /* Update timestamp with provided text */
    return osd_update_timestamp(ctx, timestamp);
}

/* Set logo OSD - bridge function */
int osd_set_logo(int group_id, int x, int y, const uint8_t* logo_data, int width, int height)
{
    // IMP_LOG_DBG(TAG, "Setting Logo OSD for Group %d at (%d,%d) size %dx%d", group_id, x, y, width, height);

    if (group_id < 0 || group_id >= FS_CHN_NUM || !g_osd_contexts[group_id]) {
        IMP_LOG_ERR(TAG, "Invalid Group ID or uninitialized OSD context: %d", group_id);
        return -1;
    }

    osd_context_t* ctx = g_osd_contexts[group_id];

    /* Check if logo is enabled in configuration */
    if (!ctx->config->osd.logo.enabled) {
        IMP_LOG_DBG(TAG, "Logo OSD disabled in config for Group %d", group_id);
        return 0; /* Not an error - just skip */
    }

    /* Show the logo region that was created during initialization */
    int ret = IMP_OSD_ShowRgn(ctx->region_handles[OSD_REGION_LOGO], group_id, 1);
    if (ret != 0) {
        IMP_LOG_ERR(TAG, "Failed to show logo region for Group %d: %d (handle=%d)", group_id, ret, ctx->region_handles[OSD_REGION_LOGO]);
        return -1;
    }

    IMP_LOG_INFO(TAG, "Logo OSD enabled for Group %d", group_id);
    return 0;
}

/* Set text OSD - bridge function */
int osd_set_text(int group_id, int x, int y, const char* text)
{
    // IMP_LOG_DBG(TAG, "Setting Text OSD for Group %d at (%d,%d): %s", group_id, x, y, text);

    if (group_id < 0 || group_id >= FS_CHN_NUM || !g_osd_contexts[group_id]) {
        IMP_LOG_ERR(TAG, "Invalid Group ID or uninitialized OSD context: %d", group_id);
        return -1;
    }

    osd_context_t* ctx = g_osd_contexts[group_id];

    /* For now, treat text OSD as timestamp OSD with custom text */
    /* Update position in configuration if provided */
    if (x >= 0 && y >= 0) {
        snprintf(ctx->config->osd.time.position, sizeof(ctx->config->osd.time.position), "%d,%d", x, y);
    }

    /* Enable timestamp if not already enabled */
    if (!ctx->config->osd.time.enabled) {
        ctx->config->osd.time.enabled = true;
        IMP_LOG_INFO(TAG, "Enabled text OSD for Group %d", group_id);
    }

    /* Create text bitmap and update region */
    uint8_t* bitmap_data;
    int bitmap_width, bitmap_height;
    if (osd_create_text_bitmap(ctx, text, &bitmap_data, &bitmap_width, &bitmap_height) != 0) {
        IMP_LOG_ERR(TAG, "Failed to create text bitmap for Group %d", group_id);
        return -1;
    }

    /* Update timestamp region with custom text */
    if (ctx->timestamp.data) {
        free(ctx->timestamp.data);
    }
    ctx->timestamp.data = bitmap_data;
    ctx->timestamp.width = bitmap_width;
    ctx->timestamp.height = bitmap_height;

    /* Update region attributes */
    IMPOSDRgnAttr rgnAttr;
    memset(&rgnAttr, 0, sizeof(IMPOSDRgnAttr));
    rgnAttr.type = OSD_REG_PIC;

    /* Parse position from configuration */
    int pos_x, pos_y;
    parse_position(ctx->config->osd.time.position,
                   ctx->stream_width,
                   ctx->stream_height,
                   ctx->timestamp.width,
                   ctx->timestamp.height,
                   &pos_x,
                   &pos_y);

    /* Set region attributes */
    rgnAttr.rect.p0.x = pos_x;
    rgnAttr.rect.p0.y = pos_y;
    rgnAttr.rect.p1.x = pos_x + ctx->timestamp.width - 1;
    rgnAttr.rect.p1.y = pos_y + ctx->timestamp.height - 1;
    rgnAttr.fmt = PIX_FMT_BGRA;
    rgnAttr.data.picData.pData = ctx->timestamp.data;

    /* Update the region attributes */
    int ret = IMP_OSD_SetRgnAttr(ctx->timestamp.handle, &rgnAttr);
    if (ret != 0) {
        IMP_LOG_ERR(TAG, "Failed to set region attributes: %d", ret);
        return -1;
    }

    /* Show the region */
    ret = IMP_OSD_ShowRgn(ctx->timestamp.handle, ctx->group_id, 1);
    if (ret != 0) {
        IMP_LOG_ERR(TAG, "Failed to show text region for Group %d: %d (handle=%d)", group_id, ret, ctx->timestamp.handle);
        return -1;
    }

    // IMP_LOG_INFO(TAG, "Text OSD set successfully for Group %d", group_id);
    return 0;
}

/* Update OSD (called periodically) - bridge function */
int osd_update(int group_id)
{
    if (group_id < 0 || group_id >= FS_CHN_NUM || !g_osd_contexts[group_id]) {
        return 0; /* Silently skip if not initialized */
    }

    return osd_context_update(g_osd_contexts[group_id]);
}

/* Cleanup OSD - bridge function */
int osd_cleanup(int group_id)
{
    IMP_LOG_INFO(TAG, "Cleaning up OSD for Group %d", group_id);

    if (group_id < 0 || group_id >= FS_CHN_NUM) {
        IMP_LOG_ERR(TAG, "Invalid Group ID: %d", group_id);
        return -1;
    }

    if (!g_osd_contexts[group_id]) {
        IMP_LOG_DBG(TAG, "OSD context for group %d already cleaned up", group_id);
        return 0;
    }

    /* Cleanup timestamp region first */
    osd_context_t* ctx = g_osd_contexts[group_id];
    if (ctx->timestamp.handle != INVHANDLE) {
        IMP_OSD_ShowRgn(ctx->timestamp.handle, group_id, 0);
        IMP_OSD_UnRegisterRgn(ctx->timestamp.handle, group_id);
        IMP_OSD_DestroyRgn(ctx->timestamp.handle);
        ctx->timestamp.handle = INVHANDLE;
    }

    if (ctx->timestamp.data) {
        free(ctx->timestamp.data);
        ctx->timestamp.data = NULL;
    }

    if (ctx->timestamp.rgnAttrData) {
        free(ctx->timestamp.rgnAttrData);
        ctx->timestamp.rgnAttrData = NULL;
    }

    /* Cleanup OSD context */
    osd_context_cleanup(ctx);

    /* Free context */
    free(g_osd_contexts[group_id]);
    g_osd_contexts[group_id] = NULL;

    IMP_LOG_INFO(TAG, "OSD cleanup completed for group %d", group_id);

    return 0;
}

/* Cleanup all OSD contexts - bridge function */
void osd_cleanup_all(void)
{
    IMP_LOG_INFO(TAG, "Cleaning up all OSD contexts");

    for (int i = 0; i < MAX_STREAMS; i++) {
        if (g_osd_contexts[i]) {
            IMP_LOG_INFO(TAG, "Cleaning up OSD context for group %d", i);
            osd_context_cleanup(g_osd_contexts[i]);
            free(g_osd_contexts[i]);
            g_osd_contexts[i] = NULL;
        }
    }

    IMP_LOG_INFO(TAG, "All OSD contexts cleaned up");
}

/* Update timestamp with current time */
int osd_update_timestamp_current(osd_context_t* ctx)
{
    if (!ctx) {
        IMP_LOG_ERR(TAG, "osd_update_timestamp_current called with NULL context");
        return 0;
    }

    if (!ctx->config->osd.time.enabled) {
        IMP_LOG_DBG(TAG, "Timestamp disabled for group %d, skipping update", ctx->group_id);
        return 0;
    }
    time_t now;
    char timestamp[64];
    time(&now);
    struct tm* tm_info = localtime(&now);

    const char* format = ctx->validated_time_format;
    if (strftime(timestamp, sizeof(timestamp), format, tm_info) == 0) {
        IMP_LOG_ERR(TAG, "strftime failed with validated format '%s' for group %d", format, ctx->group_id);
        return -1;
    }

    /* Cache timestamp string to prevent duplicate bitmap generation */
    static char last_timestamp[64] = "";
    static time_t last_timestamp_time = 0;

    /* Only update if timestamp string actually changed */
    if (now != last_timestamp_time || strcmp(timestamp, last_timestamp) != 0) {
        /* Use the same time for debugging to avoid race conditions */
        char debug_time[32];
        strftime(debug_time, sizeof(debug_time), "%H:%M:%S", tm_info);
        // IMP_LOG_INFO(TAG, "Setting Timestamp OSD for Group %d: %s (system_time=%s)", ctx->group_id, timestamp, debug_time);

        /* Update cache with new timestamp */
        strncpy(last_timestamp, timestamp, sizeof(last_timestamp) - 1);
        last_timestamp[sizeof(last_timestamp) - 1] = '\0';
        last_timestamp_time = now;
    } else {
        /* Timestamp hasn't changed, skip bitmap regeneration */
        // IMP_LOG_DBG(TAG, "Timestamp unchanged for group %d, skipping update", ctx->group_id);
        return 0;
    }

    /* Create text bitmap */
    uint8_t* bitmap_data;
    int bitmap_width, bitmap_height;
    if (osd_create_text_bitmap(ctx, timestamp, &bitmap_data, &bitmap_width, &bitmap_height) != 0) {
        IMP_LOG_ERR(TAG, "Failed to create text bitmap for Group %d", ctx->group_id);
        return -1;
    }

    /* Update timestamp region with new bitmap */
    if (ctx->timestamp.data) {
        free(ctx->timestamp.data);
    }
    ctx->timestamp.data = bitmap_data;
    ctx->timestamp.width = bitmap_width;
    ctx->timestamp.height = bitmap_height;

    /* Update region attributes */
    IMPOSDRgnAttr rgnAttr;
    memset(&rgnAttr, 0, sizeof(IMPOSDRgnAttr));
    rgnAttr.type = OSD_REG_PIC;

    /* Parse position from configuration */
    int pos_x, pos_y;
    parse_position(ctx->config->osd.time.position,
                   ctx->stream_width,
                   ctx->stream_height,
                   ctx->timestamp.width,
                   ctx->timestamp.height,
                   &pos_x,
                   &pos_y);

    /* Set region attributes */
    rgnAttr.rect.p0.x = pos_x;
    rgnAttr.rect.p0.y = pos_y;
    rgnAttr.rect.p1.x = pos_x + ctx->timestamp.width - 1;
    rgnAttr.rect.p1.y = pos_y + ctx->timestamp.height - 1;
    rgnAttr.fmt = PIX_FMT_BGRA;
    rgnAttr.data.picData.pData = ctx->timestamp.data;

    /* Update the region attributes */
    int ret = IMP_OSD_SetRgnAttr(ctx->timestamp.handle, &rgnAttr);
    if (ret != 0) {
        IMP_LOG_ERR(TAG, "Failed to set region attributes: %d", ret);
        return -1;
    }

    /* Set group region attributes for alpha transparency */
    IMPOSDGrpRgnAttr grAttr;
    memset(&grAttr, 0, sizeof(IMPOSDGrpRgnAttr));
    grAttr.show = 1;
    grAttr.layer = 1;  /* Layer 1 for timestamp */
    grAttr.scalex = 1;
    grAttr.scaley = 1;
    grAttr.gAlphaEn = 1;  /* Enable global alpha */

    /* Extract alpha from text color configuration */
    uint32_t font_color = 0xFFFFFFFF;
    const char* color_str = ctx->config->osd.time.color;
    if (color_str && color_str[0] == '#' && strlen(color_str) == 9) {
        font_color = (uint32_t) strtoul(color_str + 1, NULL, 16);
    }
    grAttr.fgAlhpa = font_color & 0xFF;  /* Use alpha from color config */

    ret = IMP_OSD_SetGrpRgnAttr(ctx->timestamp.handle, ctx->group_id, &grAttr);
    if (ret != 0) {
        IMP_LOG_ERR(TAG, "Failed to set group region attributes: %d", ret);
        return -1;
    }

    /* Show the region */
    ret = IMP_OSD_ShowRgn(ctx->timestamp.handle, ctx->group_id, 1);
    if (ret != 0) {
        IMP_LOG_ERR(TAG, "Failed to show timestamp region for Group %d: %d (handle=%d)", ctx->group_id, ret, ctx->timestamp.handle);
        return -1;
    }

    return 0;
}

/* Module interface implementation */

int osd_module_init(void* config)
{
    IMP_LOG_INFO(TAG, "Initializing OSD module");

    if (g_osd_module_state.initialized) {
        IMP_LOG_WARN(TAG, "OSD module already initialized");
        return 0;
    }

    if (!config) {
        IMP_LOG_ERR(TAG, "Invalid configuration provided");
        return -1;
    }

    /* Copy configuration */
    memcpy(&g_osd_module_state.config, config, sizeof(osd_module_config_t));

    /* Show what configuration was copied */
    IMP_LOG_INFO(TAG, "OSD module init - copied config: enabled=%s, stream0_enabled=%s, stream1_enabled=%s",
                g_osd_module_state.config.enabled ? "true" : "false",
                g_osd_module_state.config.streams[0].enabled ? "true" : "false",
                g_osd_module_state.config.streams[1].enabled ? "true" : "false");

    IMP_LOG_INFO(TAG, "Stream 0 time config: enabled=%s, format='%s', font='%s', size=%d",
                g_osd_module_state.config.streams[0].time.enabled ? "true" : "false",
                g_osd_module_state.config.streams[0].time.format,
                g_osd_module_state.config.streams[0].time.font,
                g_osd_module_state.config.streams[0].time.size);

    IMP_LOG_INFO(TAG, "Stream 1 time config: enabled=%s, format='%s', font='%s', size=%d",
                g_osd_module_state.config.streams[1].time.enabled ? "true" : "false",
                g_osd_module_state.config.streams[1].time.format,
                g_osd_module_state.config.streams[1].time.font,
                g_osd_module_state.config.streams[1].time.size);

    /* The actual OSD initialization happens in main.c using the existing system */
    /* This module just provides the interface and configuration management */

    g_osd_module_state.initialized = true;
    IMP_LOG_INFO(TAG, "OSD module initialized successfully");
    return 0;
}

int osd_module_start(void)
{
    IMP_LOG_INFO(TAG, "Starting OSD module");

    if (!g_osd_module_state.initialized) {
        IMP_LOG_ERR(TAG, "OSD module not initialized");
        return -1;
    }

    if (g_osd_module_state.running) {
        IMP_LOG_WARN(TAG, "OSD module already running");
        return 0;
    }

    if (!g_osd_module_state.config.enabled) {
        IMP_LOG_INFO(TAG, "OSD module disabled in configuration");
        return 0;
    }

    /* The OSD system is already started by main.c */
    /* This module just tracks the state */

    /* Start the independent OSD timer thread */
    IMP_LOG_INFO(TAG, "Starting OSD timer thread");
    if (osd_start_timer_thread() != 0) {
        IMP_LOG_ERR(TAG, "Failed to start OSD timer thread");
        return -1;
    }

    g_osd_module_state.running = true;
    IMP_LOG_INFO(TAG, "OSD module started successfully with independent timer thread");
    return 0;
}

int osd_module_stop(void)
{
    IMP_LOG_INFO(TAG, "Stopping OSD module");

    if (!g_osd_module_state.running) {
        IMP_LOG_WARN(TAG, "OSD module not running");
        return 0;
    }

    /* Stop the independent OSD timer thread */
    osd_stop_timer_thread();

    /* The OSD system cleanup is handled by main.c */
    /* This module just tracks the state */

    g_osd_module_state.running = false;
    IMP_LOG_INFO(TAG, "OSD module stopped successfully");
    return 0;
}

int osd_module_cleanup(void)
{
    IMP_LOG_INFO(TAG, "Cleaning up OSD module");

    if (g_osd_module_state.running) {
        osd_module_stop();
    }

    if (!g_osd_module_state.initialized) {
        return 0;
    }

    /* Cleanup using existing OSD functions */
    osd_cleanup_all();

    /* Reset state */
    memset(&g_osd_module_state, 0, sizeof(g_osd_module_state));

    IMP_LOG_INFO(TAG, "OSD module cleaned up successfully");
    return 0;
}

/* RTSP server integration */
int osd_module_set_rtsp_server(rtsp_server_t* server)
{
    g_osd_module_state.rtsp_server = server;
    IMP_LOG_INFO(TAG, "RTSP server reference set for OSD module");
    return 0;
}

/* Independent OSD timer thread - updates timestamps every second */
static void* osd_timer_thread(void* arg)
{
    IMP_LOG_INFO(TAG, "OSD timer thread started");

    while (g_osd_module_state.timer_thread_running) {
        pthread_mutex_lock(&g_osd_module_state.timer_mutex);

        /* Update OSD for all active channels */
        for (int channel = 0; channel < MAX_STREAMS; channel++) {
            if (g_osd_contexts[channel] && g_osd_contexts[channel]->initialized) {
                /* Check if this channel has active RTSP clients */
                int client_count = 0;
                if (g_osd_module_state.rtsp_server) {
                    client_count = rtsp_server_get_client_count(g_osd_module_state.rtsp_server, channel);
                }

                // IMP_LOG_DBG(TAG, "Timer thread - channel %d: initialized=%s, client_count=%d",
                //            channel,
                //            g_osd_contexts[channel]->initialized ? "true" : "false",
                //            client_count);

                /* Update OSD if channel has clients or if we want to update regardless */
                if (client_count > 0) {
                    // IMP_LOG_DBG(TAG, "Updating timestamp for channel %d (has %d clients)", channel, client_count);
                    osd_update_timestamp_current(g_osd_contexts[channel]);
                } else {
                    // IMP_LOG_DBG(TAG, "Skipping timestamp update for channel %d (no clients)", channel);
                }
            } else {
                if (channel < MAX_STREAMS) {
                    IMP_LOG_DBG(TAG, "Channel %d not available - ctx=%p, initialized=%s",
                               channel,
                               g_osd_contexts[channel],
                               g_osd_contexts[channel] ? (g_osd_contexts[channel]->initialized ? "true" : "false") : "N/A");
                }
            }
        }

        pthread_mutex_unlock(&g_osd_module_state.timer_mutex);

        /* Sleep for 1 second */
        sleep(1);
    }

    IMP_LOG_INFO(TAG, "OSD timer thread stopped");
    return NULL;
}

/* Start the independent OSD timer thread */
static int osd_start_timer_thread(void)
{
    if (g_osd_module_state.timer_thread_running) {
        IMP_LOG_WARN(TAG, "OSD timer thread already running");
        return 0;
    }

    g_osd_module_state.timer_thread_running = true;

    if (pthread_create(&g_osd_module_state.timer_thread, NULL, osd_timer_thread, NULL) != 0) {
        IMP_LOG_ERR(TAG, "Failed to create OSD timer thread");
        g_osd_module_state.timer_thread_running = false;
        return -1;
    }

    IMP_LOG_INFO(TAG, "OSD timer thread started successfully");
    return 0;
}

/* Stop the independent OSD timer thread */
static int osd_stop_timer_thread(void)
{
    if (!g_osd_module_state.timer_thread_running) {
        return 0;
    }

    IMP_LOG_INFO(TAG, "Stopping OSD timer thread");
    g_osd_module_state.timer_thread_running = false;

    /* Wait for thread to finish */
    if (pthread_join(g_osd_module_state.timer_thread, NULL) != 0) {
        IMP_LOG_ERR(TAG, "Failed to join OSD timer thread");
        return -1;
    }

    IMP_LOG_INFO(TAG, "OSD timer thread stopped successfully");
    return 0;
}

/* RTSP frame callback - handles OSD updates */
int osd_module_rtsp_frame_callback(rtsp_server_t* server, int channel,
    const uint8_t* frame_data, uint32_t frame_size, const struct timeval* timestamp)
{
    if (!g_osd_module_state.running || !g_osd_module_state.rtsp_server) {
        return 0;
    }

    /* Only update OSD for the specific channel that triggered this callback */
    if (channel < 0 || channel >= MAX_STREAMS || !g_osd_contexts[channel] || !g_osd_contexts[channel]->initialized) {
        IMP_LOG_DBG(TAG, "Skipping OSD update for channel %d (not initialized or invalid)", channel);
        return 0;
    }

    /* OSD updates are now handled by independent timer thread */
    /* This callback is no longer needed for OSD updates */
    /* OSD callback logging disabled for performance */

    return 0;
}

/* Module registration - following metrics module pattern */
module_info_t osd_module_info = {
    .name = OSD_MODULE_NAME,
    .version = OSD_MODULE_VERSION,
    .description = "On-Screen Display overlay system",
    .state = MODULE_STATE_UNREGISTERED,
    .module_data = &g_osd_module_state,

    /* Lifecycle callbacks */
    .init = osd_module_init,
    .start = osd_module_start,
    .stop = osd_module_stop,
    .cleanup = osd_module_cleanup,

    /* Configuration */
    .config_size = sizeof(osd_module_config_t),
    .config_parse = osd_module_config_parse,

    /* RTSP integration */
    .rtsp_setup = osd_module_set_rtsp_server,
    .rtsp_frame_callback = osd_module_rtsp_frame_callback,
    .rtsp_cleanup = NULL,

    /* Statistics - not implemented */
    .get_stats = NULL
};

/* Auto-register module at startup */
MODULE_REGISTER(osd_module_info);

/* Module registration function for manual registration */
int register_osd_module(void)
{
    return module_register(&osd_module_info);
}

int osd_module_get_config_size(void)
{
    return sizeof(osd_module_config_t);
}

int osd_module_config_parse(json_object* json, void* config)
{

    if (!json || !config) {
        return -1;
    }

    osd_module_config_t* osd_config = (osd_module_config_t*)config;

    /* Initialize default values for motion zones */
    for (int i = 0; i < 2; i++) {
        osd_config->streams[i].motion_zones.enabled = false;
        osd_config->streams[i].motion_zones.show_include_zones = true;
        osd_config->streams[i].motion_zones.show_exclude_zones = true;
        strncpy(osd_config->streams[i].motion_zones.include_color, "#00FF0080", sizeof(osd_config->streams[i].motion_zones.include_color) - 1);
        strncpy(osd_config->streams[i].motion_zones.exclude_color, "#FF000080", sizeof(osd_config->streams[i].motion_zones.exclude_color) - 1);
        osd_config->streams[i].motion_zones.line_width = 2;
    }

    /* JSON root is the osd config directly (no wrapper) */
    json_object* osd_obj = json;

    /* Parse enabled flag */
    json_object* enabled_obj;
    if (json_object_object_get_ex(osd_obj, "enabled", &enabled_obj)) {
        osd_config->enabled = json_object_get_boolean(enabled_obj);
    }

    /* Parse update interval */
    json_object* interval_obj;
    if (json_object_object_get_ex(osd_obj, "update_interval_ms", &interval_obj)) {
        osd_config->update_interval_ms = json_object_get_int(interval_obj);
    }

    /* Parse streams array */
    json_object* streams_obj;
    if (json_object_object_get_ex(osd_obj, "streams", &streams_obj)) {
        if (json_object_is_type(streams_obj, json_type_array)) {
            int stream_count = json_object_array_length(streams_obj);
            for (int i = 0; i < stream_count && i < 2; i++) {
                json_object* stream_obj = json_object_array_get_idx(streams_obj, i);
                if (stream_obj) {
                    /* Parse stream enabled */
                    json_object* stream_enabled_obj;
                    if (json_object_object_get_ex(stream_obj, "enabled", &stream_enabled_obj)) {
                        osd_config->streams[i].enabled = json_object_get_boolean(stream_enabled_obj);
                    }

                    /* Parse time settings */
                    json_object* time_obj;
                    if (json_object_object_get_ex(stream_obj, "time", &time_obj)) {
                        json_object* time_enabled_obj;
                        if (json_object_object_get_ex(time_obj, "enabled", &time_enabled_obj)) {
                            osd_config->streams[i].time.enabled = json_object_get_boolean(time_enabled_obj);
                        }

                        json_object* format_obj;
                        if (json_object_object_get_ex(time_obj, "format", &format_obj)) {
                            const char* format_str = json_object_get_string(format_obj);
                            if (format_str) {
                                strncpy(osd_config->streams[i].time.format, format_str, sizeof(osd_config->streams[i].time.format) - 1);
                            }
                        }

                        json_object* font_obj;
                        if (json_object_object_get_ex(time_obj, "font", &font_obj)) {
                            const char* font_str = json_object_get_string(font_obj);
                            if (font_str) {
                                strncpy(osd_config->streams[i].time.font, font_str, sizeof(osd_config->streams[i].time.font) - 1);
                            }
                        }

                        json_object* size_obj;
                        if (json_object_object_get_ex(time_obj, "size", &size_obj)) {
                            osd_config->streams[i].time.size = json_object_get_int(size_obj);
                        }

                        json_object* color_obj;
                        if (json_object_object_get_ex(time_obj, "color", &color_obj)) {
                            const char* color_str = json_object_get_string(color_obj);
                            if (color_str) {
                                strncpy(osd_config->streams[i].time.color, color_str, sizeof(osd_config->streams[i].time.color) - 1);
                            }
                        }

                        json_object* background_obj;
                        if (json_object_object_get_ex(time_obj, "background", &background_obj)) {
                            const char* bg_str = json_object_get_string(background_obj);
                            if (bg_str) {
                                strncpy(osd_config->streams[i].time.background, bg_str, sizeof(osd_config->streams[i].time.background) - 1);
                            }
                        }

                        json_object* position_obj;
                        if (json_object_object_get_ex(time_obj, "position", &position_obj)) {
                            const char* pos_str = json_object_get_string(position_obj);
                            if (pos_str) {
                                strncpy(osd_config->streams[i].time.position, pos_str, sizeof(osd_config->streams[i].time.position) - 1);
                            }
                        }
                    }

                    /* Parse logo settings */
                    json_object* logo_obj;
                    if (json_object_object_get_ex(stream_obj, "logo", &logo_obj)) {
                        json_object* logo_enabled_obj;
                        if (json_object_object_get_ex(logo_obj, "enabled", &logo_enabled_obj)) {
                            osd_config->streams[i].logo.enabled = json_object_get_boolean(logo_enabled_obj);
                        }

                        json_object* image_obj;
                        if (json_object_object_get_ex(logo_obj, "image", &image_obj)) {
                            const char* image_str = json_object_get_string(image_obj);
                            if (image_str) {
                                strncpy(osd_config->streams[i].logo.image, image_str, sizeof(osd_config->streams[i].logo.image) - 1);
                            }
                        }

                        json_object* logo_size_obj;
                        if (json_object_object_get_ex(logo_obj, "size", &logo_size_obj)) {
                            const char* size_str = json_object_get_string(logo_size_obj);
                            if (size_str) {
                                strncpy(osd_config->streams[i].logo.size, size_str, sizeof(osd_config->streams[i].logo.size) - 1);
                            }
                        }

                        json_object* logo_position_obj;
                        if (json_object_object_get_ex(logo_obj, "position", &logo_position_obj)) {
                            const char* pos_str = json_object_get_string(logo_position_obj);
                            if (pos_str) {
                                strncpy(osd_config->streams[i].logo.position, pos_str, sizeof(osd_config->streams[i].logo.position) - 1);
                            }
                        }

                        json_object* opacity_obj;
                        if (json_object_object_get_ex(logo_obj, "opacity", &opacity_obj)) {
                            osd_config->streams[i].logo.opacity = json_object_get_double(opacity_obj);
                        }
                    }

                    /* Parse info settings */
                    json_object* info_obj;
                    if (json_object_object_get_ex(stream_obj, "info", &info_obj)) {
                        json_object* info_enabled_obj;
                        if (json_object_object_get_ex(info_obj, "enabled", &info_enabled_obj)) {
                            osd_config->streams[i].info.enabled = json_object_get_boolean(info_enabled_obj);
                        }

                        json_object* info_position_obj;
                        if (json_object_object_get_ex(info_obj, "position", &info_position_obj)) {
                            const char* pos_str = json_object_get_string(info_position_obj);
                            if (pos_str) {
                                strncpy(osd_config->streams[i].info.position, pos_str, sizeof(osd_config->streams[i].info.position) - 1);
                            }
                        }
                    }

                    /* Parse motion zones settings */
                    json_object* motion_zones_obj;
                    if (json_object_object_get_ex(stream_obj, "motion_zones", &motion_zones_obj)) {
                        json_object* motion_zones_enabled_obj;
                        if (json_object_object_get_ex(motion_zones_obj, "enabled", &motion_zones_enabled_obj)) {
                            osd_config->streams[i].motion_zones.enabled = json_object_get_boolean(motion_zones_enabled_obj);
                        }

                        json_object* show_include_obj;
                        if (json_object_object_get_ex(motion_zones_obj, "show_include_zones", &show_include_obj)) {
                            osd_config->streams[i].motion_zones.show_include_zones = json_object_get_boolean(show_include_obj);
                        }

                        json_object* show_exclude_obj;
                        if (json_object_object_get_ex(motion_zones_obj, "show_exclude_zones", &show_exclude_obj)) {
                            osd_config->streams[i].motion_zones.show_exclude_zones = json_object_get_boolean(show_exclude_obj);
                        }

                        json_object* include_color_obj;
                        if (json_object_object_get_ex(motion_zones_obj, "include_color", &include_color_obj)) {
                            const char* color_str = json_object_get_string(include_color_obj);
                            if (color_str) {
                                strncpy(osd_config->streams[i].motion_zones.include_color, color_str, sizeof(osd_config->streams[i].motion_zones.include_color) - 1);
                            }
                        }

                        json_object* exclude_color_obj;
                        if (json_object_object_get_ex(motion_zones_obj, "exclude_color", &exclude_color_obj)) {
                            const char* color_str = json_object_get_string(exclude_color_obj);
                            if (color_str) {
                                strncpy(osd_config->streams[i].motion_zones.exclude_color, color_str, sizeof(osd_config->streams[i].motion_zones.exclude_color) - 1);
                            }
                        }

                        json_object* line_width_obj;
                        if (json_object_object_get_ex(motion_zones_obj, "line_width", &line_width_obj)) {
                            osd_config->streams[i].motion_zones.line_width = json_object_get_int(line_width_obj);
                        }
                    }
                }
            }
        }
    }

    return 0;
}

/* Motion zone visualization functions */

/* Setup motion zones region */
int osd_setup_motion_zones(osd_context_t* ctx)
{
    if (!ctx) {
        return -1;
    }

    /* Set up group region attributes for motion zones */
    IMPOSDGrpRgnAttr grAttr;
    memset(&grAttr, 0, sizeof(IMPOSDGrpRgnAttr));
    grAttr.show = 1;
    grAttr.layer = 0;  /* Layer 0 for motion zones (top layer) */
    grAttr.scalex = 1;
    grAttr.scaley = 1;
    grAttr.gAlphaEn = 1;  /* Enable global alpha */
    grAttr.fgAlhpa = 0xFF;  /* Foreground alpha */
    grAttr.bgAlhpa = 0x00;  /* Background alpha (transparent) */

    /* Setup all motion zone regions */
    for (int i = 0; i < OSD_MAX_MOTION_ZONES; i++) {
        int region_index = OSD_MOTION_ZONE_REGION_START + i;
        int ret = IMP_OSD_SetGrpRgnAttr(ctx->region_handles[region_index], ctx->group_id, &grAttr);
        if (ret < 0) {
            IMP_LOG_ERR(TAG, "Failed to set motion zone %d group region attributes for Group %d: %d", i, ctx->group_id, ret);
            return -1;
        }
        IMP_LOG_DBG(TAG, "Motion zone region %d setup completed for Group %d", i, ctx->group_id);
    }

    IMP_LOG_INFO(TAG, "All %d motion zone regions setup completed for Group %d", OSD_MAX_MOTION_ZONES, ctx->group_id);
    return 0;
}

/* Enable/disable motion zone visualization */
int osd_enable_motion_zones(int group_id, bool enabled)
{
    if (group_id < 0 || group_id >= MAX_STREAMS || !g_osd_contexts[group_id]) {
        IMP_LOG_ERR(TAG, "Invalid group_id %d or context not initialized", group_id);
        return -1;
    }

    osd_context_t* ctx = g_osd_contexts[group_id];
    pthread_mutex_lock(&ctx->mutex);

    ctx->motion_zones.enabled = enabled;

    if (!enabled) {
        /* Hide all motion zone regions */
        for (int i = 0; i < OSD_MAX_MOTION_ZONES; i++) {
            int region_index = OSD_MOTION_ZONE_REGION_START + i;
            IMP_OSD_ShowRgn(ctx->region_handles[region_index], group_id, 0);
        }
    }

    pthread_mutex_unlock(&ctx->mutex);

    IMP_LOG_INFO(TAG, "Motion zones visualization %s for group %d",
                enabled ? "enabled" : "disabled", group_id);
    return 0;
}

/* Set motion zone colors */
int osd_set_motion_zone_colors(int group_id, uint32_t include_color, uint32_t exclude_color)
{
    if (group_id < 0 || group_id >= MAX_STREAMS || !g_osd_contexts[group_id]) {
        IMP_LOG_ERR(TAG, "Invalid group_id %d or context not initialized", group_id);
        return -1;
    }

    osd_context_t* ctx = g_osd_contexts[group_id];
    pthread_mutex_lock(&ctx->mutex);

    ctx->motion_zones.include_color = include_color;
    ctx->motion_zones.exclude_color = exclude_color;

    pthread_mutex_unlock(&ctx->mutex);

    IMP_LOG_INFO(TAG, "Motion zone colors updated for group %d: include=0x%08X, exclude=0x%08X",
                group_id, include_color, exclude_color);
    return 0;
}

/* Update motion zones visualization */
int osd_update_motion_zones(int group_id)
{
    IMP_LOG_INFO(TAG, "osd_update_motion_zones called for group_id=%d", group_id);

    if (group_id < 0 || group_id >= MAX_STREAMS || !g_osd_contexts[group_id]) {
        IMP_LOG_ERR(TAG, "Invalid group_id %d or context not found", group_id);
        return -1;
    }

    osd_context_t* ctx = g_osd_contexts[group_id];

    if (!ctx->motion_zones.enabled) {
        IMP_LOG_INFO(TAG, "Motion zones disabled for group %d, skipping update", group_id);
        return 0;
    }

    IMP_LOG_INFO(TAG, "Motion zones enabled for group %d, proceeding with update", group_id);

    /* Get motion zones from motion module */
    extern int motion_module_get_zones(int* zone_count, void** zones_data);
    int zone_count = 0;
    void* zones_data = NULL;

    if (motion_module_get_zones(&zone_count, &zones_data) != 0 || zone_count == 0) {
        /* No zones to display, hide all motion zone regions */
        IMP_LOG_WARN(TAG, "No motion zones available for group %d (get_zones failed or zone_count=0)", group_id);
        for (int i = 0; i < OSD_MAX_MOTION_ZONES; i++) {
            int region_index = OSD_MOTION_ZONE_REGION_START + i;
            IMP_OSD_ShowRgn(ctx->region_handles[region_index], group_id, 0);
        }
        return 0;
    }

    IMP_LOG_INFO(TAG, "Got %d motion zones for group %d, proceeding with visualization", zone_count, group_id);

    pthread_mutex_lock(&ctx->mutex);

    /* Cast zones data to the correct structure from motion module */
    struct motion_zone {
        int id;
        char type[16];
        int x, y;
        int width, height;
        char name[64];
    }* zones = (struct motion_zone*)zones_data;

    IMP_LOG_ERR(TAG, "DEBUG: zone_count=%d, zones_data=%p", zone_count, zones_data);

    /* Limit zone count to available regions */
    int zones_to_display = (zone_count > OSD_MAX_MOTION_ZONES) ? OSD_MAX_MOTION_ZONES : zone_count;

    /* Hide all motion zone regions first */
    for (int i = 0; i < OSD_MAX_MOTION_ZONES; i++) {
        int region_index = OSD_MOTION_ZONE_REGION_START + i;
        IMP_OSD_ShowRgn(ctx->region_handles[region_index], group_id, 0);
    }

    /* Detect original zone coordinate system by finding max coordinates */
    int max_zone_x = 0, max_zone_y = 0;
    for (int i = 0; i < zones_to_display; i++) {
        struct motion_zone* z = &zones[i];
        if (z->width > 0 && z->height > 0) {  /* Skip full-frame zones */
            int right = z->x + z->width;
            int bottom = z->y + z->height;
            if (right > max_zone_x) max_zone_x = right;
            if (bottom > max_zone_y) max_zone_y = bottom;
        }
    }

    /* Determine original coordinate system */
    int original_width = 1920, original_height = 1080;  /* Default to full resolution */
    if (max_zone_x > 0 && max_zone_y > 0) {
        if (max_zone_x <= 640 && max_zone_y <= 360) {
            original_width = 640; original_height = 360;
        } else if (max_zone_x <= 1280 && max_zone_y <= 720) {
            original_width = 1280; original_height = 720;
        } else {
            original_width = 1920; original_height = 1080;
        }
    }

    /* Calculate scaling factors for this stream */
    float scale_x = (float)ctx->stream_width / original_width;
    float scale_y = (float)ctx->stream_height / original_height;

    IMP_LOG_INFO(TAG, "Zone scaling for group %d: original=%dx%d, stream=%dx%d, scale=(%.3f,%.3f)",
                group_id, original_width, original_height, ctx->stream_width, ctx->stream_height, scale_x, scale_y);

    /* Process each zone */
    for (int zone_idx = 0; zone_idx < zones_to_display; zone_idx++) {
        struct motion_zone* zone = &zones[zone_idx];
        int region_index = OSD_MOTION_ZONE_REGION_START + zone_idx;

        IMP_LOG_ERR(TAG, "DEBUG: Zone[%d] - id=%d, type='%s', name='%s', original coords=(%d,%d,%d,%d)",
                   zone_idx, zone->id, zone->type, zone->name, zone->x, zone->y, zone->width, zone->height);

        /* Scale zone coordinates to match this stream's resolution */
        int zone_x, zone_y, zone_w, zone_h;

        /* Handle full-frame zones (width/height = 0) */
        if (zone->width == 0 || zone->height == 0) {
            zone_x = 0;
            zone_y = 0;
            zone_w = ctx->stream_width;
            zone_h = ctx->stream_height;
        } else {
            /* Scale coordinates proportionally */
            zone_x = (int)(zone->x * scale_x);
            zone_y = (int)(zone->y * scale_y);
            zone_w = (int)(zone->width * scale_x);
            zone_h = (int)(zone->height * scale_y);
        }

        IMP_LOG_INFO(TAG, "Zone[%d] '%s' scaled: (%d,%d,%d,%d) -> (%d,%d,%d,%d)",
                    zone_idx, zone->name, zone->x, zone->y, zone->width, zone->height,
                    zone_x, zone_y, zone_w, zone_h);

        /* Skip zones with invalid dimensions */
        if (zone_w <= 0 || zone_h <= 0) {
            IMP_LOG_WARN(TAG, "Skipping zone[%d] '%s' with invalid dimensions: %dx%d",
                        zone_idx, zone->name, zone_w, zone_h);
            continue;
        }

        /* Skip zones that are completely outside the frame */
        if (zone_x >= ctx->stream_width || zone_y >= ctx->stream_height) {
            IMP_LOG_WARN(TAG, "Skipping zone[%d] '%s' outside frame bounds: pos=(%d,%d), frame=%dx%d",
                        zone_idx, zone->name, zone_x, zone_y, ctx->stream_width, ctx->stream_height);
            continue;
        }

        /* Map configured color to closest predefined OSD color */
        uint32_t configured_color = (strcmp(zone->type, "include") == 0) ?
                                   ctx->motion_zones.include_color : ctx->motion_zones.exclude_color;

        /* Extract RGB components from #RRGGBBAA format */
        uint32_t r = (configured_color >> 24) & 0xFF;
        uint32_t g = (configured_color >> 16) & 0xFF;
        uint32_t b = (configured_color >> 8) & 0xFF;

        /* Map to closest predefined OSD color for T31 SDK */
        uint32_t osd_color;
        if (r > 200 && g < 100 && b < 100) {
            osd_color = OSD_RED;    /* 0xffff0000 */
        } else if (r < 100 && g > 200 && b < 100) {
            osd_color = OSD_GREEN;  /* 0xff00ff00 */
        } else if (r < 100 && g < 100 && b > 200) {
            osd_color = OSD_BLUE;   /* 0xff0000ff */
        } else if (r > 200 && g > 200 && b > 200) {
            osd_color = OSD_WHITE;  /* 0xffffffff */
        } else {
            osd_color = OSD_BLACK;  /* 0xff000000 */
        }

        IMP_LOG_INFO(TAG, "Drawing motion zone '%s' (%s): rect=(%d,%d) to (%d,%d), color=0x%08X->0x%08X",
                    zone->name, zone->type, zone_x, zone_y, zone_x + zone_w - 1, zone_y + zone_h - 1,
                    configured_color, osd_color);

        /* Use rectangle following T31 1.1.6 sample pattern */
        IMPOSDRgnAttr rAttr;
        memset(&rAttr, 0, sizeof(IMPOSDRgnAttr));
        rAttr.type = OSD_REG_RECT;
        rAttr.rect.p0.x = zone_x;
        rAttr.rect.p0.y = zone_y;
        rAttr.rect.p1.x = zone_x + zone_w - 1;
        rAttr.rect.p1.y = zone_y + zone_h - 1;
        rAttr.fmt = PIX_FMT_MONOWHITE;  /* Following T31 sample */
        rAttr.data.lineRectData.color = osd_color;
        rAttr.data.lineRectData.linewidth = ctx->motion_zones.line_width;

        int ret = IMP_OSD_SetRgnAttr(ctx->region_handles[region_index], &rAttr);
        if (ret == 0) {
            /* Set group region attributes AFTER setting region attributes */
            IMPOSDGrpRgnAttr grAttr;
            memset(&grAttr, 0, sizeof(IMPOSDGrpRgnAttr));
            grAttr.show = 1;
            grAttr.layer = 0;  /* Top layer */
            grAttr.scalex = 1;
            grAttr.scaley = 1;
            grAttr.gAlphaEn = 1;
            grAttr.fgAlhpa = 0xFF;
            grAttr.bgAlhpa = 0x00;

            int grp_ret = IMP_OSD_SetGrpRgnAttr(ctx->region_handles[region_index], group_id, &grAttr);
            if (grp_ret == 0) {
                int show_ret = IMP_OSD_ShowRgn(ctx->region_handles[region_index], group_id, 1);
                if (show_ret == 0) {
                    IMP_LOG_INFO(TAG, "Motion zone[%d] '%s' visualized successfully for group %d", zone_idx, zone->name, group_id);
                } else {
                    IMP_LOG_ERR(TAG, "Failed to SHOW motion zone[%d] rectangle for group %d: show_ret=%d", zone_idx, group_id, show_ret);
                }
            } else {
                IMP_LOG_ERR(TAG, "Failed to set GROUP region attributes for zone[%d] group %d: grp_ret=%d", zone_idx, group_id, grp_ret);
            }
        } else {
            IMP_LOG_ERR(TAG, "Failed to SET motion zone[%d] rectangle attributes for group %d: set_ret=%d", zone_idx, group_id, ret);
        }
    }

    pthread_mutex_unlock(&ctx->mutex);

    IMP_LOG_INFO(TAG, "Updated %d motion zones for group %d", zones_to_display, group_id);
    return 0;
}
