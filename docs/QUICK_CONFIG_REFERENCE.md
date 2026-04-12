# Thingino Streamer JSON Configuration Quick Reference

## Common Configuration Tasks

### Enable Motion Detection

```bash
jct /etc/streamer.json set motion.enabled true
jct /etc/streamer.json set motion.sensitivity 3
jct /etc/streamer.json set motion.cooldown_time 10
```

### Change Stream Quality

```bash
# Ultra High Quality (for local recording)
jct /etc/streamer.json set stream0.bitrate 8000
jct /etc/streamer.json set stream0.mode "VBR"
jct /etc/streamer.json set stream0.profile 2
jct /etc/streamer.json set stream0.max_bitrate 12000
jct /etc/streamer.json set stream0.min_qp 18
jct /etc/streamer.json set stream0.max_qp 35

# High Quality (for fast internet)
jct /etc/streamer.json set stream0.bitrate 5000
jct /etc/streamer.json set stream0.mode "VBR"
jct /etc/streamer.json set stream0.max_bitrate 7500
jct /etc/streamer.json set stream0.min_qp 20
jct /etc/streamer.json set stream0.max_qp 40

# Balanced Quality (default)
jct /etc/streamer.json set stream0.bitrate 3000
jct /etc/streamer.json set stream0.mode "CBR"
jct /etc/streamer.json set stream0.min_qp 25
jct /etc/streamer.json set stream0.max_qp 45

# Low Bandwidth (for slow internet)
jct /etc/streamer.json set stream1.bitrate 500
jct /etc/streamer.json set stream1.width 640
jct /etc/streamer.json set stream1.height 360
jct /etc/streamer.json set stream1.mode "CBR"
jct /etc/streamer.json set stream1.min_qp 30
jct /etc/streamer.json set stream1.max_qp 50
```

### Configure Authentication

```bash
# Enable HTTP authentication
jct /etc/streamer.json set http.auth.enabled true
jct /etc/streamer.json set http.auth.username "admin"
jct /etc/streamer.json set http.auth.password "secure123"

# Enable RTSP authentication
jct /etc/streamer.json set rtsp.auth.enabled true
jct /etc/streamer.json set rtsp.auth.username "admin"
jct /etc/streamer.json set rtsp.auth.password "secure123"

# Enable ONVIF authentication
jct /etc/streamer.json set onvif.auth.enabled true
jct /etc/streamer.json set onvif.auth.username "admin"
jct /etc/streamer.json set onvif.auth.password "secure123"

# Disable localhost bypass (force auth for all connections)
jct /etc/streamer.json set rtsp.auth.localhost_bypass false
```

### Configure Snapshot Fallback

```bash
# Enable snapshot fallback system
jct /etc/streamer.json set snapshot_fallback.enabled true

# Change capture interval to 10 seconds
jct /etc/streamer.json set snapshot_fallback.update_interval_ms 10000

# Change output directory
jct /etc/streamer.json set snapshot_fallback.output_dir "/var/snapshots"

# Configure file cleanup (1 hour = 3600 seconds)
jct /etc/streamer.json set snapshot_fallback.max_file_age_seconds 3600

# Disable automatic cleanup
jct /etc/streamer.json set snapshot_fallback.max_file_age_seconds 0
```

### Adjust Image Settings

```bash
# Flip image
jct /etc/streamer.json set image.vflip true
jct /etc/streamer.json set image.hflip false

# Adjust brightness and contrast
jct /etc/streamer.json set image.brightness 140
jct /etc/streamer.json set image.contrast 120
```

### Configure Audio

```bash
# Enable audio input
jct /etc/streamer.json set audio.input_enabled true
jct /etc/streamer.json set audio.input_format "OPUS"
jct /etc/streamer.json set audio.input_bitrate 64

# Enable on streams
jct /etc/streamer.json set stream0.audio_enabled true
jct /etc/streamer.json set stream1.audio_enabled true
```

### OSD Configuration

