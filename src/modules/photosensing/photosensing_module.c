/*
 * photosensing_module.c - Photosensing module implementation
 * Self-contained photosensing module for automatic day/night mode switching
 * Copyright (C) 2025 Paul Philippov, Thingino Project
 */

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <imp/imp_isp.h>
#include <json-c/json.h>
#include <sys/stat.h>

#include "photosensing_module.h"
#include "../../common.h"

#define TAG "PHOTOSENSING_MODULE"

/* Global photosensing module state */
static photosensing_module_state_t g_photosensing_state = {0};

/* Forward declarations */
static void* photosensing_control_thread(void* arg);
static int setup_photosensing_defaults(void);

/* Module registration - manual registration due to symbol stripping */
module_info_t photosensing_module_info = {
    .name = PHOTOSENSING_MODULE_NAME,
    .version = PHOTOSENSING_MODULE_VERSION,
    .description = "Automatic day/night mode switching based on light conditions",
    .state = MODULE_STATE_UNREGISTERED,
    .module_data = &g_photosensing_state,

    /* Lifecycle callbacks */
    .init = photosensing_module_init,
    .start = photosensing_module_start,
    .stop = photosensing_module_stop,
    .cleanup = photosensing_module_cleanup,

    /* Configuration callbacks */
    .config_parse = photosensing_module_config_parse,
    .config_validate = photosensing_module_config_validate,
    .config_free = photosensing_module_config_free,
    .config_size = sizeof(photosensing_module_config_t),

    /* RTSP integration - not needed for photosensing */
    .rtsp_setup = NULL,
    .rtsp_frame_callback = NULL,
    .rtsp_cleanup = NULL,

    /* Statistics */
    .get_stats = photosensing_module_get_stats
};

/* Auto-register module at startup */
MODULE_REGISTER(photosensing_module_info);

int photosensing_module_init(void* config)
{
    if (!config) {
        IMP_LOG_ERR(TAG, "Invalid photosensing configuration");
        return -1;
    }

    if (g_photosensing_state.initialized) {
        IMP_LOG_WARN(TAG, "Photosensing module already initialized");
        return 0;
    }

    IMP_LOG_INFO(TAG, "Initializing photosensing module");

    /* Copy configuration */
    memcpy(&g_photosensing_state.config, config, sizeof(photosensing_module_config_t));

    /* Check if photosensing is enabled */
    if (!g_photosensing_state.config.enabled) {
        IMP_LOG_INFO(TAG, "Photosensing disabled in configuration");
        g_photosensing_state.initialized = true;
        return 0;
    }

    /* Initialize mutex */
    if (pthread_mutex_init(&g_photosensing_state.mutex, NULL) != 0) {
        IMP_LOG_ERR(TAG, "Failed to initialize photosensing mutex");
        return -1;
    }

    /* Setup default values */
    if (setup_photosensing_defaults() != 0) {
        IMP_LOG_ERR(TAG, "Failed to setup photosensing defaults");
        pthread_mutex_destroy(&g_photosensing_state.mutex);
        return -1;
    }

    /* Initialize statistics */
    memset(&g_photosensing_state.stats, 0, sizeof(photosensing_stats_t));
    g_photosensing_state.stats.current_mode = PHOTOSENSING_MODE_DAY;
    g_photosensing_state.start_time = time(NULL);

    g_photosensing_state.initialized = true;
    IMP_LOG_INFO(TAG, "Photosensing module initialized successfully");

    return 0;
}

int photosensing_module_start(void)
{
    if (!g_photosensing_state.initialized) {
        IMP_LOG_ERR(TAG, "Photosensing module not initialized");
        return -1;
    }

    if (!g_photosensing_state.config.enabled) {
        IMP_LOG_INFO(TAG, "Photosensing disabled, not starting");
        return 0;
    }

    if (g_photosensing_state.running) {
        IMP_LOG_WARN(TAG, "Photosensing module already running");
        return 0;
    }

    IMP_LOG_INFO(TAG, "Starting photosensing module");

    /* Set initial ISP mode to day */
    int ret = IMP_ISP_Tuning_SetISPRunningMode(IMPISP_RUNNING_MODE_DAY);
    if (ret < 0) {
        IMP_LOG_ERR(TAG, "Failed to set initial ISP running mode to day");
        return -1;
    }

    /* Set initial IR cut state */
    if (g_photosensing_state.config.ircut_enabled) {
        ret = photosensing_set_ircut(g_photosensing_state.config.ircut_day_state);
        if (ret < 0) {
            IMP_LOG_WARN(TAG, "Failed to set initial IR cut state, continuing");
        }
        g_photosensing_state.ircut_status = (g_photosensing_state.config.ircut_day_state == 0);
    }

    /* Start control thread */
    g_photosensing_state.thread_should_exit = false;
    ret = pthread_create(&g_photosensing_state.control_thread, NULL,
                        photosensing_control_thread, NULL);
    if (ret != 0) {
        IMP_LOG_ERR(TAG, "Failed to create photosensing control thread: %s", strerror(ret));
        return -1;
    }

    g_photosensing_state.running = true;
    IMP_LOG_INFO(TAG, "Photosensing module started successfully");

    return 0;
}

