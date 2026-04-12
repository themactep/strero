# IMP Control Module Documentation

## Overview

The IMP Control Module provides comprehensive image quality control for Thingino cameras using the T31X ISP (Image Signal Processor). This module exposes 17 different image quality parameters through a clean REST API, allowing real-time adjustment of camera image settings.

## Features

- **17 Image Quality Parameters**: Complete control over brightness, contrast, saturation, sharpness, and advanced ISP features
- **Real-time Control**: Changes apply immediately to the hardware ISP
- **REST API**: Clean JSON-based HTTP endpoints for integration
- **Configuration Persistence**: Save settings to survive camera reboots
- **Thread-safe Operations**: Concurrent access protection with mutex locks
- **Dual Configuration System**: Support for both development and production configurations
- **Hardware Integration**: Direct T31X IMP API integration for optimal performance

## Architecture

### Module Structure

```
src/modules/imp_control/
├── imp_control.h          # Module interface and data structures
├── imp_control.c          # Core implementation
├── Makefile.imp_control   # Build configuration
└── README.md             # Module-specific documentation

res/config/
└── imp_control.json      # Default configuration file
```

### Integration Points

- **Module System**: Registered as a standard module with lifecycle management
- **HTTP Server**: Provides REST API endpoints under `/imp/` prefix
- **Configuration System**: Uses dual-path configuration loading
- **T31X IMP API**: Direct hardware integration for real-time control

## Parameters Reference

### Basic Image Quality (Priority 1)

| Parameter | Range | Default | Description | IMP API Function |
|-----------|-------|---------|-------------|------------------|
| `brightness` | 0-255 | 128 | Image brightness level | `IMP_ISP_Tuning_SetBrightness()` |
| `contrast` | 0-255 | 128 | Image contrast level | `IMP_ISP_Tuning_SetContrast()` |
| `saturation` | 0-255 | 128 | Color saturation level | `IMP_ISP_Tuning_SetSaturation()` |
| `sharpness` | 0-255 | 128 | Image sharpness level | `IMP_ISP_Tuning_SetSharpness()` |

### Advanced Image Quality (Priority 1)

| Parameter | Range | Default | Description | IMP API Function |
|-----------|-------|---------|-------------|------------------|
| `hue` | 0-255 | 128 | Color hue adjustment | `IMP_ISP_Tuning_SetBcshHue()` |
| `ae_compensation` | 0-255 | 128 | Auto-exposure compensation | `IMP_ISP_Tuning_SetAeComp()` |
| `noise_reduction_2d` | 0-255 | 128 | 2D noise reduction strength | `IMP_ISP_Tuning_SetSinterStrength()` |
| `noise_reduction_3d` | 0-255 | 128 | 3D noise reduction strength | `IMP_ISP_Tuning_SetTemperStrength()` |
| `flip_horizontal` | true/false | false | Horizontal image flip | `IMP_ISP_Tuning_SetISPHflip()` |
| `flip_vertical` | true/false | false | Vertical image flip | `IMP_ISP_Tuning_SetISPVflip()` |
| `day_night_mode` | "day"/"night"/"auto" | "auto" | Day/night mode control | `IMP_ISP_Tuning_SetISPRunningMode()` |

### Professional Features (Priority 2)

| Parameter | Range | Default | Description | IMP API Function |
|-----------|-------|---------|-------------|------------------|
| `anti_flicker` | "disable"/"50hz"/"60hz" | "disable" | Anti-flicker for indoor lighting | `IMP_ISP_Tuning_SetAntiFlickerAttr()` |
| `backlight_compensation` | 0-10 | 0 | Backlight compensation strength | `IMP_ISP_Tuning_SetBacklightComp()` |
| `highlight_suppression` | 0-10 | 0 | Highlight suppression strength | `IMP_ISP_Tuning_SetHiLightDepress()` |
| `white_balance_mode` | "auto"/"manual" | "auto" | White balance mode | `IMP_ISP_Tuning_SetWB()` |
| `white_balance_r_gain` | 0-1023 | 256 | Manual red gain (manual WB only) | `IMP_ISP_Tuning_SetWB()` |
| `white_balance_b_gain` | 0-1023 | 256 | Manual blue gain (manual WB only) | `IMP_ISP_Tuning_SetWB()` |
| `drc_strength` | 0-255 | 128 | Dynamic range compression | `IMP_ISP_Tuning_SetDRC_Strength()` |
| `defog_strength` | 0-255 | 0 | Defog processing strength | `IMP_ISP_Tuning_SetDefog_Strength()` |

