# OSD Motion Zone Visualization

## Overview

The OSD (On-Screen Display) Motion Zone Visualization system provides real-time visual overlay of motion detection zones on video streams. This feature integrates with the motion detection module to display zone boundaries, helping users configure and troubleshoot motion detection settings.

## Architecture

### System Components

```
Motion Module → Zone Data → OSD Module → Hardware OSD → Video Stream
     ↓              ↓           ↓            ↓            ↓
Zone Config → Coordinates → Scaling → Rectangle → Visual Overlay
```

### Key Components

1. **Motion Module**: Provides zone configuration and coordinates
2. **OSD Module**: Handles scaling, rendering, and display management
3. **Hardware OSD**: T31 hardware acceleration for efficient rendering
4. **Multi-Stream Support**: Displays zones on all configured video streams

## Technical Implementation

### Coordinate System Management

#### Original Coordinate Detection
The system automatically detects the coordinate system used in zone definitions:

```c
// Detect original frame size from zone coordinates
int max_x = 0, max_y = 0;
for (int i = 0; i < zones_to_display; i++) {
    struct motion_zone* z = &zones[i];
    if (z->width > 0 && z->height > 0) {
        int right = z->x + z->width;
        int bottom = z->y + z->height;
        if (right > max_x) max_x = right;
        if (bottom > max_y) max_y = bottom;
    }
}

// Determine original coordinate system
if (max_x <= 640 && max_y <= 360) {
    original_width = 640; original_height = 360;      // Sub-stream
} else if (max_x <= 1280 && max_y <= 720) {
    original_width = 1280; original_height = 720;     // HD
} else {
    original_width = 1920; original_height = 1080;    // Full HD
}
```

#### Per-Stream Scaling
Each stream receives appropriately scaled zone overlays:

```c
// Calculate scaling factors for target stream
float scale_x = (float)ctx->stream_width / original_width;
float scale_y = (float)ctx->stream_height / original_height;

// Apply scaling to zone coordinates
zone_x = (int)(zone->x * scale_x);
zone_y = (int)(zone->y * scale_y);
zone_w = (int)(zone->width * scale_x);
zone_h = (int)(zone->height * scale_y);
```

### OSD Integration

#### Rectangle Rendering
Motion zones are rendered using hardware-accelerated OSD rectangles:

```c
// Configure OSD rectangle region
IMPOSDRgnAttr rAttr;
memset(&rAttr, 0, sizeof(IMPOSDRgnAttr));
rAttr.type = OSD_REG_RECT;
rAttr.rect.p0.x = zone_x;
rAttr.rect.p0.y = zone_y;
rAttr.rect.p1.x = zone_x + zone_w - 1;
rAttr.rect.p1.y = zone_y + zone_h - 1;
rAttr.fmt = PIX_FMT_MONOWHITE;
rAttr.data.lineRectData.color = zone_color;
rAttr.data.lineRectData.linewidth = line_width;
```

#### Color Management
Zone colors are configurable and type-specific:

```c
// Default colors (BGRA format)
#define DEFAULT_INCLUDE_COLOR 0xFF00FF00  // Green for include zones
#define DEFAULT_EXCLUDE_COLOR 0xFF0000FF  // Red for exclude zones

// Color conversion for OSD hardware
uint32_t osd_color = ((configured_color & 0xFF) << 16) |     // B->R
                     (configured_color & 0xFF00) |           // G->G  
                     ((configured_color >> 16) & 0xFF) |     // R->B
                     (configured_color & 0xFF000000);        // A->A
```

## Configuration

### Zone Visualization Settings

#### OSD Context Configuration
```c
struct {
    bool enabled;                    // Visualization enabled
    bool show_include_zones;         // Show include zones
    bool show_exclude_zones;         // Show exclude zones
    uint32_t include_color;          // BGRA color for include zones
    uint32_t exclude_color;          // BGRA color for exclude zones  
    uint32_t line_width;            // Border line width (pixels)
} motion_zones;
```

#### Default Settings
```c
// Initialize with sensible defaults
ctx->motion_zones.enabled = false;
ctx->motion_zones.show_include_zones = true;
ctx->motion_zones.show_exclude_zones = true;
ctx->motion_zones.include_color = 0xFF00FF00;  // Green
ctx->motion_zones.exclude_color = 0xFF0000FF;  // Red
ctx->motion_zones.line_width = 2;              // 2 pixels
```

### Runtime Configuration

#### Enable/Disable Visualization
```c
// Enable motion zone visualization for a stream
int osd_enable_motion_zones(int group_id, bool enabled);

// Enable for all streams (motion module level)
int motion_module_enable_zone_visualization(bool enabled);
```

#### Color Customization
```c
// Set custom colors for zone types
int osd_set_motion_zone_colors(int group_id, 
                               uint32_t include_color, 
                               uint32_t exclude_color);
```

#### Update Zone Display
```c
// Refresh zone visualization (called automatically)
int osd_update_motion_zones(int group_id);
```

## API Reference

### Core Functions