```bash
# Enable OSD
jct /etc/streamer.json set stream0.osd.enabled true
jct /etc/streamer.json set stream0.osd.time_enabled true
jct /etc/streamer.json set stream0.osd.user_text_enabled true

# Position elements
jct /etc/streamer.json set stream0.osd.pos_time_x 20
jct /etc/streamer.json set stream0.osd.pos_time_y 20
```

### WebSocket Settings

```bash
# Enable WebSocket
jct /etc/streamer.json set websocket.enabled true
jct /etc/streamer.json set websocket.port 8089
```

### JPEG Snapshots

```bash
# Configure snapshots
jct /etc/streamer.json set stream2.enabled true
jct /etc/streamer.json set stream2.jpeg_quality 85
jct /etc/streamer.json set stream2.jpeg_refresh 500
```

### Advanced Quality Tuning

```bash
# Enable adaptive features for better quality
jct /etc/streamer.json set stream0.adaptive_gop true
jct /etc/streamer.json set stream0.scene_change_detection true

# Fine-tune QP for specific scenarios
# For security cameras (prioritize detail)
jct /etc/streamer.json set stream0.min_qp 15
jct /etc/streamer.json set stream0.max_qp 35

# For wildlife cameras (balance quality/battery)
jct /etc/streamer.json set stream0.min_qp 25
jct /etc/streamer.json set stream0.max_qp 45

# For indoor monitoring (consistent quality)
jct /etc/streamer.json set stream0.mode "CBR"
jct /etc/streamer.json set stream0.min_qp 28
jct /etc/streamer.json set stream0.max_qp 42

# For outdoor monitoring (variable conditions)
jct /etc/streamer.json set stream0.mode "VBR"
jct /etc/streamer.json set stream0.max_bitrate 6000
jct /etc/streamer.json set stream0.min_qp 20
jct /etc/streamer.json set stream0.max_qp 50
```

## Quick Checks

### View Current Settings

```bash
# Check motion detection
jct /etc/streamer.json get motion.enabled

# Check stream settings
jct /etc/streamer.json get stream0.bitrate
jct /etc/streamer.json get stream0.mode

# Check RTSP settings
jct /etc/streamer.json get rtsp.port
jct /etc/streamer.json get rtsp.auth_required
```

### Print Sections

```bash
# Print entire config
jct /etc/streamer.json print

# Print specific sections (requires jq)
jct /etc/streamer.json print | jq .motion
jct /etc/streamer.json print | jq .stream0
jct /etc/streamer.json print | jq .rtsp
```

## Restart Service

After making changes, restart the service:

```bash
/etc/init.d/S95streamer restart
```

## Backup and Restore

### Backup Configuration

```bash
cp /etc/streamer.json /etc/streamer.json.backup
```

### Restore Configuration

```bash
cp /etc/streamer.json.backup /etc/streamer.json
/etc/init.d/S95streamer restart
```

## Validation

Always validate JSON after manual editing:

```bash
jq . /etc/streamer.json
```

If validation fails, restore from backup and try again.

## Default Values

If a setting is not specified in the JSON file, streamer will use its built-in default values. You only need to specify settings you want to change from the defaults.

## Color Values

OSD colors are specified as decimal ARGB values:

- White: `4294967295` (0xFFFFFFFF)
- Black: `4278190080` (0xFF000000)
- Red: `4294901760` (0xFFFF0000)
- Green: `4278255360` (0xFF00FF00)
- Blue: `4278190335` (0xFF0000FF)

## Format Strings

### Time Format

Uses strftime format:
- `%F %T`: 2023-12-25 14:30:45
- `%Y-%m-%d %H:%M:%S`: 2023-12-25 14:30:45
- `%a %b %d %H:%M`: Mon Dec 25 14:30

### User Text Variables

- `%hostname`: System hostname
- Custom text: Any static string

### Uptime Format

Uses printf format for hours:minutes:seconds:
- `"Uptime: %02lu:%02lu:%02lu"`: Uptime: 12:34:56