## API Reference

### Base URL
All IMP control endpoints are available under the `/imp/` prefix:
```
http://<camera-ip>:8080/imp/
```

### Endpoints

#### GET /imp/params.json
Get current IMP parameters from hardware.

**Response:**
```json
{
  "brightness": 128,
  "contrast": 128,
  "saturation": 128,
  "sharpness": 128,
  "hue": 128,
  "ae_compensation": 128,
  "noise_reduction_2d": 128,
  "noise_reduction_3d": 128,
  "flip_horizontal": false,
  "flip_vertical": false,
  "day_night_mode": "day",
  "anti_flicker": "disable",
  "backlight_compensation": 0,
  "highlight_suppression": 0,
  "white_balance_mode": "auto",
  "white_balance_r_gain": 451,
  "white_balance_b_gain": 474,
  "drc_strength": 128,
  "defog_strength": 0,
  "timestamp": 1753078837
}
```

#### POST /imp/params
Set one or more IMP parameters.

**Request Body:**
```json
{
  "brightness": 140,
  "contrast": 120,
  "anti_flicker": "50hz"
}
```

**Response:**
```json
{
  "success": true,
  "brightness": 140,
  "contrast": 120,
  "saturation": 128,
  "sharpness": 128,
  "hue": 128,
  "ae_compensation": 128,
  "noise_reduction_2d": 128,
  "noise_reduction_3d": 128,
  "flip_horizontal": false,
  "flip_vertical": false,
  "day_night_mode": "day",
  "anti_flicker": "50hz",
  "backlight_compensation": 0,
  "highlight_suppression": 0,
  "white_balance_mode": "auto",
  "white_balance_r_gain": 451,
  "white_balance_b_gain": 474,
  "drc_strength": 128,
  "defog_strength": 0,
  "timestamp": 1753078837
}
```

#### POST /imp/save
Save current parameters to configuration file.

**Response:**
```json
{
  "success": true,
  "message": "IMP parameters saved to configuration",
  "brightness": 140,
  "contrast": 120,
  ...
  "timestamp": 1753078837
}
```

#### POST /imp/reset
Reset all parameters to configured defaults.

**Response:**
```json
{
  "success": true,
  "message": "IMP parameters reset to defaults",
  "brightness": 128,
  "contrast": 128,
  ...
  "timestamp": 1753078837
}
```

## Configuration

### Configuration File Structure

The module uses a dedicated configuration file `/etc/streamer.d/imp_control.json`:

```json
{
  "brightness": 128,
  "contrast": 128,
  "saturation": 128,
  "sharpness": 128,
  "hue": 128,
  "ae_compensation": 128,
  "noise_reduction_2d": 128,
  "noise_reduction_3d": 128,
  "flip_horizontal": false,
  "flip_vertical": false,
  "day_night_mode": "auto",
  "anti_flicker": "disable",
  "backlight_compensation": 0,
  "highlight_suppression": 0,
  "white_balance_mode": "auto",
  "white_balance_r_gain": 256,
  "white_balance_b_gain": 256,
  "drc_strength": 128,
  "defog_strength": 0
}
```

### Configuration Loading Priority

The module follows the standard module configuration hierarchy:

1. **Binary Directory** (Development): `./imp_control.json`
   - Used for testing from network shares
   - Takes priority if present
   - Never overwritten by save operations