int photosensing_module_stop(void)
{
    if (!g_photosensing_state.running) {
        IMP_LOG_INFO(TAG, "Photosensing module not running");
        return 0;
    }

    IMP_LOG_INFO(TAG, "Stopping photosensing module");

    /* Signal thread to exit */
    g_photosensing_state.thread_should_exit = true;

    /* Wait for thread to finish */
    if (pthread_join(g_photosensing_state.control_thread, NULL) != 0) {
        IMP_LOG_WARN(TAG, "Failed to join photosensing control thread");
    }

    g_photosensing_state.running = false;
    IMP_LOG_INFO(TAG, "Photosensing module stopped successfully");

    return 0;
}

int photosensing_module_cleanup(void)
{
    if (!g_photosensing_state.initialized) {
        return 0;
    }

    IMP_LOG_INFO(TAG, "Cleaning up photosensing module");

    /* Stop if running */
    if (g_photosensing_state.running) {
        photosensing_module_stop();
    }

    /* Cleanup mutex */
    pthread_mutex_destroy(&g_photosensing_state.mutex);

    /* Reset state */
    memset(&g_photosensing_state, 0, sizeof(photosensing_module_state_t));

    IMP_LOG_INFO(TAG, "Photosensing module cleanup complete");

    return 0;
}

static int setup_photosensing_defaults(void)
{
    /* Initialize state variables */
    g_photosensing_state.current_mode = PHOTOSENSING_MODE_DAY;
    g_photosensing_state.ircut_status = true;
    g_photosensing_state.day_count = 0;
    g_photosensing_state.night_count = 0;
    g_photosensing_state.gb_gain_record = g_photosensing_state.config.gb_gain_record_init;
    g_photosensing_state.gr_gain_record = g_photosensing_state.config.gr_gain_record_init;
    g_photosensing_state.thread_should_exit = false;

    return 0;
}

