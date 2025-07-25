# Motion Detection Module Documentation

## Overview

The Motion Detection module provides hardware-accelerated motion detection capabilities for the Thingino Streamer using Ingenic's IVS (Intelligent Video Surveillance) technology. This module enables real-time motion detection with configurable regions of interest (ROI), sensitivity controls, and automated script execution.

## Table of Contents

1. [Features](#features)
2. [Architecture](#architecture)
3. [Configuration](#configuration)
4. [API Reference](#api-reference)
5. [Integration](#integration)
6. [Performance Considerations](#performance-considerations)
7. [Troubleshooting](#troubleshooting)
8. [Examples](#examples)

## Features

### Core Capabilities
- **Hardware-Accelerated Detection**: Uses Ingenic IMP_IVS for efficient motion detection
- **Multi-ROI Support**: Configure up to 4 regions of interest with individual sensitivity settings
- **Configurable Sensitivity**: Supports sensitivity levels 0-4 (normal cameras) or 0-8 (panoramic cameras)
- **Script Execution**: Automatically execute custom scripts when motion is detected
- **Resource Efficient**: Client-aware processing and configurable frame skipping
- **Timing Controls**: Cooldown, debounce, and minimum motion duration settings

### Technical Features
- **Modular Architecture**: Fully integrated with Thingino's module system
- **Thread-Safe**: Dedicated detection thread with proper synchronization
- **JSON Configuration**: Easy configuration through JSON files
- **Statistics Tracking**: Motion events, frames processed, and performance metrics
- **Lifecycle Management**: Proper initialization, start, stop, and cleanup procedures

## Architecture

### Module Structure
```
src/modules/motion/
├── motion_module.h          # Module interface and configuration structures
├── motion_module.c          # Core implementation (~760 lines)
├── Makefile.motion         # Build configuration
└── README.md               # Module-specific documentation

res/config/
└── motion.json             # Default configuration file
```

### Core Components

#### 1. Motion Detection Engine
- **IVS Interface**: `IMP_IVS_CreateMoveInterface()` for hardware acceleration
- **Detection Thread**: Dedicated thread for continuous motion monitoring
- **Result Processing**: Analyzes motion detection results from IVS

#### 2. Configuration System
- **JSON-based**: Human-readable configuration format
- **Runtime Validation**: Parameter validation during initialization
- **Default Values**: Sensible defaults for all parameters

#### 3. Event Management
- **Motion State Tracking**: Tracks current motion status and timing
- **Event-Driven Scripts**: Executes scripts once at motion start and once at motion stop
- **Process Management**: Clean fork/wait handling with proper process cleanup
- **Statistics Collection**: Comprehensive metrics for monitoring

### Data Flow
```
Video Frame → IVS Motion Detection → Result Analysis → Event Processing → Script Execution
     ↓              ↓                    ↓              ↓               ↓
Frame Manager → Motion Thread → ROI Checking → State Update → Action Trigger
```

## Configuration

### Configuration Files
The motion detection system uses two separate configuration files:

- **Motion Settings**: `/etc/streamer.d/motion.json` - Detection parameters and behavior
- **Zone Definitions**: `/etc/streamer.d/roi.json` - ROI zones (managed by web UI)

### Configuration File Locations
- **Development**: `./motion.json` and `./roi.json` (same directory as binary)
- **Runtime**: `/etc/streamer.d/motion.json` and `/etc/streamer.d/roi.json`

### Configuration Parameters

#### Motion Settings (`motion.json`)
```json
{
  "enabled": true,                    // Enable/disable motion detection
  "monitor_stream": 1,                // Stream channel to monitor (0 or 1)
  "sensitivity": 3,                   // Motion sensitivity (0-4 normal, 0-8 panoramic)
  "skip_frame_count": 5,              // Frames to skip between detections
  "ivs_polling_timeout": 1000,        // IVS polling timeout (milliseconds)

  "cooldown_time": 5,                 // Cooldown between events (seconds)
  "debounce_time": 0,                 // Debounce time (seconds)
  "init_time": 5,                     // Initialization delay (seconds)
  "min_time": 1,                      // Minimum motion duration (seconds)
  "post_time": 0,                     // Post-motion recording time (seconds)

  "script_path": "/usr/sbin/motion"   // Script to execute on motion events
}
```

**Key Features:**
- **Automatic Frame Detection**: Frame dimensions are automatically detected from the selected channel
- **No Manual Dimensions**: No need to specify frame width/height - they're detected dynamically
- **Channel Flexibility**: Works with any channel resolution (main stream, sub-stream, etc.)
- **Event-Driven Scripts**: Scripts execute once at motion start and once at motion stop with parameters

#### Zone Configuration (`roi.json`)
The zone configuration uses the existing web UI format and is managed separately:

```json
{
  "zones": [
    {
      "id": 1,                        // Zone ID
      "type": "include",              // "include" or "exclude"
      "x": 0,                         // Zone top-left X coordinate
      "y": 0,                         // Zone top-left Y coordinate
      "width": 0,                     // Zone width (0 = auto-detect full frame)
      "height": 0,                    // Zone height (0 = auto-detect full frame)
      "name": "Full Frame"            // Zone name/description
    },
    {
      "id": 2,
      "type": "include",
      "x": 100,
      "y": 100,
      "width": 500,
      "height": 300,
      "name": "Entrance Area"
    }
  ],
  "settings": {
    "enabled": true,                  // Zone system enabled
    "sensitivity": "30",              // Web UI sensitivity (0-100, converted to 0-8)
    "minObjectSize": "5"              // Minimum object size percentage
  }
}
```

**Zone Features:**
- **Web UI Integration**: Uses the same format as the existing web UI zone system
- **Include/Exclude Types**: Support for both inclusion and exclusion zones
- **Auto-Detection**: Width/height of 0 automatically uses full frame dimensions
- **Multiple Zones**: Support for up to 4 zones per configuration

#### Script Execution
The motion detection system executes scripts with event-driven parameters:

```bash
# Motion event lifecycle
/usr/sbin/motion start    # Called once when motion begins
/usr/sbin/motion stop     # Called once when motion ends
```

**Script Implementation Example:**
```bash
#!/bin/sh
case "$1" in
    start)
        echo "Motion started at $(date)"
        # Handle motion start logic
        # Set flags, send notifications, start recording, etc.
        ;;
    stop)
        echo "Motion stopped at $(date)"
        # Handle motion stop logic
        # Clear flags, stop recording, send completion notifications, etc.
        ;;
    *)
        echo "Usage: $0 {start|stop}"
        exit 1
        ;;
esac
```

**Script Features:**
- **Event Parameters**: Script receives "start" or "stop" as first parameter
- **Clean Execution**: Each script call waits for completion before next event
- **No Conflicts**: Eliminates multiple script instance conflicts
- **Process Management**: Proper fork/wait handling with 100ms timeout

### Parameter Validation

#### Sensitivity Levels
- **Motion Settings**: 0-4 for normal cameras, 0-8 for panoramic cameras
- **Web UI Settings**: 0-100 (automatically converted to 0-8 scale)
- **Lower values**: Less sensitive (fewer false positives)
- **Higher values**: More sensitive (may increase false positives)

#### Zone Configuration
- **Maximum Zones**: 4 zones supported
- **Zone Types**: "include" (detect motion) or "exclude" (ignore motion)
- **Auto-Detection**: Width/height of 0 automatically uses detected frame size
- **Coordinates**: Must be within frame boundaries (validated after auto-detection)
- **Overlap**: Zones can overlap

#### Timing Parameters
- **cooldown_time**: Prevents rapid successive motion events
- **debounce_time**: Filters out brief motion interruptions
- **min_time**: Ensures motion events meet minimum duration
- **init_time**: Allows system stabilization before detection starts

#### Script Execution Behavior
- **Motion Start**: Script executed once with "start" parameter when motion begins
- **Motion Stop**: Script executed once with "stop" parameter when motion ends
- **Process Management**: Each script execution waits for completion (100ms timeout)
- **No Cooldowns**: No artificial delays between script executions
- **Clean State**: Proper process cleanup prevents conflicts

#### Automatic Frame Detection
- **Dynamic Resolution**: Frame dimensions automatically detected from selected channel
- **Channel Flexibility**: Works with main stream (1920x1080) or sub-stream (640x360)
- **No Configuration Errors**: Eliminates channel/dimension mismatches
- **Zero Dimension Handling**: Zones with 0x0 dimensions automatically use full frame size

## API Reference

### Module Interface Functions

#### Lifecycle Management
```c
int motion_module_init(void* config);           // Initialize module with configuration
int motion_module_start(void);                 // Start motion detection
int motion_module_stop(void);                  // Stop motion detection
int motion_module_cleanup(void);               // Cleanup resources
```

#### Configuration Functions
```c
int motion_module_config_parse(json_object* json, void* config);    // Parse JSON config
int motion_module_config_validate(void* config);                    // Validate config
void motion_module_config_free(void* config);                       // Free config
```

#### Status and Statistics
```c
bool motion_module_is_motion_detected(void);           // Check current motion status
unsigned long motion_module_get_motion_events(void);   // Get total motion events
unsigned long motion_module_get_frames_processed(void); // Get frames processed
```

#### RTSP Integration
```c
int motion_module_set_rtsp_server(struct rtsp_server* server);  // Set RTSP server reference
```

### Configuration Structure
```c
typedef struct {
    bool enabled;                               // Enable/disable module
    int monitor_stream;                         // Stream to monitor
    int sensitivity;                            // Motion sensitivity
    int skip_frame_count;                       // Frame skip count
    int ivs_polling_timeout;                    // Polling timeout

    // Timing parameters
    int cooldown_time, debounce_time, init_time, min_time, post_time;

    // Frame parameters (auto-detected)
    int frame_width, frame_height;              // Detected from channel

    // Zone configuration (loaded from roi.json)
    int zone_count;                             // Number of zones
    struct {
        int id;                                 // Zone ID
        char type[16];                          // "include" or "exclude"
        int x, y, width, height;                // Zone coordinates
        char name[64];                          // Zone name
    } zones[4];

    int min_object_size;                        // Minimum object size percentage
    char script_path[256];                      // Script execution path
} motion_module_config_t;
```

## Integration

### Build System Integration

#### Buildroot Configuration
```bash
# Enable motion detection in buildroot
BR2_PACKAGE_THINGINO_STREAMER_MOTION=y
```

#### Compile-time Flags
```makefile
# Enable motion detection module
ENABLE_MOTION=1
```

### Module Registration
The motion detection module is automatically registered during application startup:

```c
#ifdef ENABLE_MOTION_MODULE
    extern module_info_t motion_module_info;
    if (module_register(&motion_module_info) != 0) {
        IMP_LOG_WARN(TAG, "Failed to register motion detection module");
    }
#endif
```

### Frame Manager Integration
The motion detection module integrates with the frame manager for efficient video processing:

- **Frame Source**: Receives frames from the configured monitor stream
- **Processing**: Analyzes frames using IVS hardware acceleration
- **Dynamic Resolution**: Automatically detects frame dimensions from channel configuration
- **Resource Management**: Only processes when clients are connected (optional)

### Web UI Integration
The motion detection system seamlessly integrates with the existing web UI:

- **Zone Management**: Uses the same zone format as the web UI
- **Separate Configuration**: Motion settings and zones are managed independently
- **Real-time Updates**: Zone changes in web UI are reflected in motion detection
- **Visual Configuration**: Zones can be drawn and configured graphically

## Performance Considerations

### Hardware Requirements
- **SoC**: Ingenic T31/T31X with IVS support
- **Memory**: ~150KB additional memory usage
- **CPU**: Minimal CPU overhead due to hardware acceleration

### Optimization Settings

#### Frame Skip Count
- **Higher values**: Reduce CPU usage, may miss brief motion
- **Lower values**: More responsive detection, higher CPU usage
- **Recommended**: 3-5 for most applications

#### ROI Configuration
- **Fewer ROIs**: Better performance
- **Smaller ROIs**: Reduced processing overhead
- **Strategic placement**: Focus on important areas only

#### Sensitivity Tuning
- **Start low**: Begin with sensitivity 1-2 to minimize false positives
- **Adjust gradually**: Increase sensitivity if motion is missed
- **Environment-specific**: Tune based on lighting and scene conditions

### Resource Management
- **Client-aware processing**: Only processes when RTSP clients are connected
- **Configurable timeouts**: Adjust polling timeout based on requirements
- **Memory efficient**: Minimal memory allocation during runtime

## Troubleshooting

### Common Issues

#### 1. Motion Detection Not Working
**Symptoms**: No motion events detected despite movement in scene

**Solutions**:
- Verify `enabled: true` in configuration
- Check sensitivity settings (try increasing from 1 to 2-3)
- Ensure ROI regions cover areas with expected motion
- Verify frame dimensions match actual stream resolution
- Check IVS initialization in logs

#### 2. Too Many False Positives
**Symptoms**: Motion detected when no actual movement occurs

**Solutions**:
- Reduce sensitivity level (try 0 or 1)
- Increase `skip_frame_count` to reduce processing frequency
- Configure smaller, more specific ROI regions
- Add `debounce_time` to filter brief interruptions
- Check for environmental factors (lighting changes, shadows)

#### 3. Script Not Executing
**Symptoms**: Motion detected but script not running

**Solutions**:
- Verify script path exists and is executable: `chmod +x /usr/sbin/motion`
- Check script permissions and ownership
- Test script manually: `/usr/sbin/motion`
- Review system logs for execution errors
- Ensure script doesn't require interactive input

#### 4. High CPU Usage
**Symptoms**: System performance degraded with motion detection enabled

**Solutions**:
- Increase `skip_frame_count` (try 8-10)
- Reduce number of ROI regions
- Increase `ivs_polling_timeout`
- Configure smaller ROI regions
- Consider using lower resolution stream for detection

#### 5. Script Execution Issues
**Symptoms**: Scripts not executing or multiple script conflicts

**Solutions**:
- Ensure script is executable: `chmod +x /usr/sbin/motion`
- Check script handles both parameters: `start` and `stop`
- Verify script path in configuration
- Check system logs for script execution errors
- Test script manually: `/usr/sbin/motion start` and `/usr/sbin/motion stop`

**Script Template**:
```bash
#!/bin/sh
case "$1" in
    start)
        echo "Motion started: $(date)" >> /var/log/motion.log
        # Your motion start logic here
        ;;
    stop)
        echo "Motion stopped: $(date)" >> /var/log/motion.log
        # Your motion stop logic here
        ;;
    *)
        echo "Usage: $0 {start|stop}"
        exit 1
        ;;
esac
```

#### 6. Module Initialization Failures
**Symptoms**: Motion module fails to start

**Solutions**:
- Verify IVS support in hardware/firmware
- Check configuration file syntax: `jq . /etc/streamer.d/motion.json`
- Validate zone configuration: `jq . /etc/streamer.d/roi.json`
- Ensure frame dimensions are valid
- Verify zone regions are within frame boundaries
- Check available memory and system resources

### Debug Information

#### Log Messages
Motion detection module provides detailed logging:

```bash
# Enable debug logging
export IMP_LOG_LEVEL=DEBUG

# Key log messages to monitor:
[I] MOTION: Motion detection module initialized successfully
[I] MOTION: IVS motion detection setup completed successfully
[I] MOTION: Motion detected! Event #1
[I] MOTION: Executing motion script: /usr/sbin/motion start
[D] MOTION: Motion script started with PID 12345 (action: start)
[D] MOTION: Motion script completed successfully (action: start)
[I] MOTION: Motion ended after 3 seconds
[I] MOTION: Executing motion script: /usr/sbin/motion stop
[D] MOTION: Motion script started with PID 12346 (action: stop)
[D] MOTION: Motion script completed successfully (action: stop)
```

#### Configuration Validation
```bash
# Validate motion configuration
jq '.' /etc/streamer.d/motion.json

# Validate zone configuration
jq '.' /etc/streamer.d/roi.json

# Check if motion module is enabled in build
strings /usr/bin/streamer | grep -i motion
```

#### Runtime Status
```bash
# Check if motion detection thread is running
ps aux | grep streamer

# Monitor motion events (if logging enabled)
tail -f /var/log/messages | grep MOTION
```

## Examples

### Example 1: Basic Full-Frame Detection

**motion.json:**
```json
{
  "enabled": true,
  "monitor_stream": 1,
  "sensitivity": 3,
  "skip_frame_count": 5,
  "cooldown_time": 10,
  "init_time": 5,
  "min_time": 1,
  "script_path": "/usr/sbin/motion_alert"
}
```

**roi.json:**
```json
{
  "zones": [
    {
      "id": 1,
      "type": "include",
      "x": 0,
      "y": 0,
      "width": 0,
      "height": 0,
      "name": "Full Frame"
    }
  ],
  "settings": {
    "enabled": true,
    "sensitivity": "30",
    "minObjectSize": "5"
  }
}
```

### Example 2: Multi-Zone Detection

**motion.json:**
```json
{
  "enabled": true,
  "monitor_stream": 0,
  "sensitivity": 4,
  "skip_frame_count": 3,
  "cooldown_time": 5,
  "script_path": "/usr/sbin/zone_motion"
}
```

**roi.json:**
```json
{
  "zones": [
    {
      "id": 1,
      "type": "include",
      "x": 0,
      "y": 0,
      "width": 640,
      "height": 540,
      "name": "Left Zone"
    },
    {
      "id": 2,
      "type": "include",
      "x": 640,
      "y": 0,
      "width": 640,
      "height": 540,
      "name": "Center Zone"
    },
    {
      "id": 3,
      "type": "include",
      "x": 1280,
      "y": 0,
      "width": 640,
      "height": 540,
      "name": "Right Zone"
    }
  ],
  "settings": {
    "enabled": true,
    "sensitivity": "40",
    "minObjectSize": "3"
  }
}
```

### Example 3: Perimeter Detection with Exclusion Zone

**motion.json:**
```json
{
  "enabled": true,
  "monitor_stream": 1,
  "sensitivity": 4,
  "skip_frame_count": 2,
  "cooldown_time": 3,
  "debounce_time": 1,
  "min_time": 2,
  "script_path": "/usr/sbin/perimeter_alert"
}
```

**roi.json:**
```json
{
  "zones": [
    {
      "id": 1,
      "type": "include",
      "x": 0,
      "y": 0,
      "width": 0,
      "height": 200,
      "name": "Top Perimeter"
    },
    {
      "id": 2,
      "type": "include",
      "x": 0,
      "y": 880,
      "width": 0,
      "height": 200,
      "name": "Bottom Perimeter"
    },
    {
      "id": 3,
      "type": "exclude",
      "x": 400,
      "y": 300,
      "width": 200,
      "height": 150,
      "name": "Tree Area"
    }
  ],
  "settings": {
    "enabled": true,
    "sensitivity": "60",
    "minObjectSize": "2"
  }
}
```

### Example 4: Low-Resource Configuration

**motion.json:**
```json
{
  "enabled": true,
  "monitor_stream": 1,
  "sensitivity": 2,
  "skip_frame_count": 10,
  "ivs_polling_timeout": 2000,
  "cooldown_time": 15,
  "init_time": 10,
  "script_path": "/usr/sbin/motion_simple"
}
```

**roi.json:**
```json
{
  "zones": [
    {
      "id": 1,
      "type": "include",
      "x": 160,
      "y": 90,
      "width": 320,
      "height": 180,
      "name": "Center Area"
    }
  ],
  "settings": {
    "enabled": true,
    "sensitivity": "20",
    "minObjectSize": "8"
  }
}
```

**Note**: This example uses a smaller zone in the center of the sub-stream (640x360) to reduce processing load.

### Sample Motion Scripts

#### Basic Alert Script (`/usr/sbin/motion_alert`)
```bash
#!/bin/sh
# Basic motion detection alert script with start/stop events

TIMESTAMP=$(date '+%Y-%m-%d %H:%M:%S')

case "$1" in
    start)
        echo "[$TIMESTAMP] Motion started!" >> /var/log/motion.log
        # Optional: Send start notification, begin recording, etc.
        # curl -X POST "http://notification-server/alert" -d "motion_start"
        ;;
    stop)
        echo "[$TIMESTAMP] Motion stopped!" >> /var/log/motion.log
        # Optional: Send stop notification, end recording, etc.
        # curl -X POST "http://notification-server/alert" -d "motion_stop"
        ;;
    *)
        echo "Usage: $0 {start|stop}"
        exit 1
        ;;
esac
```

#### Zone-Based Script (`/usr/sbin/zone_motion`)
```bash
#!/bin/sh
# Zone-based motion detection with start/stop events

TIMESTAMP=$(date '+%Y-%m-%d %H:%M:%S')
MOTION_STATE_FILE="/tmp/motion_state"

case "$1" in
    start)
        echo "[$TIMESTAMP] Zone motion started" >> /var/log/motion.log
        echo "active" > "$MOTION_STATE_FILE"

        # Trigger zone-specific start actions
        HOUR=$(date '+%H')
        if [ "$HOUR" -ge 22 ] || [ "$HOUR" -le 6 ]; then
            # Night mode - send immediate alert
            curl -X POST "http://alert-server/night-motion"
        fi
        ;;
    stop)
        echo "[$TIMESTAMP] Zone motion stopped" >> /var/log/motion.log
        rm -f "$MOTION_STATE_FILE"
        # Trigger zone-specific stop actions
        ;;
    *)
        echo "Usage: $0 {start|stop}"
        exit 1
        ;;
esac
```

## Integration with Other Systems

### Home Automation
```bash
# Example: Home Assistant integration
curl -X POST \
  -H "Authorization: Bearer YOUR_TOKEN" \
  -H "Content-Type: application/json" \
  -d '{"state": "motion_detected", "attributes": {"timestamp": "'$(date -Iseconds)'"}}' \
  "http://homeassistant.local:8123/api/states/binary_sensor.camera_motion"
```

### Recording Integration
```bash
# Example: Trigger recording on motion
ffmpeg -f rtsp -i rtsp://camera.local/stream1 \
  -t 30 -c copy "/recordings/motion_$(date +%Y%m%d_%H%M%S).mp4"
```

### Notification Systems
```bash
# Example: Send email notification based on event
case "$1" in
    start)
        echo "Motion started at $(date)" | \
          mail -s "Camera Motion Alert - Started" admin@example.com
        ;;
    stop)
        echo "Motion ended at $(date)" | \
          mail -s "Camera Motion Alert - Ended" admin@example.com
        ;;
esac
```

## Best Practices

### Configuration Guidelines
1. **Start Conservative**: Begin with low sensitivity and adjust upward
2. **Test Thoroughly**: Validate detection in various lighting conditions
3. **Monitor Performance**: Check CPU usage and adjust frame skip accordingly
4. **Use Appropriate Zones**: Focus on areas where motion is expected
5. **Set Reasonable Cooldowns**: Prevent notification spam
6. **Leverage Auto-Detection**: Use 0x0 dimensions for full-frame zones
7. **Web UI Integration**: Configure zones visually through the web interface

### Security Considerations
1. **Script Security**: Ensure motion scripts are secure and validated
2. **File Permissions**: Restrict access to configuration files
3. **Network Security**: Secure any network notifications or integrations
4. **Log Management**: Rotate and secure motion detection logs

### Maintenance
1. **Regular Testing**: Periodically verify motion detection functionality
2. **Configuration Backup**: Backup both motion.json and roi.json configurations
3. **Performance Monitoring**: Monitor system resources and adjust as needed
4. **Update Management**: Keep firmware and software updated
5. **Zone Validation**: Ensure zones remain valid after resolution changes
6. **Web UI Sync**: Keep zone configurations synchronized between web UI and motion detection

---

## Technical Implementation Details

### IVS Integration
The motion detection module leverages Ingenic's IVS (Intelligent Video Surveillance) hardware acceleration:

- **Hardware Acceleration**: Offloads motion detection from CPU to dedicated hardware
- **Real-time Processing**: Processes video frames in real-time without significant latency
- **Low Power Consumption**: Efficient hardware implementation reduces power usage
- **Scalable Performance**: Handles multiple ROI regions efficiently

### Thread Architecture
```
Main Thread
    ├── Module System
    ├── Frame Manager
    └── Motion Module
            └── Detection Thread (dedicated)
                    ├── IVS Polling
                    ├── Result Processing
                    └── Event Handling
```

### Memory Management
- **Static Allocation**: Configuration and state structures use static allocation
- **Minimal Runtime Allocation**: No dynamic memory allocation during detection
- **Resource Cleanup**: Proper cleanup of IVS resources on module shutdown
- **Thread Safety**: Mutex protection for shared state variables

This comprehensive documentation provides everything needed to understand, configure, and troubleshoot the motion detection module in the Thingino Streamer.