2. **System Directory** (Production): `/etc/streamer.d/imp_control.json`
   - Standard deployment location
   - Used if binary directory config not found
   - Target for save operations

3. **Main Config Fallback** (Compatibility): `/etc/streamer.json` with `imp_control` section
   - Backward compatibility support
   - Used if no dedicated config files found
   - Not recommended for new deployments

## Usage Examples

### Basic Image Quality Adjustment

```bash
# Increase brightness and contrast
curl -X POST http://192.168.1.109:8080/imp/params \
  -H "Content-Type: application/json" \
  -d '{
    "brightness": 150,
    "contrast": 140,
    "saturation": 120
  }'

# Adjust sharpness and hue
curl -X POST http://192.168.1.109:8080/imp/params \
  -d '{"sharpness": 160, "hue": 140}'
```

### Advanced Features

```bash
# Enable 50Hz anti-flicker for indoor lighting
curl -X POST http://192.168.1.109:8080/imp/params \
  -d '{"anti_flicker": "50hz"}'

# Manual white balance for specific lighting conditions
curl -X POST http://192.168.1.109:8080/imp/params \
  -d '{
    "white_balance_mode": "manual",
    "white_balance_r_gain": 400,
    "white_balance_b_gain": 350
  }'

# Enhanced night mode settings
curl -X POST http://192.168.1.109:8080/imp/params \
  -d '{
    "day_night_mode": "night",
    "noise_reduction_2d": 180,
    "noise_reduction_3d": 160,
    "drc_strength": 200
  }'
```

### Challenging Lighting Conditions

```bash
# Backlight compensation for subjects against bright backgrounds
curl -X POST http://192.168.1.109:8080/imp/params \
  -d '{
    "backlight_compensation": 5,
    "highlight_suppression": 3,
    "ae_compensation": 160
  }'

# Defog processing for hazy conditions
curl -X POST http://192.168.1.109:8080/imp/params \
  -d '{"defog_strength": 100}'

# High dynamic range scenes
curl -X POST http://192.168.1.109:8080/imp/params \
  -d '{"drc_strength": 200}'
```

### Image Orientation

```bash
# Flip image for ceiling-mounted cameras
curl -X POST http://192.168.1.109:8080/imp/params \
  -d '{"flip_horizontal": true, "flip_vertical": true}'

# Rotate 180 degrees
curl -X POST http://192.168.1.109:8080/imp/params \
  -d '{"flip_horizontal": true, "flip_vertical": true}'
```

### Configuration Management

```bash
# Get current settings
curl http://192.168.1.109:8080/imp/params.json

# Save current settings as defaults
curl -X POST http://192.168.1.109:8080/imp/save

# Reset to saved defaults
curl -X POST http://192.168.1.109:8080/imp/reset
```

## Integration Examples

### Shell Script Integration

```bash
#!/bin/bash
# Camera image quality control script

CAMERA_IP="192.168.1.109"
BASE_URL="http://${CAMERA_IP}:8080/imp"

# Function to set day mode
set_day_mode() {
    curl -X POST "${BASE_URL}/params" \
        -H "Content-Type: application/json" \
        -d '{
            "day_night_mode": "day",
            "brightness": 128,
            "contrast": 128,
            "saturation": 128,
            "anti_flicker": "50hz"
        }'
}

# Function to set night mode
set_night_mode() {
    curl -X POST "${BASE_URL}/params" \
        -H "Content-Type: application/json" \
        -d '{
            "day_night_mode": "night",
            "brightness": 140,
            "noise_reduction_2d": 180,
            "noise_reduction_3d": 160,
            "drc_strength": 200
        }'
}

# Function to get current settings
get_settings() {
    curl -s "${BASE_URL}/params.json" | jq '.'
}

case "$1" in
    day)
        echo "Setting day mode..."
        set_day_mode
        ;;
    night)
        echo "Setting night mode..."
        set_night_mode
        ;;
    status)
        echo "Current settings:"
        get_settings
        ;;
    *)
        echo "Usage: $0 {day|night|status}"
        exit 1
        ;;
esac
```

