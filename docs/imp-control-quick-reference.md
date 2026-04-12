# IMP Control Module - Quick Reference

## API Endpoints

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/imp/params.json` | GET | Get current parameters |
| `/imp/params` | POST | Set parameters |
| `/imp/save` | POST | Save to config |
| `/imp/reset` | POST | Reset to defaults |

## Parameters Quick Reference

### Basic Image Quality
```bash
curl -X POST http://camera-ip:8080/imp/params -d '{
  "brightness": 128,     # 0-255, default 128
  "contrast": 128,       # 0-255, default 128
  "saturation": 128,     # 0-255, default 128
  "sharpness": 128       # 0-255, default 128
}'
```

### Advanced Controls
```bash
curl -X POST http://camera-ip:8080/imp/params -d '{
  "hue": 128,                    # 0-255, default 128
  "ae_compensation": 128,        # 0-255, default 128
  "noise_reduction_2d": 128,     # 0-255, default 128
  "noise_reduction_3d": 128,     # 0-255, default 128
  "flip_horizontal": false,      # true/false, default false
  "flip_vertical": false,        # true/false, default false
  "day_night_mode": "auto"       # "day"/"night"/"auto", default "auto"
}'
```

### Professional Features
```bash
curl -X POST http://camera-ip:8080/imp/params -d '{
  "anti_flicker": "disable",           # "disable"/"50hz"/"60hz", default "disable"
  "backlight_compensation": 0,         # 0-10, default 0
  "highlight_suppression": 0,          # 0-10, default 0
  "white_balance_mode": "auto",        # "auto"/"manual", default "auto"
  "white_balance_r_gain": 256,         # 0-1023, default 256
  "white_balance_b_gain": 256,         # 0-1023, default 256
  "drc_strength": 128,                 # 0-255, default 128
  "defog_strength": 0                  # 0-255, default 0
}'
```

## Common Use Cases

### Day Mode Optimization
```bash
curl -X POST http://camera-ip:8080/imp/params -d '{
  "day_night_mode": "day",
  "brightness": 128,
  "contrast": 128,
  "saturation": 128,
  "anti_flicker": "50hz",
  "noise_reduction_2d": 100,
  "noise_reduction_3d": 100
}'
```

### Night Mode Optimization
```bash
curl -X POST http://camera-ip:8080/imp/params -d '{
  "day_night_mode": "night",
  "brightness": 140,
  "contrast": 120,
  "noise_reduction_2d": 180,
  "noise_reduction_3d": 160,
  "drc_strength": 200
}'
```

### Indoor Lighting (Anti-flicker)
```bash
curl -X POST http://camera-ip:8080/imp/params -d '{
  "anti_flicker": "50hz",
  "brightness": 135,
  "contrast": 125
}'
```

### Backlight Compensation
```bash
curl -X POST http://camera-ip:8080/imp/params -d '{
  "backlight_compensation": 5,
  "highlight_suppression": 3,
  "ae_compensation": 160
}'
```

### Manual White Balance (Warm Indoor)
```bash
curl -X POST http://camera-ip:8080/imp/params -d '{
  "white_balance_mode": "manual",
  "white_balance_r_gain": 300,
  "white_balance_b_gain": 400
}'
```

### Manual White Balance (Cool Outdoor)
```bash
curl -X POST http://camera-ip:8080/imp/params -d '{
  "white_balance_mode": "manual",
  "white_balance_r_gain": 400,
  "white_balance_b_gain": 300
}'
```

### High Noise Environment
```bash
curl -X POST http://camera-ip:8080/imp/params -d '{
  "noise_reduction_2d": 180,
  "noise_reduction_3d": 160,
  "brightness": 135
}'
```

### High Dynamic Range Scene
```bash
curl -X POST http://camera-ip:8080/imp/params -d '{
  "drc_strength": 200,
  "highlight_suppression": 3,
  "backlight_compensation": 3
}'
```

### Foggy/Hazy Conditions
```bash
curl -X POST http://camera-ip:8080/imp/params -d '{
  "defog_strength": 100,
  "contrast": 140,
  "drc_strength": 150
}'
```

### Ceiling Mount (Rotate 180°)
```bash
curl -X POST http://camera-ip:8080/imp/params -d '{
  "flip_horizontal": true,
  "flip_vertical": true
}'
```

## Configuration Management

### Get Current Settings
```bash
curl http://camera-ip:8080/imp/params.json
```

### Save Current Settings as Defaults
```bash
curl -X POST http://camera-ip:8080/imp/save
```

### Reset to Saved Defaults
```bash
curl -X POST http://camera-ip:8080/imp/reset
```

## Troubleshooting

### Image Too Dark
```bash
curl -X POST http://camera-ip:8080/imp/params -d '{
  "brightness": 150,
  "ae_compensation": 160,
  "drc_strength": 180
}'
```

### Image Too Bright
```bash
curl -X POST http://camera-ip:8080/imp/params -d '{
  "brightness": 110,
  "highlight_suppression": 5,
  "ae_compensation": 100
}'
```

### Too Much Noise
```bash
curl -X POST http://camera-ip:8080/imp/params -d '{
  "noise_reduction_2d": 180,
  "noise_reduction_3d": 160
}'
```

### Color Issues
```bash
curl -X POST http://camera-ip:8080/imp/params -d '{
  "saturation": 128,
  "hue": 128,
  "white_balance_mode": "auto"
}'
```

### Flickering Under Lights
```bash
curl -X POST http://camera-ip:8080/imp/params -d '{
  "anti_flicker": "50hz"
}'
# Use "60hz" for North America/60Hz regions
```

## Python One-liner Examples

```python
# Get current settings
import requests; print(requests.get('http://camera-ip:8080/imp/params.json').json())

# Set day mode
import requests; requests.post('http://camera-ip:8080/imp/params', json={'day_night_mode': 'day', 'brightness': 128})

# Set night mode
import requests; requests.post('http://camera-ip:8080/imp/params', json={'day_night_mode': 'night', 'brightness': 140, 'noise_reduction_2d': 180})

# Save settings
import requests; requests.post('http://camera-ip:8080/imp/save')
```

## Shell Script Template

```bash
#!/bin/bash
CAMERA_IP="192.168.1.109"
BASE_URL="http://${CAMERA_IP}:8080/imp"

# Get current settings
get_settings() {
    curl -s "${BASE_URL}/params.json" | jq '.'
}

# Set custom parameters
set_params() {
    curl -X POST "${BASE_URL}/params" \
        -H "Content-Type: application/json" \
        -d "$1"
}

# Usage examples:
# get_settings
# set_params '{"brightness": 140, "contrast": 130}'
```

## Configuration File Location

- **Development**: `./imp_control.json` (same directory as binary)
- **Production**: `/etc/streamer.d/imp_control.json`
- **Save operations always write to**: `/etc/streamer.d/imp_control.json`

## Build Configuration

Enable in build with:
```bash
make ENABLE_IMP_CONTROL=1
# or
./build.sh  # (if enabled in default config)
```

## Log Monitoring

```bash
# Check module status
grep "IMP_CONTROL" /tmp/streamer.log

# Monitor API requests
tail -f /tmp/streamer.log | grep "imp/"
```

---

**For complete documentation, see**: [imp-control-module.md](imp-control-module.md)
