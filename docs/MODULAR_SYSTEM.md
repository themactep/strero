# Thingino Streamer Module Development Guide

This guide provides comprehensive documentation for creating modules that integrate with the Thingino Streamer's modular architecture system.

## Table of Contents
1. [Module System Overview](#module-system-overview)
2. [Module Structure](#module-structure)
3. [Step-by-Step Module Creation](#step-by-step-module-creation)
4. [Module Interface](#module-interface)
5. [Configuration System](#configuration-system)
6. [Build System Integration](#build-system-integration)
7. [Buildroot Package Integration](#buildroot-package-integration)
8. [Best Practices](#best-practices)
9. [Example: OSD Module](#example-osd-module)
10. [Testing and Debugging](#testing-and-debugging)

## Module System Overview

The Thingino Streamer uses a plugin-based modular architecture that allows optional features to be compiled in or out based on configuration. Each module is self-contained with its own:

- Source code directory
- Build configuration (Makefile)
- Configuration files
- Lifecycle management
- RTSP integration (if needed)

### Key Benefits
- **Modularity**: Features can be enabled/disabled at compile time
- **Resource Efficiency**: Only needed modules consume memory/CPU
- **Maintainability**: Each module is isolated and self-contained
- **Extensibility**: New features can be added without modifying core code

## Module Structure

### Directory Layout
```
src/modules/your_module/
├── Makefile.your_module          # Build configuration
├── your_module.c                 # Main module implementation
├── your_module.h                 # Module header/interface
└── (optional additional files)

res/config/
└── your_module.json              # Module configuration file

buildroot/package/thingino-streamer/
├── Config.in                     # Buildroot menu option
└── thingino-streamer.mk          # Buildroot build rules
```

### Core Files Required
1. **Module Implementation** (`your_module.c`)
2. **Module Header** (`your_module.h`)
3. **Module Makefile** (`Makefile.your_module`)
4. **Configuration File** (`your_module.json`)
5. **Buildroot Integration** (Config.in + .mk updates)

## Step-by-Step Module Creation

### Step 1: Create Module Directory Structure

```bash
mkdir -p src/modules/your_module
mkdir -p res/config
```

### Step 2: Create Module Header (`src/modules/your_module/your_module.h`)

```c
#ifndef __YOUR_MODULE_H__
#define __YOUR_MODULE_H__

#include <stdbool.h>
#include <stdint.h>
#include "../../module_system.h"

#define YOUR_MODULE_VERSION "1.0.0"
#define YOUR_MODULE_NAME "your_module"

/* Module configuration structure */
typedef struct {
    bool enabled;                     /* Enable/disable module */
    int update_interval_ms;           /* Update interval in milliseconds */
    /* Add your module-specific config here */
} your_module_config_t;

/* Module interface */
extern module_info_t your_module_info;

/* Module lifecycle functions */
int your_module_init(void* config);
int your_module_start(void);
int your_module_stop(void);
int your_module_cleanup(void);
int your_module_get_config_size(void);
int your_module_set_defaults(void* config);

/* Module registration function */
int register_your_module(void);

/* Module-specific functions (if needed) */
struct rtsp_server;
int your_module_set_rtsp_server(struct rtsp_server* server);

#endif /* __YOUR_MODULE_H__ */
```

### Step 3: Create Module Implementation (`src/modules/your_module/your_module.c`)

```c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>

#include "your_module.h"
#include "../../logger.h"
#include "../../common.h"
#include "../../config.h"
#include "../../rtsp_server.h"

#define TAG "YOUR_MODULE"

/* Module state */
static struct {
    bool initialized;
    bool running;
    your_module_config_t config;
    pthread_t update_thread;
    bool thread_should_exit;
    struct rtsp_server* rtsp_server;  /* Reference to RTSP server */
} g_your_module_state = {0};

/* Forward declarations */
static void* your_module_update_thread(void* arg);

/* Module thread function */
static void* your_module_update_thread(void* arg) {
    IMP_LOG_INFO(TAG, "Module update thread started");

    while (!g_your_module_state.thread_should_exit) {
        /* Your module's main processing loop */

        /* Example: Only process when RTSP clients are connected */
        if (g_your_module_state.rtsp_server) {
            int client_count = rtsp_server_get_client_count(g_your_module_state.rtsp_server);
            if (client_count > 0) {
                /* Do your module work here */
                IMP_LOG_DEBUG(TAG, "Processing for %d clients", client_count);
            }
        }

        /* Sleep for configured interval */
        usleep(g_your_module_state.config.update_interval_ms * 1000);
    }

    IMP_LOG_INFO(TAG, "Module update thread exiting");
    return NULL;
}

/* Module lifecycle callbacks */
int your_module_init(void* config) {
    IMP_LOG_INFO(TAG, "Initializing module");

    if (g_your_module_state.initialized) {
        IMP_LOG_WARN(TAG, "Module already initialized");
        return 0;
    }

    if (!config) {
        IMP_LOG_ERR(TAG, "Invalid configuration provided");
        return -1;
    }

    /* Copy configuration */
    memcpy(&g_your_module_state.config, config, sizeof(your_module_config_t));

    /* Initialize your module resources here */

    g_your_module_state.initialized = true;
    IMP_LOG_INFO(TAG, "Module initialized successfully");
    return 0;
}

int your_module_start(void) {
    IMP_LOG_INFO(TAG, "Starting module");

    if (!g_your_module_state.initialized) {
        IMP_LOG_ERR(TAG, "Module not initialized");
        return -1;
    }

    if (g_your_module_state.running) {
        IMP_LOG_WARN(TAG, "Module already running");
        return 0;
    }

    if (!g_your_module_state.config.enabled) {
        IMP_LOG_INFO(TAG, "Module disabled in configuration");
        return 0;
    }

    /* Start update thread */
    g_your_module_state.thread_should_exit = false;
    if (pthread_create(&g_your_module_state.update_thread, NULL, your_module_update_thread, NULL) != 0) {
        IMP_LOG_ERR(TAG, "Failed to create module update thread");
        return -1;
    }

    g_your_module_state.running = true;
    IMP_LOG_INFO(TAG, "Module started successfully");
    return 0;
}

int your_module_stop(void) {
    IMP_LOG_INFO(TAG, "Stopping module");

    if (!g_your_module_state.running) {
        IMP_LOG_WARN(TAG, "Module not running");
        return 0;
    }

    /* Signal thread to exit */
    g_your_module_state.thread_should_exit = true;

    /* Wait for thread to finish */
    if (pthread_join(g_your_module_state.update_thread, NULL) != 0) {
        IMP_LOG_ERR(TAG, "Failed to join module update thread");
        return -1;
    }

    g_your_module_state.running = false;
    IMP_LOG_INFO(TAG, "Module stopped successfully");
    return 0;
}

int your_module_cleanup(void) {
    IMP_LOG_INFO(TAG, "Cleaning up module");

    if (g_your_module_state.running) {
        your_module_stop();
    }

    if (!g_your_module_state.initialized) {
        return 0;
    }

    /* Cleanup your module resources here */

    /* Reset state */
    memset(&g_your_module_state, 0, sizeof(g_your_module_state));

    IMP_LOG_INFO(TAG, "Module cleaned up successfully");
    return 0;
}

int your_module_get_config_size(void) {
    return sizeof(your_module_config_t);
}

int your_module_set_defaults(void* config) {
    if (!config) {
        return -1;
    }

    your_module_config_t* your_config = (your_module_config_t*)config;
    memset(your_config, 0, sizeof(your_module_config_t));

    /* Set default values */
    your_config->enabled = true;
    your_config->update_interval_ms = 1000;  /* Update every second */

    return 0;
}

/* RTSP server integration */
int your_module_set_rtsp_server(struct rtsp_server* server) {
    g_your_module_state.rtsp_server = server;
    IMP_LOG_INFO(TAG, "RTSP server reference set for module");
    return 0;
}

/* Module registration - following the established pattern */
module_info_t your_module_info = {
    .name = YOUR_MODULE_NAME,
    .version = YOUR_MODULE_VERSION,
    .description = "Your module description",
    .state = MODULE_STATE_UNREGISTERED,
    .module_data = &g_your_module_state,

    /* Lifecycle callbacks */
    .init = your_module_init,
    .start = your_module_start,
    .stop = your_module_stop,
    .cleanup = your_module_cleanup,

    /* Configuration */
    .config_size = sizeof(your_module_config_t),

    /* RTSP integration - not needed for all modules */
    .rtsp_setup = NULL,
    .rtsp_frame_callback = NULL,
    .rtsp_cleanup = NULL,

    /* Statistics - not implemented */
    .get_stats = NULL
};

/* Auto-register module at startup */
MODULE_REGISTER(your_module_info);

/* Module registration function for manual registration */
int register_your_module(void) {
    return module_register(&your_module_info);
}
```

### Step 4: Create Module Makefile (`src/modules/your_module/Makefile.your_module`)

```makefile
# Your Module Makefile

# Module object files
YOUR_MODULE_OBJECTS = obj/modules/your_module/your_module.o

# Add module objects to the main build
OBJS += $(YOUR_MODULE_OBJECTS)

# Module compilation rules
obj/modules/your_module/your_module.o: src/modules/your_module/your_module.c src/modules/your_module/your_module.h
	@mkdir -p obj/modules/your_module
	$(CC) $(CFLAGS) \
		-I$(LIBIMP_INC_DIR) \
		-I$(LIBIMP_INC_DIR)/imp \
		-I$(LIBIMP_INC_DIR)/sysutils \
		-isystem $(THIRDPARTY_INC_DIR) \
		-c $< -o $@

# Clean rule for module
clean-your-module:
	rm -f $(YOUR_MODULE_OBJECTS)

.PHONY: clean-your-module
```

### Step 5: Create Configuration File (`res/config/your_module.json`)

```json
{
  "enabled": true,
  "update_interval": 1,
  "custom_parameter": "value"
}
```

### Step 6: Update Main Makefile

Add to the main `Makefile`:

```makefile
# Include your module if enabled
ifeq ($(ENABLE_YOUR_MODULE),1)
    CFLAGS += -DENABLE_YOUR_MODULE=1
    include src/modules/your_module/Makefile.your_module
endif
```

**Note:** The module objects are automatically added to `OBJS` by the module's Makefile.

### Step 7: Register Module in main.c

Add to `src/main.c`:

```c
#ifdef ENABLE_YOUR_MODULE
#include "modules/your_module/your_module.h"
#endif

// In the module registration section:
#ifdef ENABLE_YOUR_MODULE
    if (register_your_module() != 0) {
        IMP_LOG_WARN(TAG, "Failed to register your module");
    } else {
        IMP_LOG_INFO(TAG, "Your module registered successfully");
    }
#endif
```

**Alternative:** If using `MODULE_REGISTER()` macro, the module will auto-register and manual registration is not needed.

## Module Interface

### Core Module Structure (`module_info_t`)

```c
typedef struct module_info {
    /* Module identification */
    char name[MAX_MODULE_NAME_LEN];
    const char* version;
    const char* description;

    /* Module state */
    module_state_t state;
    void* module_data;

    /* Lifecycle callbacks */
    int (*init)(void* config);
    int (*start)(void);
    int (*stop)(void);
    int (*cleanup)(void);

    /* Configuration callbacks */
    int (*config_parse)(json_object* json, void* config);
    int (*config_validate)(void* config);
    int (*get_config_size)(void);

    /* RTSP integration */
    void (*rtsp_frame_callback)(rtsp_server_t* server, void* user_data);
    int (*rtsp_setup)(rtsp_server_t* server);
} module_info_t;
```

### Module States

```c
typedef enum {
    MODULE_STATE_UNREGISTERED = 0,
    MODULE_STATE_REGISTERED,
    MODULE_STATE_INITIALIZED,
    MODULE_STATE_RUNNING,
    MODULE_STATE_ERROR
} module_state_t;
```

### Lifecycle Callbacks

1. **`init(void* config)`**: Initialize module resources
2. **`start(void)`**: Start module operation (threads, etc.)
3. **`stop(void)`**: Stop module operation gracefully
4. **`cleanup(void)`**: Free all module resources

### Configuration Callbacks

1. **`config_parse(json_object* json, void* config)`**: Parse JSON configuration
2. **`config_validate(void* config)`**: Validate configuration parameters
3. **`get_config_size(void)`**: Return size of configuration structure

### RTSP Integration

1. **`rtsp_frame_callback(rtsp_server_t* server, void* user_data)`**: Called for each frame
2. **`rtsp_setup(rtsp_server_t* server)`**: Called when RTSP server starts

## Configuration System

### JSON Configuration Format

Modules can define their configuration in JSON format. The configuration system automatically:

1. Loads configuration from `./your_module.json` or `/etc/streamer.d/your_module.json` if the binary directory config is not found
2. Parses JSON using the module's `config_parse` callback
3. Validates configuration using `config_validate` callback
4. Passes configuration to module's `init` function

### Configuration Best Practices

1. **Provide defaults**: Always initialize config structure with sensible defaults
2. **Validate parameters**: Check ranges, required fields, etc.
3. **Handle missing config**: Module should work with default configuration
4. **Document parameters**: Include comments in JSON config files

## Build System Integration

### Main Makefile Integration

The main Makefile uses conditional compilation based on `ENABLE_*` variables:

```makefile
# Set default values
ENABLE_YOUR_MODULE ?= 0

# Include module if enabled
ifeq ($(ENABLE_YOUR_MODULE),1)
    CFLAGS += -DENABLE_YOUR_MODULE
    include src/modules/your_module/Makefile.your_module
    EXTRA_OBJECTS += $(YOUR_MODULE_OBJECTS)
endif
```

### Module-Specific Makefiles

Each module has its own Makefile that:

1. Defines source and object files
2. Creates necessary directories
3. Provides build rules with proper includes
4. Adds module-specific CFLAGS
5. Provides clean rules

## Buildroot Package Integration

### Update Config.in

Add module option to `buildroot/package/thingino-streamer/Config.in`:

```
config BR2_PACKAGE_THINGINO_STREAMER_YOUR_MODULE
	bool "Your module support"
	depends on BR2_PACKAGE_THINGINO_STREAMER
	default y
	help
	  Enable your module functionality.

	  Features: List key features
	  Dependencies: List any dependencies
	  Additional memory usage: Estimate memory impact
```

### Update thingino-streamer.mk

Add to `buildroot/package/thingino-streamer/thingino-streamer.mk`:

```makefile
# Add CFLAGS
ifeq ($(BR2_PACKAGE_THINGINO_STREAMER_YOUR_MODULE),y)
THINGINO_STREAMER_CFLAGS += -DENABLE_YOUR_MODULE=1
endif

# Add to build commands
$(if $(BR2_PACKAGE_THINGINO_STREAMER_YOUR_MODULE),ENABLE_YOUR_MODULE=1,) \
```

## Best Practices

### Code Organization

1. **Single Responsibility**: Each module should have one clear purpose
2. **Minimal Dependencies**: Avoid unnecessary dependencies on other modules
3. **Error Handling**: Always check return values and handle errors gracefully
4. **Resource Management**: Clean up all allocated resources in cleanup callback
5. **Thread Safety**: Use proper synchronization for shared data

### Performance Considerations

1. **Memory Efficiency**: Minimize memory usage, especially on embedded devices
2. **CPU Usage**: Avoid busy loops, use appropriate sleep intervals
3. **Client Awareness**: Only process when clients are connected (if applicable)
4. **Configurable Intervals**: Make timing parameters configurable

### Logging and Debugging

1. **Consistent Logging**: Use IMP_LOG_* macros with module-specific TAG from logger.h, not imp/imp_log.h
2. **Log Levels**: Use appropriate log levels (IMP_LOG_INFO, IMP_LOG_WARN, IMP_LOG_ERR, IMP_LOG_DEBUG)
3. **Startup/Shutdown**: Log module lifecycle events
4. **Error Conditions**: Always log error conditions with context

### Configuration Guidelines

1. **Sensible Defaults**: Module should work without configuration
2. **Validation**: Validate all configuration parameters
3. **Documentation**: Document all configuration options
4. **Backward Compatibility**: Handle configuration format changes gracefully

## Example: OSD Module

The OSD (On-Screen Display) module is a complete real-world example of the modular system:

### Key Features
- **Complete Integration**: All OSD functionality moved from core to module
- **Client Awareness**: Only updates when RTSP clients connected
- **Precise Timing**: Updates exactly once per second
- **Resource Efficient**: Minimal CPU/memory usage
- **Configurable**: JSON-based configuration
- **Backward Compatible**: Existing OSD functions still work

### Real Implementation Structure
```
src/modules/osd/
├── osd_module.h          # Module interface and configuration
├── osd_module.c          # Complete OSD implementation (~2000+ lines)
└── Makefile.osd         # Build configuration

res/config/
└── osd.json             # Module configuration
```

### Key Implementation Details

```c
/* Module state structure */
static struct {
    bool initialized;
    bool running;
    osd_module_config_t config;
    pthread_t update_thread;
    bool thread_should_exit;
    struct rtsp_server* rtsp_server;
} g_osd_module_state = {0};

/* Thread function with client awareness */
static void* osd_update_thread(void* arg) {
    while (!g_osd_module_state.thread_should_exit) {
        /* Only update when clients are connected */
        if (g_osd_module_state.rtsp_server) {
            int client_count = rtsp_server_get_client_count(g_osd_module_state.rtsp_server);
            if (client_count > 0) {
                /* Update OSD overlays */
                osd_update_timestamps();
            }
        }
        usleep(g_osd_module_state.config.update_interval_ms * 1000);
    }
    return NULL;
}
```

### Conversion Process
The OSD module was created by:
1. **Moving existing code** directly to module directory (no copying)
2. **Adding module wrapper** functions for lifecycle management
3. **Updating build system** to remove old OSD compilation
4. **Fixing integration points** with conditional compilation
5. **Testing thoroughly** to ensure functionality preserved

## Testing and Debugging

### Module Testing Checklist

1. **Compilation**: Module compiles with and without ENABLE flag
2. **Registration**: Module registers successfully at startup
3. **Lifecycle**: Init → Start → Stop → Cleanup works correctly
4. **Configuration**: JSON configuration is parsed and validated
5. **Functionality**: Core module functionality works as expected
6. **Resource Cleanup**: No memory leaks or resource leaks
7. **Error Handling**: Graceful handling of error conditions
8. **Integration**: Works correctly with RTSP server and other modules

### Debugging Tips

1. **Enable Debug Logging**: Use IMP_LOG_DEBUG for detailed tracing
2. **Check Module State**: Verify module progresses through states correctly
3. **Monitor Resources**: Check memory usage and thread behavior
4. **Test Edge Cases**: Test with no clients, invalid config, etc.
5. **Use Static Analysis**: Check for potential issues with static analysis tools
6. **Build Incrementally**: Test compilation after each major change
7. **Check Dependencies**: Ensure all required headers are included

### Common Issues

1. **Include Path Problems**: Use proper IMP include paths with -I$(LIBIMP_INC_DIR)
2. **Duplicate Functions**: Remove old implementations when converting existing code
3. **Missing Struct Members**: Ensure module state has all required fields
4. **Thread Cleanup**: Always join threads in stop callback
5. **Configuration Parsing**: Handle missing or invalid JSON gracefully
6. **RTSP Integration**: Ensure proper client count checking
7. **Conditional Compilation**: Wrap module usage with #ifdef ENABLE_MODULE
8. **Build System**: Include module Makefile and add objects correctly

## Conclusion

The Thingino Streamer modular system provides a robust framework for adding new functionality while maintaining system efficiency and maintainability. By following this guide and the established patterns, you can create modules that integrate seamlessly with the existing architecture.

### Key Takeaways:
- **Follow established patterns**: Use the OSD module as a reference implementation
- **Move code, don't copy**: When converting existing functionality, move the real code directly
- **Proper build integration**: Include correct IMP paths and conditional compilation
- **Test incrementally**: Build and test after each major change
- **Handle errors gracefully**: Always check return values and clean up resources
- **Use consistent logging**: IMP_LOG_* macros with module-specific TAG
- **Document thoroughly**: Both code and configuration options

### Module Development Workflow:
1. **Plan the module**: Define functionality and configuration needs
2. **Create structure**: Set up directory and files following the template
3. **Implement incrementally**: Start with basic lifecycle, add functionality gradually
4. **Test frequently**: Build and test after each major addition
5. **Integrate properly**: Update build system and main.c registration
6. **Document**: Update configuration files and add usage documentation

### Real-World Success:
The OSD module conversion demonstrates that complex existing functionality (2000+ lines) can be successfully modularized while:
- Maintaining all existing functionality
- Improving code organization
- Enabling conditional compilation
- Preserving performance characteristics
- Providing clean integration points

This modular approach ensures that the Thingino Streamer can grow and adapt to new requirements while maintaining its efficiency and reliability on embedded hardware platforms.