### Python Integration

```python
#!/usr/bin/env python3
"""
IMP Control Module Python Integration Example
"""

import requests
import json
from typing import Dict, Any, Optional

class IMPController:
    def __init__(self, camera_ip: str, port: int = 8080):
        self.base_url = f"http://{camera_ip}:{port}/imp"

    def get_params(self) -> Dict[str, Any]:
        """Get current IMP parameters"""
        response = requests.get(f"{self.base_url}/params.json")
        response.raise_for_status()
        return response.json()

    def set_params(self, params: Dict[str, Any]) -> Dict[str, Any]:
        """Set IMP parameters"""
        response = requests.post(
            f"{self.base_url}/params",
            headers={"Content-Type": "application/json"},
            json=params
        )
        response.raise_for_status()
        return response.json()

    def save_params(self) -> Dict[str, Any]:
        """Save current parameters to configuration"""
        response = requests.post(f"{self.base_url}/save")
        response.raise_for_status()
        return response.json()

    def reset_params(self) -> Dict[str, Any]:
        """Reset parameters to defaults"""
        response = requests.post(f"{self.base_url}/reset")
        response.raise_for_status()
        return response.json()

    def set_day_mode(self):
        """Configure optimal day mode settings"""
        return self.set_params({
            "day_night_mode": "day",
            "brightness": 128,
            "contrast": 128,
            "saturation": 128,
            "anti_flicker": "50hz",
            "noise_reduction_2d": 100,
            "noise_reduction_3d": 100
        })

    def set_night_mode(self):
        """Configure optimal night mode settings"""
        return self.set_params({
            "day_night_mode": "night",
            "brightness": 140,
            "contrast": 120,
            "noise_reduction_2d": 180,
            "noise_reduction_3d": 160,
            "drc_strength": 200
        })

    def set_indoor_lighting(self, frequency: str = "50hz"):
        """Configure for indoor lighting with anti-flicker"""
        return self.set_params({
            "anti_flicker": frequency,
            "brightness": 135,
            "contrast": 125
        })

    def set_backlight_compensation(self, strength: int = 5):
        """Configure backlight compensation"""
        return self.set_params({
            "backlight_compensation": strength,
            "highlight_suppression": 3,
            "ae_compensation": 160
        })

# Usage example
if __name__ == "__main__":
    # Initialize controller
    imp = IMPController("192.168.1.109")

    # Get current settings
    current = imp.get_params()
    print("Current settings:", json.dumps(current, indent=2))

    # Set day mode
    result = imp.set_day_mode()
    print("Day mode set:", result["success"])

    # Save settings
    save_result = imp.save_params()
    print("Settings saved:", save_result["success"])
```

## Troubleshooting

### Common Issues

#### Module Not Initialized
```
[E] IMP_CONTROL: IMP control module not initialized
```
**Solution**: Ensure the module is enabled in the build configuration and the camera has been restarted.

#### Configuration Loading Failed
```
[W] IMP_CONTROL: Failed to load configuration, using defaults
```
**Solution**: Check that `/etc/streamer.d/imp_control.json` exists and has valid JSON format.

#### API Endpoint Not Found
```
HTTP 404 Not Found
```
**Solution**: Verify the HTTP module is enabled and the IMP control module is properly registered.

#### Parameter Out of Range
**Solution**: Check parameter ranges in the reference table above. Values outside valid ranges may be clamped or rejected.

### Debug Information

Enable debug logging to troubleshoot issues:

```bash
# Check module registration
grep "IMP_CONTROL" /tmp/streamer.log

# Check configuration loading
grep "imp_control" /tmp/streamer.log

# Check API requests
grep "imp/" /tmp/streamer.log
```

### Hardware Limitations

- Some parameters may have hardware-specific ranges or behavior
- Not all combinations of parameters may be supported simultaneously
- Changes to certain parameters (like day/night mode) may take several seconds to fully apply
- White balance gains are only effective in manual white balance mode

