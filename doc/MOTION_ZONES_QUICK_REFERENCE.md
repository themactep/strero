# Motion Zone Visualization - Quick Reference

## Quick Start

### Enable Zone Visualization
```c
// Enable for all streams
motion_module_enable_zone_visualization(true);

// Enable for specific stream
osd_enable_motion_zones(stream_id, true);
```

### Set Zone Colors
```c
// Set colors (BGRA format)
osd_set_motion_zone_colors(stream_id, 
                           0xFF00FF00,  // Green for include zones
                           0xFF0000FF); // Red for exclude zones
```

### Update Zone Display
```c
// Refresh zones (usually automatic)
osd_update_motion_zones(stream_id);
```

## Color Reference

### Common Colors (BGRA Format)
```c
#define GREEN_INCLUDE    0xFF00FF00  // Bright green
#define RED_EXCLUDE      0xFF0000FF  // Bright red
#define BLUE_INCLUDE     0xFFFF0000  // Bright blue
#define YELLOW_EXCLUDE   0xFF00FFFF  // Bright yellow
#define CYAN_INCLUDE     0xFFFFFF00  // Bright cyan
#define MAGENTA_EXCLUDE  0xFFFF00FF  // Bright magenta
#define WHITE_ZONES      0xFFFFFFFF  // White
#define ORANGE_ZONES     0xFF0080FF  // Orange
```

### Color Format Conversion
```c
// RGBA to BGRA conversion
uint32_t rgba_to_bgra(uint32_t rgba) {
    return ((rgba & 0xFF) << 16) |      // R -> B
           (rgba & 0xFF00) |            // G -> G
           ((rgba >> 16) & 0xFF) |      // B -> R
           (rgba & 0xFF000000);         // A -> A
}
```

## Zone Scaling Reference

### Coordinate System Detection
| Max Coordinates | Detected System | Common Use |
|----------------|-----------------|------------|
| ≤ 640x360      | 640x360        | Sub-stream zones |
| ≤ 1280x720     | 1280x720       | HD zones |
| > 1280x720     | 1920x1080      | Full HD zones |

### Scaling Examples
| Original Zone | Stream Resolution | Scaled Zone | Scale Factor |
|---------------|------------------|-------------|--------------|
| (160,90,320,180) | 1920x1080 | (480,270,960,540) | 3.0x |
| (480,270,960,540) | 640x360 | (160,90,320,180) | 0.33x |
| (0,0,640,360) | 1920x1080 | (0,0,1920,1080) | 3.0x |

## API Quick Reference

### Core Functions
```c
// Zone visualization control
int osd_enable_motion_zones(int group_id, bool enabled);
int osd_update_motion_zones(int group_id);
int osd_set_motion_zone_colors(int group_id, uint32_t include_color, uint32_t exclude_color);

// Motion module integration
int motion_module_enable_zone_visualization(bool enabled);
int motion_module_get_zones(int* zone_count, void** zones_data);
```

### Return Values
- **0**: Success
- **-1**: Error (invalid parameters, not initialized, etc.)

## Configuration Examples

### Example 1: Basic Setup
```c
// Enable visualization for both streams
osd_enable_motion_zones(0, true);  // Main stream
osd_enable_motion_zones(1, true);  // Sub stream

// Set standard colors
osd_set_motion_zone_colors(0, 0xFF00FF00, 0xFF0000FF);
osd_set_motion_zone_colors(1, 0xFF00FF00, 0xFF0000FF);
```

### Example 2: Custom Colors per Stream
```c
// Main stream: Green include, Red exclude
osd_set_motion_zone_colors(0, 0xFF00FF00, 0xFF0000FF);

// Sub stream: Cyan include, Magenta exclude  
osd_set_motion_zone_colors(1, 0xFFFFFF00, 0xFFFF00FF);
```

### Example 3: Dynamic Toggle
```c
static bool zones_visible = false;

void toggle_zones() {
    zones_visible = !zones_visible;
    motion_module_enable_zone_visualization(zones_visible);
}
```

## Troubleshooting Quick Fixes

### Zones Not Visible
```bash
# Check if enabled
tail -f /var/log/messages | grep "Motion zone.*enabled"

# Verify OSD initialization
tail -f /var/log/messages | grep "OSD.*initialized"

# Check zone data
tail -f /var/log/messages | grep "Got.*motion zones"
```

### Wrong Zone Sizes
```bash
# Check coordinate detection
tail -f /var/log/messages | grep "Zone scaling.*original"

# Monitor scaling calculations
tail -f /var/log/messages | grep "Zone.*scaled:"
```

### Performance Issues
```c
// Reduce line width
ctx->motion_zones.line_width = 1;

// Disable when not needed
osd_enable_motion_zones(stream_id, false);
```

## Debug Commands

### Enable Debug Logging
```bash
export IMP_LOG_LEVEL=DEBUG
```

### Monitor Zone Activity
```bash
# Zone visualization events
tail -f /var/log/messages | grep -E "(Zone scaling|Motion zone.*visualized)"

# OSD region management
tail -f /var/log/messages | grep "OSD.*motion.*zone"

# Motion module updates
tail -f /var/log/messages | grep "Successfully updated OSD motion zones"
```

### Verify Configuration
```bash
# Check zone configuration
jq '.' /etc/streamer.d/roi.json

# Validate motion settings
jq '.' /etc/streamer.d/motion.json

# Check module status
ps aux | grep streamer
```

## Common Zone Configurations

### Full Frame Zone
```json
{
  "id": 1,
  "type": "include",
  "x": 0,
  "y": 0,
  "width": 0,
  "height": 0,
  "name": "Full Frame"
}
```

### Center Area Zone
```json
{
  "id": 2,
  "type": "include",
  "x": 160,
  "y": 90,
  "width": 320,
  "height": 180,
  "name": "Center Area"
}
```

### Perimeter Zones
```json
{
  "zones": [
    {
      "id": 1,
      "type": "include",
      "x": 0,
      "y": 0,
      "width": 640,
      "height": 50,
      "name": "Top Edge"
    },
    {
      "id": 2,
      "type": "include",
      "x": 0,
      "y": 310,
      "width": 640,
      "height": 50,
      "name": "Bottom Edge"
    }
  ]
}
```

### Exclusion Zone Example
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
    },
    {
      "id": 2,
      "type": "exclude",
      "x": 200,
      "y": 150,
      "width": 240,
      "height": 120,
      "name": "Tree Area"
    }
  ]
}
```

## Performance Guidelines

### Optimal Settings
- **Line Width**: 1-2 pixels
- **Zone Count**: ≤ 4 zones per stream
- **Update Frequency**: Only when zones change
- **Stream Selection**: Enable only on needed streams

### Resource Usage
- **CPU Impact**: Minimal (hardware OSD)
- **Memory Usage**: ~1KB per zone per stream
- **Bandwidth**: No impact on video streams

## Integration Notes

### Web UI Integration
- Zones configured in web UI appear automatically
- Real-time preview of zone placement
- Visual feedback for zone configuration
- Consistent zone format between web UI and motion detection

### Motion Detection Integration
- Zones update automatically when motion module starts
- Zone changes reflected immediately on all streams
- Coordinate scaling preserves original zone definitions
- Multi-stream support ensures consistent coverage

This quick reference provides essential information for implementing and troubleshooting motion zone visualization.