#### Zone Visualization Control
```c
// Enable/disable motion zone visualization for a specific stream
int osd_enable_motion_zones(int group_id, bool enabled);

// Update motion zone display with current zone data
int osd_update_motion_zones(int group_id);

// Set colors for include and exclude zones
int osd_set_motion_zone_colors(int group_id, uint32_t include_color, uint32_t exclude_color);
```

#### Motion Module Integration
```c
// Enable zone visualization for all streams
int motion_module_enable_zone_visualization(bool enabled);

// Get zone data for visualization
int motion_module_get_zones(int* zone_count, void** zones_data);
```

### Data Structures

#### Zone Data Structure
```c
struct motion_zone {
    int id;                    // Zone ID
    char type[16];             // "include" or "exclude"
    int x, y;                  // Zone coordinates (original)
    int width, height;         // Zone dimensions (original)
    char name[64];             // Zone name/identifier
};
```

#### OSD Context Motion Zones
```c
struct {
    bool enabled;              // Visualization enabled
    bool show_include_zones;   // Show include zones
    bool show_exclude_zones;   // Show exclude zones
    uint32_t include_color;    // BGRA color for include zones
    uint32_t exclude_color;    // BGRA color for exclude zones
    uint32_t line_width;       // Border line width
} motion_zones;
```

## Usage Examples

### Example 1: Basic Zone Visualization

```c
// Enable motion zone visualization for stream 0
osd_enable_motion_zones(0, true);

// Set custom colors
osd_set_motion_zone_colors(0, 0xFF00FF00, 0xFF0000FF);  // Green include, red exclude

// Update display (usually called automatically)
osd_update_motion_zones(0);
```

### Example 2: Multi-Stream Configuration

```c
// Enable visualization for all streams
motion_module_enable_zone_visualization(true);

// Customize colors for different streams
osd_set_motion_zone_colors(0, 0xFF00FF00, 0xFF0000FF);  // Stream 0: Green/Red
osd_set_motion_zone_colors(1, 0xFF00FFFF, 0xFFFF00FF);  // Stream 1: Cyan/Magenta
```

### Example 3: Dynamic Control

```c
// Toggle visualization based on user input
bool visualization_enabled = false;

void toggle_zone_visualization() {
    visualization_enabled = !visualization_enabled;
    
    for (int stream_id = 0; stream_id < 2; stream_id++) {
        osd_enable_motion_zones(stream_id, visualization_enabled);
    }
    
    printf("Zone visualization %s\n", 
           visualization_enabled ? "enabled" : "disabled");
}
```

## Troubleshooting

### Common Issues

#### Zones Not Visible
**Symptoms**: No zone rectangles appear on video stream

**Solutions**:
1. Verify visualization is enabled: `osd_enable_motion_zones(stream_id, true)`
2. Check motion module is running and zones are configured
3. Ensure OSD system is initialized for the target stream
4. Verify zone coordinates are within stream boundaries

#### Incorrect Zone Sizes
**Symptoms**: Zone rectangles are too small or too large

**Solutions**:
1. Check coordinate system detection in logs
2. Verify all zones use consistent coordinate system
3. Review scaling calculations in debug output
4. Ensure zone definitions match intended resolution

#### Performance Issues
**Symptoms**: Video performance degraded with zone visualization

**Solutions**:
1. Reduce line width: `ctx->motion_zones.line_width = 1`
2. Limit number of visible zones
3. Disable visualization when not needed
4. Check for OSD memory limitations

### Debug Information

#### Key Log Messages
```bash
# Zone scaling information
[I] OSD: Zone scaling for group 0: original=640x360, stream=1920x1080, scale=(3.000,3.000)

# Individual zone scaling
[I] OSD: Zone[0] 'Center Area' scaled: (160,90,320,180) -> (480,270,960,540)

# Visualization success
[I] OSD: Motion zone[0] 'Center Area' visualized successfully for group 0

# Update confirmation
[I] MOTION: Successfully updated OSD motion zones for stream 0
```

#### Debug Commands
```bash
# Enable detailed logging
export IMP_LOG_LEVEL=DEBUG

# Monitor zone visualization
tail -f /var/log/messages | grep -E "(Zone scaling|Motion zone.*visualized|OSD.*motion)"

# Check OSD region status
tail -f /var/log/messages | grep "OSD.*region"
```

## Best Practices

### Performance Optimization
1. **Minimal Line Width**: Use thin lines (1-2 pixels) for better performance
2. **Selective Enabling**: Enable visualization only when needed
3. **Stream Selection**: Consider which streams need zone display
4. **Zone Count**: Limit number of zones for optimal performance

### Visual Design
1. **Color Contrast**: Choose colors that contrast with typical video content
2. **Consistent Colors**: Use consistent color scheme across streams
3. **Zone Overlap**: Be aware of overlapping zones and color conflicts
4. **User Experience**: Consider end-user visibility and clarity

### Integration Guidelines
1. **Automatic Updates**: Zones update automatically when motion module changes
2. **Error Handling**: Handle OSD initialization failures gracefully
3. **Resource Management**: Clean up OSD resources on module shutdown
4. **Thread Safety**: Use proper synchronization for multi-threaded access

This documentation provides comprehensive coverage of the OSD motion zone visualization system, enabling effective implementation and troubleshooting.