## Performance Considerations

### Memory Usage
- Module uses minimal memory footprint (~2KB for state and configuration)
- Thread-safe operations use mutex locks (minimal performance impact)
- JSON parsing is optimized for small payloads

### Response Times
- Parameter changes apply immediately to hardware
- API response times typically < 50ms
- Configuration file operations may take 100-200ms

### Concurrent Access
- Multiple API requests are handled safely with mutex protection
- No limit on concurrent read operations
- Write operations are serialized to prevent conflicts

## Development

### Building the Module

The module is automatically included when `ENABLE_IMP_CONTROL=1` is set in the build configuration.

```bash
# Build with IMP control module
make ENABLE_IMP_CONTROL=1

# Or use the build script
./build.sh
```

### Module Structure

```c
// Core data structures
typedef struct {
    imp_control_config_t config;
    imp_control_params_t current_params;
    pthread_mutex_t params_mutex;
    bool initialized;
    bool running;
} imp_control_state_t;

// Module lifecycle
int imp_control_init(void* config);
int imp_control_start(void);
int imp_control_stop(void);
int imp_control_cleanup(void);

// Parameter control
int imp_control_get_params(imp_control_params_t* params);
int imp_control_set_params(const imp_control_params_t* params);
```

### Adding New Parameters

To add new IMP parameters:

1. **Update structures** in `imp_control.h`:
   ```c
   typedef struct {
       // ... existing parameters
       unsigned char new_parameter;
   } imp_control_config_t;
   ```

2. **Add IMP API calls** in `apply_imp_params()`:
   ```c
   ret = IMP_ISP_Tuning_SetNewParameter(params->new_parameter);
   ```

3. **Update JSON handling** in parsing and response functions

4. **Add individual setter function**:
   ```c
   int imp_control_set_new_parameter(unsigned char value);
   ```

5. **Update configuration defaults** in `imp_control_set_defaults()`

6. **Update documentation** and API reference

## License

This module is part of the Thingino project and is licensed under the same terms as the main project.

## Contributing

Contributions are welcome! Please follow the project's coding standards and include appropriate tests for new features.

### Code Style
- Use consistent indentation (4 spaces)
- Follow existing naming conventions
- Add comprehensive error handling
- Include detailed logging for debugging
- Document all public functions

### Testing
- Test all parameter ranges and edge cases
- Verify thread safety with concurrent access
- Test configuration loading and saving
- Validate API responses and error handling

## Advanced Topics

### Parameter Interactions

Some parameters work together to achieve optimal image quality:

#### Day/Night Mode Combinations
```bash
# Optimal day settings
curl -X POST http://camera-ip:8080/imp/params -d '{
  "day_night_mode": "day",
  "brightness": 128,
  "contrast": 128,
  "saturation": 128,
  "noise_reduction_2d": 100,
  "noise_reduction_3d": 100,
  "drc_strength": 128
}'

# Optimal night settings
curl -X POST http://camera-ip:8080/imp/params -d '{
  "day_night_mode": "night",
  "brightness": 140,
  "contrast": 120,
  "noise_reduction_2d": 180,
  "noise_reduction_3d": 160,
  "drc_strength": 200
}'
```

#### White Balance and Color Settings
```bash
# Warm indoor lighting compensation
curl -X POST http://camera-ip:8080/imp/params -d '{
  "white_balance_mode": "manual",
  "white_balance_r_gain": 300,
  "white_balance_b_gain": 400,
  "hue": 120,
  "saturation": 140
}'

# Cool outdoor lighting
curl -X POST http://camera-ip:8080/imp/params -d '{
  "white_balance_mode": "manual",
  "white_balance_r_gain": 400,
  "white_balance_b_gain": 300,
  "hue": 135,
  "saturation": 120
}'
```

### Automation Scripts