int photosensing_set_ircut(int enable)
{
    int fd, fd_gpio1, fd_gpio2;
    char on[4], off[4];
    char gpio1_str[8], gpio2_str[8];
    char gpio1_path[64], gpio2_path[64];

    /* Get GPIO pins from configuration */
    int gpio1 = g_photosensing_state.config.ircut_gpio1;
    int gpio2 = g_photosensing_state.config.ircut_gpio2;

    /* Validate GPIO pins are configured - prevent hardware damage */
    if (gpio1 < 0 || gpio2 < 0) {
        IMP_LOG_ERR(TAG, "IR cut GPIO pins not configured (gpio1=%d, gpio2=%d) - cannot control IR cut to prevent hardware damage", gpio1, gpio2);
        return -1;
    }

    snprintf(gpio1_str, sizeof(gpio1_str), "%d", gpio1);
    snprintf(gpio2_str, sizeof(gpio2_str), "%d", gpio2);

    /* Check for custom IR cut script */
    if (!access("/tmp/setir", 0)) {
        if (enable) {
            system("/tmp/setir 0 1");
        } else {
            system("/tmp/setir 1 0");
        }
        return 0;
    }

    /* Use GPIO control */
    fd = open("/sys/class/gpio/export", O_WRONLY);
    if (fd < 0) {
        IMP_LOG_DBG(TAG, "open /sys/class/gpio/export error!");
        return -1;
    }

    write(fd, gpio1_str, strlen(gpio1_str));
    write(fd, gpio2_str, strlen(gpio2_str));
    close(fd);

    snprintf(gpio1_path, sizeof(gpio1_path), "/sys/class/gpio/gpio%d/direction", gpio1);
    snprintf(gpio2_path, sizeof(gpio2_path), "/sys/class/gpio/gpio%d/direction", gpio2);

    fd_gpio1 = open(gpio1_path, O_RDWR);
    if (fd_gpio1 < 0) {
        IMP_LOG_DBG(TAG, "open %s error!", gpio1_path);
        return -1;
    }

    fd_gpio2 = open(gpio2_path, O_RDWR);
    if (fd_gpio2 < 0) {
        IMP_LOG_DBG(TAG, "open %s error!", gpio2_path);
        close(fd_gpio1);
        return -1;
    }

    write(fd_gpio1, "out", 3);
    write(fd_gpio2, "out", 3);
    close(fd_gpio1);
    close(fd_gpio2);

    snprintf(gpio1_path, sizeof(gpio1_path), "/sys/class/gpio/gpio%d/active_low", gpio1);
    snprintf(gpio2_path, sizeof(gpio2_path), "/sys/class/gpio/gpio%d/active_low", gpio2);

    fd_gpio1 = open(gpio1_path, O_RDWR);
    if (fd_gpio1 < 0) {
        IMP_LOG_DBG(TAG, "open %s error!", gpio1_path);
        return -1;
    }

    fd_gpio2 = open(gpio2_path, O_RDWR);
    if (fd_gpio2 < 0) {
        IMP_LOG_DBG(TAG, "open %s error!", gpio2_path);
        close(fd_gpio1);
        return -1;
    }

    write(fd_gpio1, "0", 1);
    write(fd_gpio2, "0", 1);
    close(fd_gpio1);
    close(fd_gpio2);

    snprintf(gpio1_path, sizeof(gpio1_path), "/sys/class/gpio/gpio%d/value", gpio1);
    snprintf(gpio2_path, sizeof(gpio2_path), "/sys/class/gpio/gpio%d/value", gpio2);

    fd_gpio1 = open(gpio1_path, O_RDWR);
    if (fd_gpio1 < 0) {
        IMP_LOG_DBG(TAG, "open %s error!", gpio1_path);
        return -1;
    }

    fd_gpio2 = open(gpio2_path, O_RDWR);
    if (fd_gpio2 < 0) {
        IMP_LOG_DBG(TAG, "open %s error!", gpio2_path);
        close(fd_gpio1);
        return -1;
    }

    sprintf(on, "%d", enable);
    sprintf(off, "%d", !enable);

    write(fd_gpio1, "0", 1);
    usleep(10 * 1000);

    write(fd_gpio1, on, strlen(on));
    write(fd_gpio2, off, strlen(off));

    if (!enable) {
        usleep(10 * 1000);
        write(fd_gpio1, off, strlen(off));
    }

    close(fd_gpio1);
    close(fd_gpio2);

    return 0;
}

