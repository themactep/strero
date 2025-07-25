/*
 * onvif_module.h - ONVIF Module Interface
 * Modular ONVIF implementation with WS-Discovery and SOAP services
 * Copyright (C) 2025 Paul Philippov, Thingino Project
 */

#ifndef __ONVIF_MODULE_H__
#define __ONVIF_MODULE_H__

#include <stdbool.h>
#include <stdint.h>

#include <json-c/json.h>

#include "../../module_system.h"
#include "../../auth_utils.h"

/* ONVIF module configuration - self-contained */
typedef struct onvif_module_config {
    bool enabled;                    /* Enable/disable ONVIF services */
    auth_config_t auth;              /* Authentication configuration */
    char device_name[64];            /* ONVIF device name */
    char device_location[64];        /* Device location description */
    char manufacturer[64];           /* Manufacturer name */
    char model[64];                  /* Device model */
    char serial_number[64];          /* Device serial number */
    char firmware_version[32];       /* Firmware version string */
    char hardware_id[32];            /* Hardware identifier */
    bool discovery_enabled;          /* Enable WS-Discovery service */
    int discovery_port;              /* WS-Discovery port (default: 3702) */
    bool device_service_enabled;     /* Enable ONVIF Device service */
    bool media_service_enabled;      /* Enable ONVIF Media service */
    bool event_service_enabled;      /* Enable ONVIF Event service */
} onvif_module_config_t;

/* ONVIF module interface functions */
int onvif_module_init(void* config);
int onvif_module_start(void);
int onvif_module_stop(void);
int onvif_module_cleanup(void);
int onvif_module_config_parse(json_object* json, void* config);

/* ONVIF HTTP endpoint integration */
void onvif_module_handle_request(int client_socket, const char* request, void* streamer_config);

/* ONVIF service functions */
int onvif_start_discovery_service(void);
int onvif_stop_discovery_service(void);

/* ONVIF module utility functions */
const onvif_module_config_t* onvif_module_get_config(void);
bool onvif_module_is_enabled(void);
bool onvif_module_is_running(void);

/* Module info structure */
extern module_info_t onvif_module_info;

/* Route registration function */
int onvif_register_routes(void);

#endif /* __ONVIF_MODULE_H__ */
