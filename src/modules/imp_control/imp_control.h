/*
 * imp_control.h - IMP Control Module Implementation
 * Copyright (C) 2025 Paul Philippov, Thingino Project
 */

#ifndef __IMP_CONTROL_H__
#define __IMP_CONTROL_H__

#include <stdbool.h>
#include <stdint.h>
#include "../../module_system.h"

#define IMP_CONTROL_VERSION "1.0.0"
#define IMP_CONTROL_NAME "imp_control"

/* IMP image quality parameter limits for T31X */
#define IMP_CONTROL_MIN_VALUE 0
#define IMP_CONTROL_MAX_VALUE 255
#define IMP_CONTROL_DEFAULT_VALUE 128

/* Day/Night mode enumeration */
typedef enum {
    IMP_CONTROL_MODE_DAY = 0,             /* Day mode */
    IMP_CONTROL_MODE_NIGHT = 1,           /* Night mode */
    IMP_CONTROL_MODE_AUTO = 2             /* Auto day/night switching */
} imp_control_day_night_mode_t;

/* Anti-flicker mode enumeration */
typedef enum {
    IMP_CONTROL_ANTIFLICKER_DISABLE = 0,  /* Disable anti-flicker */
    IMP_CONTROL_ANTIFLICKER_50HZ = 1,     /* 50Hz anti-flicker */
    IMP_CONTROL_ANTIFLICKER_60HZ = 2      /* 60Hz anti-flicker */
} imp_control_antiflicker_mode_t;

/* White balance mode enumeration */
typedef enum {
    IMP_CONTROL_WB_AUTO = 0,              /* Auto white balance */
    IMP_CONTROL_WB_MANUAL = 1             /* Manual white balance */
} imp_control_wb_mode_t;

/* IMP control module configuration structure - simplified to only contain IMP parameters */
typedef struct {
    /* Basic image quality parameters */
    unsigned char brightness;             /* Image brightness (0-255, default 128) */
    unsigned char contrast;               /* Image contrast (0-255, default 128) */
    unsigned char saturation;             /* Image saturation (0-255, default 128) */
    unsigned char sharpness;              /* Image sharpness (0-255, default 128) */

    /* Priority 1 extensions */
    unsigned char hue;                    /* Image hue (0-255, default 128) */
    unsigned char ae_compensation;        /* AE compensation (0-255, default 128) */
    unsigned char noise_reduction_2d;     /* 2D noise reduction (0-255, default 128) */
    unsigned char noise_reduction_3d;     /* 3D noise reduction (0-255, default 128) */
    bool flip_horizontal;                 /* Horizontal flip (default false) */
    bool flip_vertical;                   /* Vertical flip (default false) */
    imp_control_day_night_mode_t day_night_mode; /* Day/night mode (default auto) */

    /* Priority 2 extensions */
    imp_control_antiflicker_mode_t anti_flicker; /* Anti-flicker mode (default disable) */
    unsigned char backlight_compensation; /* Backlight compensation (0-10, default 0) */
    unsigned char highlight_suppression;  /* Highlight suppression (0-10, default 0) */
    imp_control_wb_mode_t white_balance_mode; /* White balance mode (default auto) */
    unsigned short white_balance_r_gain;  /* Manual WB red gain (0-1023, default 256) */
    unsigned short white_balance_b_gain;  /* Manual WB blue gain (0-1023, default 256) */
    unsigned char drc_strength;           /* Dynamic range compression (0-255, default 128) */
    unsigned char defog_strength;         /* Defog strength (0-255, default 0) */
} imp_control_config_t;

/* Current IMP parameters structure */
typedef struct {
    /* Basic image quality parameters */
    unsigned char brightness;
    unsigned char contrast;
    unsigned char saturation;
    unsigned char sharpness;

    /* Priority 1 extensions */
    unsigned char hue;
    unsigned char ae_compensation;
    unsigned char noise_reduction_2d;
    unsigned char noise_reduction_3d;
    bool flip_horizontal;
    bool flip_vertical;
    imp_control_day_night_mode_t day_night_mode;

    /* Priority 2 extensions */
    imp_control_antiflicker_mode_t anti_flicker;
    unsigned char backlight_compensation;
    unsigned char highlight_suppression;
    imp_control_wb_mode_t white_balance_mode;
    unsigned short white_balance_r_gain;
    unsigned short white_balance_b_gain;
    unsigned char drc_strength;
    unsigned char defog_strength;
} imp_control_params_t;

/* Module interface */
extern module_info_t imp_control_info;

/* Module lifecycle functions */
int imp_control_init(void* config);
int imp_control_start(void);
int imp_control_stop(void);
int imp_control_cleanup(void);
int imp_control_get_config_size(void);
int imp_control_set_defaults(void* config);

/* Module registration function */
int register_imp_control(void);

/* IMP parameter control functions */
int imp_control_get_params(imp_control_params_t* params);
int imp_control_set_params(const imp_control_params_t* params);
int imp_control_set_brightness(unsigned char brightness);
int imp_control_set_contrast(unsigned char contrast);
int imp_control_set_saturation(unsigned char saturation);
int imp_control_set_sharpness(unsigned char sharpness);
int imp_control_set_hue(unsigned char hue);
int imp_control_set_ae_compensation(unsigned char ae_comp);
int imp_control_set_noise_reduction_2d(unsigned char strength);
int imp_control_set_noise_reduction_3d(unsigned char strength);
int imp_control_set_flip_horizontal(bool enable);
int imp_control_set_flip_vertical(bool enable);
int imp_control_set_day_night_mode(imp_control_day_night_mode_t mode);
int imp_control_set_anti_flicker(imp_control_antiflicker_mode_t mode);
int imp_control_set_backlight_compensation(unsigned char strength);
int imp_control_set_highlight_suppression(unsigned char strength);
int imp_control_set_white_balance_mode(imp_control_wb_mode_t mode);
int imp_control_set_white_balance_gains(unsigned short r_gain, unsigned short b_gain);
int imp_control_set_drc_strength(unsigned char strength);
int imp_control_set_defog_strength(unsigned char strength);
int imp_control_reset_to_defaults(void);

/* Parameter validation functions */
bool imp_control_validate_param(unsigned char value);
bool imp_control_validate_params(const imp_control_params_t* params);

/* HTTP API handlers */
void imp_control_handle_get_params(int client_socket);
void imp_control_handle_set_params(int client_socket, const char* request_body);
void imp_control_handle_reset_params(int client_socket);
void imp_control_handle_save_params(int client_socket);

/* Utility functions */
const char* imp_control_get_version(void);
const char* imp_control_get_name(void);
const char* imp_control_day_night_mode_to_string(imp_control_day_night_mode_t mode);
imp_control_day_night_mode_t imp_control_string_to_day_night_mode(const char* str);
const char* imp_control_antiflicker_mode_to_string(imp_control_antiflicker_mode_t mode);
imp_control_antiflicker_mode_t imp_control_string_to_antiflicker_mode(const char* str);
const char* imp_control_wb_mode_to_string(imp_control_wb_mode_t mode);
imp_control_wb_mode_t imp_control_string_to_wb_mode(const char* str);

/* Route registration function */
int imp_control_register_routes(void);

#endif /* __IMP_CONTROL_H__ */