#### Time-based Automatic Adjustment
```bash
#!/bin/bash
# Automatic day/night switching based on time

CAMERA_IP="192.168.1.109"
HOUR=$(date +%H)

if [ $HOUR -ge 6 ] && [ $HOUR -lt 18 ]; then
    # Day mode (6 AM - 6 PM)
    curl -X POST "http://${CAMERA_IP}:8080/imp/params" -d '{
        "day_night_mode": "day",
        "brightness": 128,
        "contrast": 128,
        "anti_flicker": "50hz"
    }'
    echo "Switched to day mode"
else
    # Night mode (6 PM - 6 AM)
    curl -X POST "http://${CAMERA_IP}:8080/imp/params" -d '{
        "day_night_mode": "night",
        "brightness": 140,
        "noise_reduction_2d": 180,
        "noise_reduction_3d": 160
    }'
    echo "Switched to night mode"
fi
```

#### Motion Detection Integration
```bash
#!/bin/bash
# Adjust image quality when motion is detected

CAMERA_IP="192.168.1.109"

# Enhanced quality for motion recording
enhance_quality() {
    curl -X POST "http://${CAMERA_IP}:8080/imp/params" -d '{
        "brightness": 135,
        "contrast": 135,
        "sharpness": 160,
        "noise_reduction_2d": 120,
        "drc_strength": 150
    }'
}

# Standard quality for idle monitoring
standard_quality() {
    curl -X POST "http://${CAMERA_IP}:8080/imp/params" -d '{
        "brightness": 128,
        "contrast": 128,
        "sharpness": 128,
        "noise_reduction_2d": 128,
        "drc_strength": 128
    }'
}

# Usage: ./motion_quality.sh {enhance|standard}
case "$1" in
    enhance) enhance_quality ;;
    standard) standard_quality ;;
    *) echo "Usage: $0 {enhance|standard}" ;;
esac
```

### Home Assistant Integration

```yaml
# configuration.yaml
rest_command:
  camera_day_mode:
    url: "http://192.168.1.109:8080/imp/params"
    method: POST
    headers:
      Content-Type: "application/json"
    payload: >
      {
        "day_night_mode": "day",
        "brightness": 128,
        "contrast": 128,
        "anti_flicker": "50hz"
      }

  camera_night_mode:
    url: "http://192.168.1.109:8080/imp/params"
    method: POST
    headers:
      Content-Type: "application/json"
    payload: >
      {
        "day_night_mode": "night",
        "brightness": 140,
        "noise_reduction_2d": 180,
        "noise_reduction_3d": 160
      }

sensor:
  - platform: rest
    name: "Camera Image Settings"
    resource: "http://192.168.1.109:8080/imp/params.json"
    json_attributes:
      - brightness
      - contrast
      - saturation
      - day_night_mode
      - anti_flicker
    value_template: "{{ value_json.timestamp }}"

automation:
  - alias: "Camera Auto Day/Night"
    trigger:
      - platform: sun
        event: sunrise
      - platform: sun
        event: sunset
    action:
      - choose:
          - conditions:
              - condition: sun
                after: sunrise
                before: sunset
            sequence:
              - service: rest_command.camera_day_mode
          - conditions:
              - condition: sun
                after: sunset
            sequence:
              - service: rest_command.camera_night_mode
```

### Node-RED Integration

```json
[
    {
        "id": "camera_control",
        "type": "http request",
        "name": "Set Camera Params",
        "method": "POST",
        "ret": "obj",
        "paytoqs": "ignore",
        "url": "http://192.168.1.109:8080/imp/params",
        "headers": [
            {
                "keyType": "Content-Type",
                "keyValue": "",
                "valueType": "application/json",
                "valueValue": ""
            }
        ]
    },
    {
        "id": "day_night_switch",
        "type": "function",
        "name": "Day/Night Logic",
        "func": "const hour = new Date().getHours();\nconst isDayTime = hour >= 6 && hour < 18;\n\nif (isDayTime) {\n    msg.payload = {\n        \"day_night_mode\": \"day\",\n        \"brightness\": 128,\n        \"contrast\": 128,\n        \"anti_flicker\": \"50hz\"\n    };\n} else {\n    msg.payload = {\n        \"day_night_mode\": \"night\",\n        \"brightness\": 140,\n        \"noise_reduction_2d\": 180,\n        \"noise_reduction_3d\": 160\n    };\n}\n\nreturn msg;"
    }
]
```