int photosensing_module_config_parse(json_object* json, void* config)
{
    if (!json || !config) {
        IMP_LOG_ERR(TAG, "Invalid parameters for config parsing");
        return -1;
    }

    photosensing_module_config_t* ps_config = (photosensing_module_config_t*)config;

    /* Set default values */
    ps_config->enabled = true;
    ps_config->night_iso_threshold = 1900000.0f;
    ps_config->night_count_threshold = 5;
    ps_config->day_iso_threshold = 479832.0f;
    ps_config->day_gb_gain_offset = 15.0f;
    ps_config->day_gb_gain_threshold = 145.0f;
    ps_config->day_iso_secondary_threshold = 361880.0f;
    ps_config->day_count_threshold = 3;
    ps_config->polling_interval_ms = 1000;
    ps_config->ircut_enabled = true;
    ps_config->ircut_day_state = 1;
    ps_config->ircut_night_state = 0;
    ps_config->gb_gain_record_init = 200.0f;
    ps_config->gr_gain_record_init = 200.0f;
    ps_config->debug_logging = false;

    ps_config->ircut_gpio1 = -1;  /* Must be configured - no defaults to prevent hardware damage */
    ps_config->ircut_gpio2 = -1;

    /* Parse JSON fields */
    json_object* obj;

    if (json_object_object_get_ex(json, "enabled", &obj)) {
        ps_config->enabled = json_object_get_boolean(obj);
    }

    if (json_object_object_get_ex(json, "night_iso_threshold", &obj)) {
        ps_config->night_iso_threshold = (float)json_object_get_double(obj);
    }

    if (json_object_object_get_ex(json, "night_count_threshold", &obj)) {
        ps_config->night_count_threshold = json_object_get_int(obj);
    }

    if (json_object_object_get_ex(json, "day_iso_threshold", &obj)) {
        ps_config->day_iso_threshold = (float)json_object_get_double(obj);
    }

    if (json_object_object_get_ex(json, "day_gb_gain_offset", &obj)) {
        ps_config->day_gb_gain_offset = (float)json_object_get_double(obj);
    }

    if (json_object_object_get_ex(json, "day_gb_gain_threshold", &obj)) {
        ps_config->day_gb_gain_threshold = (float)json_object_get_double(obj);
    }

    if (json_object_object_get_ex(json, "day_iso_secondary_threshold", &obj)) {
        ps_config->day_iso_secondary_threshold = (float)json_object_get_double(obj);
    }

    if (json_object_object_get_ex(json, "day_count_threshold", &obj)) {
        ps_config->day_count_threshold = json_object_get_int(obj);
    }

    if (json_object_object_get_ex(json, "polling_interval_ms", &obj)) {
        ps_config->polling_interval_ms = json_object_get_int(obj);
    }

    if (json_object_object_get_ex(json, "ircut_enabled", &obj)) {
        ps_config->ircut_enabled = json_object_get_boolean(obj);
    }

    if (json_object_object_get_ex(json, "ircut_day_state", &obj)) {
        ps_config->ircut_day_state = json_object_get_int(obj);
    }

    if (json_object_object_get_ex(json, "ircut_night_state", &obj)) {
        ps_config->ircut_night_state = json_object_get_int(obj);
    }

    if (json_object_object_get_ex(json, "ircut_gpio1", &obj)) {
        ps_config->ircut_gpio1 = json_object_get_int(obj);
    }

    if (json_object_object_get_ex(json, "ircut_gpio2", &obj)) {
        ps_config->ircut_gpio2 = json_object_get_int(obj);
    }

    if (json_object_object_get_ex(json, "gb_gain_record_init", &obj)) {
        ps_config->gb_gain_record_init = (float)json_object_get_double(obj);
    }

    if (json_object_object_get_ex(json, "gr_gain_record_init", &obj)) {
        ps_config->gr_gain_record_init = (float)json_object_get_double(obj);
    }

    if (json_object_object_get_ex(json, "debug_logging", &obj)) {
        ps_config->debug_logging = json_object_get_boolean(obj);
    }

    IMP_LOG_INFO(TAG, "Photosensing config loaded:");
    IMP_LOG_INFO(TAG, "  enabled: %s", ps_config->enabled ? "true" : "false");
    IMP_LOG_INFO(TAG, "  night_iso_threshold: %.0f", ps_config->night_iso_threshold);
    IMP_LOG_INFO(TAG, "  night_count_threshold: %d", ps_config->night_count_threshold);
    IMP_LOG_INFO(TAG, "  day_iso_threshold: %.0f", ps_config->day_iso_threshold);
    IMP_LOG_INFO(TAG, "  day_gb_gain_offset: %.1f", ps_config->day_gb_gain_offset);
    IMP_LOG_INFO(TAG, "  day_gb_gain_threshold: %.1f", ps_config->day_gb_gain_threshold);
    IMP_LOG_INFO(TAG, "  day_iso_secondary_threshold: %.0f", ps_config->day_iso_secondary_threshold);
    IMP_LOG_INFO(TAG, "  day_count_threshold: %d", ps_config->day_count_threshold);
    IMP_LOG_INFO(TAG, "  polling_interval_ms: %d", ps_config->polling_interval_ms);
    IMP_LOG_INFO(TAG, "  ircut_enabled: %s", ps_config->ircut_enabled ? "true" : "false");
    IMP_LOG_INFO(TAG, "  ircut_day_state: %d", ps_config->ircut_day_state);
    IMP_LOG_INFO(TAG, "  ircut_night_state: %d", ps_config->ircut_night_state);
    IMP_LOG_INFO(TAG, "  ircut_gpio1: %d", ps_config->ircut_gpio1);
    IMP_LOG_INFO(TAG, "  ircut_gpio2: %d", ps_config->ircut_gpio2);
    IMP_LOG_INFO(TAG, "  gb_gain_record_init: %.1f", ps_config->gb_gain_record_init);
    IMP_LOG_INFO(TAG, "  gr_gain_record_init: %.1f", ps_config->gr_gain_record_init);
    IMP_LOG_INFO(TAG, "  debug_logging: %s", ps_config->debug_logging ? "true" : "false");

    return 0;
}

