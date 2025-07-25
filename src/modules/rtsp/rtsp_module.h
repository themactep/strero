#ifndef __RTSP_MODULE_H__
#define __RTSP_MODULE_H__

#include <stdbool.h>
#include <stdint.h>

#include "../../module_system.h"
#include "../../auth_utils.h"
#include "rtsp_server.h"

#define RTSP_MODULE_VERSION "1.0.0"
#define RTSP_MODULE_NAME "rtsp"

/* RTSP module configuration structure */
typedef struct {
    bool enabled;                     /* Enable/disable RTSP server */
    int port;                         /* RTSP server port */
    int session_reclaim;              /* Session reclaim time in seconds */
    auth_config_t auth;               /* Authentication configuration */
    char server_name[128];            /* Server name */
    int max_clients;                  /* Maximum concurrent clients */
    int session_timeout;              /* Session timeout in seconds */

    /* RTSPS (TLS) configuration */
    bool tls_enabled;                 /* Enable RTSPS (RTSP over TLS) */
    int tls_port;                     /* RTSPS port (default 322) */
    char cert_file[256];              /* Path to certificate file */
    char key_file[256];               /* Path to private key file */
    bool tls_verify_client;           /* Require client certificate verification */
} rtsp_module_config_t;

/* Module interface */
extern module_info_t rtsp_module_info;

/* Module lifecycle functions */
int rtsp_module_init(void* config);
int rtsp_module_start(void);
int rtsp_module_stop(void);
int rtsp_module_cleanup(void);
int rtsp_module_get_config_size(void);
int rtsp_module_config_parse(json_object* json, void* config);
int rtsp_module_config_validate(void* config);

/* Module registration function */
int register_rtsp_module(void);

/* RTSP server access for other modules */
rtsp_server_t* rtsp_module_get_server(void);

#endif /* __RTSP_MODULE_H__ */