## Best Practices

### Image Quality Optimization

1. **Start with defaults**: Begin with default values and make incremental adjustments
2. **Test in target environment**: Lighting conditions vary significantly between locations
3. **Use anti-flicker**: Enable appropriate anti-flicker (50Hz/60Hz) for indoor installations
4. **Balance noise reduction**: Higher values reduce noise but may soften details
5. **Adjust for use case**: Security cameras may prioritize detail over aesthetics

### Configuration Management

1. **Save working configurations**: Use `/imp/save` to persist good settings
2. **Document changes**: Keep notes on parameter combinations that work well
3. **Test before deployment**: Verify settings work in actual conditions
4. **Use version control**: Track configuration file changes in deployment scripts

### Performance Optimization

1. **Batch parameter changes**: Set multiple parameters in one API call when possible
2. **Avoid frequent changes**: Minimize parameter adjustments during active recording
3. **Monitor system resources**: Image processing changes can affect CPU usage
4. **Test concurrent access**: Verify behavior with multiple API clients

## Troubleshooting Guide

### Image Quality Issues

#### Image Too Dark
```bash
# Increase brightness and adjust exposure compensation
curl -X POST http://camera-ip:8080/imp/params -d '{
  "brightness": 150,
  "ae_compensation": 160,
  "drc_strength": 180
}'
```

#### Image Too Bright/Washed Out
```bash
# Reduce brightness and enable highlight suppression
curl -X POST http://camera-ip:8080/imp/params -d '{
  "brightness": 110,
  "highlight_suppression": 5,
  "ae_compensation": 100
}'
```

#### Excessive Noise
```bash
# Increase noise reduction
curl -X POST http://camera-ip:8080/imp/params -d '{
  "noise_reduction_2d": 180,
  "noise_reduction_3d": 160,
  "brightness": 135
}'
```

#### Color Issues
```bash
# Reset color settings
curl -X POST http://camera-ip:8080/imp/params -d '{
  "saturation": 128,
  "hue": 128,
  "white_balance_mode": "auto"
}'
```

#### Flickering Under Artificial Light
```bash
# Enable anti-flicker
curl -X POST http://camera-ip:8080/imp/params -d '{
  "anti_flicker": "50hz"
}'
# Use "60hz" for 60Hz power systems (North America, etc.)
```

### API Issues

#### Connection Refused
- Check if the HTTP module is enabled and running
- Verify the camera IP address and port (default 8080)
- Ensure firewall allows connections to port 8080

#### Invalid JSON Response
- Check request Content-Type header: `application/json`
- Validate JSON syntax in request body
- Verify parameter names match API specification exactly

#### Parameters Not Applied
- Check parameter value ranges in the reference table
- Verify the IMP control module is initialized successfully
- Check system logs for hardware-specific errors

### System Integration Issues

#### Module Not Loading
```bash
# Check module registration
grep "IMP_CONTROL" /tmp/streamer.log

# Verify build configuration
grep "ENABLE_IMP_CONTROL" /tmp/streamer.log
```

#### Configuration Not Persisting
```bash
# Check configuration file permissions
ls -la /etc/streamer.d/imp_control.json

# Verify save operation
curl -X POST http://camera-ip:8080/imp/save
```

## Support

For support and questions:
- GitHub Issues: [thingino-streamer issues](https://github.com/themactep/thingino-streamer/issues)
- Thingino Community: [Thingino Discord](https://discord.gg/thingino)
- Documentation: [Thingino Wiki](https://github.com/themactep/thingino-firmware/wiki)