int photosensing_module_config_validate(void* config)
{
    if (!config) {
        IMP_LOG_ERR(TAG, "Invalid photosensing configuration");
        return -1;
    }

    photosensing_module_config_t* ps_config = (photosensing_module_config_t*)config;

    /* Validate thresholds */
    if (ps_config->night_iso_threshold <= 0 || ps_config->day_iso_threshold <= 0) {
        IMP_LOG_ERR(TAG, "Invalid ISO thresholds");
        return -1;
    }

    if (ps_config->night_count_threshold <= 0 || ps_config->day_count_threshold <= 0) {
        IMP_LOG_ERR(TAG, "Invalid count thresholds");
        return -1;
    }

    if (ps_config->polling_interval_ms <= 0) {
        IMP_LOG_ERR(TAG, "Invalid polling interval");
        return -1;
    }

    /* Validate GPIO pins if IR cut is enabled - critical for hardware safety */
    if (ps_config->ircut_enabled) {
        if (ps_config->ircut_gpio1 < 0 || ps_config->ircut_gpio2 < 0) {
            IMP_LOG_ERR(TAG, "IR cut enabled but GPIO pins not configured (gpio1=%d, gpio2=%d) - this could damage hardware",
                       ps_config->ircut_gpio1, ps_config->ircut_gpio2);
            return -1;
        }
    }

    return 0;
}

void photosensing_module_config_free(void* config)
{
    /* No dynamic memory to free in photosensing config */
    (void)config;
}

int photosensing_module_get_stats(void* stats_buffer, size_t buffer_size)
{
    if (!stats_buffer || buffer_size < sizeof(photosensing_stats_t)) {
        IMP_LOG_ERR(TAG, "Invalid stats buffer");
        return -1;
    }

    pthread_mutex_lock(&g_photosensing_state.mutex);

    /* Update uptime */
    g_photosensing_state.stats.uptime_seconds = time(NULL) - g_photosensing_state.start_time;

    /* Copy stats */
    memcpy(stats_buffer, &g_photosensing_state.stats, sizeof(photosensing_stats_t));

    pthread_mutex_unlock(&g_photosensing_state.mutex);

    return 0;
}

static void* photosensing_control_thread(void* arg)
{
    (void)arg;

    IMP_LOG_INFO(TAG, "Photosensing control thread started");

    int i;
    float gb_gain, gr_gain;
    float iso_buf;
    float gb_gain_buf, gr_gain_buf;
    IMPISPRunningMode pmode;
    IMPISPEVAttr ExpAttr;
    IMPISPWB wb;

    while (!g_photosensing_state.thread_should_exit) {
        /* Get exposure AE information */
        int ret = IMP_ISP_Tuning_GetEVAttr(&ExpAttr);
        if (ret != 0) {
            IMP_LOG_ERR(TAG, "IMP_ISP_Tuning_GetEVAttr failed");
            break;
        }

        iso_buf = ExpAttr.ev;

        /* Get white balance statistics */
        ret = IMP_ISP_Tuning_GetWB_Statis(&wb);
        if (ret != 0) {
            IMP_LOG_ERR(TAG, "IMP_ISP_Tuning_GetWB_Statis failed");
            break;
        }
        gr_gain = wb.rgain;
        gb_gain = wb.bgain;

        pthread_mutex_lock(&g_photosensing_state.mutex);

        /* Update statistics */
        g_photosensing_state.stats.last_iso_value = iso_buf;
        g_photosensing_state.stats.last_gb_gain = gb_gain;
        g_photosensing_state.stats.last_gr_gain = gr_gain;
        g_photosensing_state.stats.day_count = g_photosensing_state.day_count;
        g_photosensing_state.stats.night_count = g_photosensing_state.night_count;

        /* Night mode detection */
        if (iso_buf > g_photosensing_state.config.night_iso_threshold) {
            g_photosensing_state.night_count++;
            if (g_photosensing_state.night_count > g_photosensing_state.config.night_count_threshold) {
                IMP_ISP_Tuning_GetISPRunningMode(&pmode);
                if (pmode != IMPISP_RUNNING_MODE_NIGHT) {
                    IMP_LOG_INFO(TAG, "### Switching to night mode ###");
                    IMP_ISP_Tuning_SetISPRunningMode(IMPISP_RUNNING_MODE_NIGHT);

                    if (g_photosensing_state.config.ircut_enabled) {
                        photosensing_set_ircut(g_photosensing_state.config.ircut_night_state);
                        g_photosensing_state.ircut_status = (g_photosensing_state.config.ircut_night_state == 0);
                    }

                    /* Update statistics */
                    if (g_photosensing_state.current_mode == PHOTOSENSING_MODE_DAY) {
                        g_photosensing_state.stats.day_to_night_switches++;
                        g_photosensing_state.stats.mode_switches++;
                    }
                    g_photosensing_state.current_mode = PHOTOSENSING_MODE_NIGHT;
                    g_photosensing_state.stats.current_mode = PHOTOSENSING_MODE_NIGHT;
                }

                /* After switching to night vision, take the minimum value of 20 gb_gain samples
                 * as the reference value gb_gain_record for switching back to day mode */
                for (i = 0; i < 20; i++) {
                    IMP_ISP_Tuning_GetWB_GOL_Statis(&wb);
                    gr_gain = wb.rgain;
                    gb_gain = wb.bgain;
                    if (i == 0) {
                        gb_gain_buf = gb_gain;
                        gr_gain_buf = gr_gain;
                    }
                    gb_gain_buf = ((gb_gain_buf > gb_gain) ? gb_gain : gb_gain_buf);
                    gr_gain_buf = ((gr_gain_buf > gr_gain) ? gr_gain : gr_gain_buf);
                    usleep(300000);
                    g_photosensing_state.gb_gain_record = gb_gain_buf;
                    g_photosensing_state.gr_gain_record = gr_gain_buf;
                }
            }
        } else {
            g_photosensing_state.night_count = 0;
        }

        /* Day mode detection - meet these three conditions to enter day mode switching judgment */
        if (((int)iso_buf < g_photosensing_state.config.day_iso_threshold) &&
            (g_photosensing_state.ircut_status == true) &&
            (gb_gain > g_photosensing_state.gb_gain_record + g_photosensing_state.config.day_gb_gain_offset)) {

            if ((iso_buf < g_photosensing_state.config.day_iso_secondary_threshold) ||
                (gb_gain > g_photosensing_state.config.day_gb_gain_threshold)) {
                g_photosensing_state.day_count++;
            } else {
                g_photosensing_state.day_count = 0;
            }

            if (g_photosensing_state.day_count > g_photosensing_state.config.day_count_threshold) {
                IMP_LOG_INFO(TAG, "### Switching to day mode ###");
                IMP_ISP_Tuning_GetISPRunningMode(&pmode);
                if (pmode != IMPISP_RUNNING_MODE_DAY) {
                    IMP_ISP_Tuning_SetISPRunningMode(IMPISP_RUNNING_MODE_DAY);

                    if (g_photosensing_state.config.ircut_enabled) {
                        photosensing_set_ircut(g_photosensing_state.config.ircut_day_state);
                        g_photosensing_state.ircut_status = (g_photosensing_state.config.ircut_day_state == 0);
                    }

                    /* Update statistics */
                    if (g_photosensing_state.current_mode == PHOTOSENSING_MODE_NIGHT) {
                        g_photosensing_state.stats.night_to_day_switches++;
                        g_photosensing_state.stats.mode_switches++;
                    }
                    g_photosensing_state.current_mode = PHOTOSENSING_MODE_DAY;
                    g_photosensing_state.stats.current_mode = PHOTOSENSING_MODE_DAY;
                }
            }
        } else {
            g_photosensing_state.day_count = 0;
        }

        /* Debug logging if enabled */
        if (g_photosensing_state.config.debug_logging) {
            IMP_LOG_DBG(TAG,
                "day_count: %d, night_count: %d, iso_buf: %.0f, gb_gain: %.1f, gr_gain: %.1f, "
                "gb_gain_record: %.1f, gr_gain_record: %.1f, mode: %s",
                g_photosensing_state.day_count, g_photosensing_state.night_count,
                iso_buf, gb_gain, gr_gain,
                g_photosensing_state.gb_gain_record, g_photosensing_state.gr_gain_record,
                photosensing_mode_to_string(g_photosensing_state.current_mode));
        }

        pthread_mutex_unlock(&g_photosensing_state.mutex);

        /* Sleep for configured interval */
        usleep(g_photosensing_state.config.polling_interval_ms * 1000);
    }

    IMP_LOG_INFO(TAG, "Photosensing control thread exiting");
    return NULL;
}

/* Utility functions */
const char* photosensing_mode_to_string(photosensing_mode_t mode)
{
    switch (mode) {
        case PHOTOSENSING_MODE_DAY: return "day";
        case PHOTOSENSING_MODE_NIGHT: return "night";
        case PHOTOSENSING_MODE_UNKNOWN: return "unknown";
        default: return "invalid";
    }
}
