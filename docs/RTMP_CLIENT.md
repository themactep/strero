# RTMP Client Module Guide

The RTMP Client module enables live streaming from your Thingino camera to external streaming platforms like YouTube Live, Twitch, Facebook Live, and custom RTMP servers. This guide covers setup, configuration, and usage.

## Table of Contents

- [Overview](#overview)
- [Features](#features)
- [Quick Start](#quick-start)
- [Configuration](#configuration)
- [Supported Platforms](#supported-platforms)
- [Advanced Configuration](#advanced-configuration)
- [Troubleshooting](#troubleshooting)
- [Performance Considerations](#performance-considerations)
- [Security](#security)

## Overview

The RTMP Client module pushes your camera's video stream to external RTMP ingest servers, enabling live broadcasting to popular streaming platforms. It supports multiple simultaneous streams, automatic reconnection, and platform-specific optimizations.

### Architecture

```
Camera → Video Encoder → RTMP Client → External Platforms
                                    ├── YouTube Live
                                    ├── Twitch
                                    ├── Facebook Live
                                    ├── Instagram Live
                                    ├── TikTok Live
                                    ├── Twitter/X Live
                                    ├── Telegram Live
                                    ├── Periscope
                                    └── Custom Servers
```

## Features

- **Multi-Platform Streaming**: Stream to multiple platforms simultaneously
- **Auto-Reconnection**: Automatic retry with configurable intervals
- **Platform Templates**: Pre-configured settings for popular platforms
- **H.264/H.265 Support**: Compatible with modern video codecs
- **Bandwidth Management**: Configurable bitrate and FPS limits
- **Real-time Statistics**: Monitor connection status and performance
- **Protocol Support**: RTMP protocol with RTMPS parsing (TLS implementation planned)

## Quick Start

### 1. Enable RTMP Client

Edit `/etc/streamer.d/rtmp_client.json`:

```json
{
  "rtmp_client": {
    "enabled": true,
    "streams": [
      {
        "name": "youtube",
        "enabled": true,
        "url": "rtmp://a.rtmp.youtube.com/live2",
        "stream_key": "YOUR_YOUTUBE_STREAM_KEY"
      }
    ]
  }
}
```

### 2. Get Your Stream Key

1. **YouTube Live**: Go to YouTube Studio → Go Live → Stream Key
2. **Twitch**: Dashboard → Settings → Stream → Primary Stream Key
3. **Facebook**: Creator Studio → Live → Use Stream Key

### 3. Start Streaming

Restart the streamer service:
```bash
/etc/init.d/S95streamer restart
```

Your camera will automatically connect and start streaming!

## Configuration

### Basic Configuration Structure

```json
{
  "rtmp_client": {
    "enabled": true,
    "streams": [
      {
        "name": "platform_name",
        "enabled": true,
        "url": "rtmp://server.com/app",
        "stream_key": "your_stream_key",
        "retry_interval": 30,
        "max_retries": 5,
        "connection_timeout": 30
      }
    ],
    "video": {
      "channel": 0,
      "bitrate_limit": 4000,
      "fps_limit": 30
    },
    "connection": {
      "timeout": 30,
      "chunk_size": 4096,
      "keepalive_interval": 60
    }
  }
}
```

### Stream Configuration Parameters

| Parameter | Type | Description | Default |
|-----------|------|-------------|---------|
| `name` | string | Unique identifier for the stream | Required |
| `enabled` | boolean | Enable/disable this stream | `false` |
| `url` | string | RTMP server URL | Required |
| `stream_key` | string | Authentication key from platform | Required |
| `retry_interval` | integer | Seconds between retry attempts | `30` |
| `max_retries` | integer | Maximum retry attempts (0 = infinite) | `5` |
| `connection_timeout` | integer | Connection timeout in seconds | `30` |

### Video Configuration

| Parameter | Type | Description | Default |
|-----------|------|-------------|---------|
| `channel` | integer | Video channel to stream (0 or 1) | `0` |
| `bitrate_limit` | integer | Maximum bitrate in kbps (0 = no limit) | `0` |
| `fps_limit` | integer | Maximum FPS (0 = no limit) | `0` |

### Connection Configuration

| Parameter | Type | Description | Default |
|-----------|------|-------------|---------|
| `timeout` | integer | General connection timeout | `30` |
| `chunk_size` | integer | RTMP chunk size in bytes | `4096` |
| `keepalive_interval` | integer | Keepalive interval in seconds | `60` |

## Important Notes

### RTMPS (TLS) Support Status

**Current Status**: RTMPS URL parsing is supported, but TLS encryption is not yet implemented.

**Impact**: Most modern streaming platforms require RTMPS (encrypted RTMP) for security. The current implementation can parse `rtmps://` URLs but will attempt plain TCP connections, which will likely fail.

**Workarounds**:
1. **Use alternative endpoints**: Some platforms offer plain RTMP endpoints for testing
2. **Local proxy**: Use a local RTMP-to-RTMPS proxy (e.g., stunnel, nginx)
3. **Wait for TLS implementation**: TLS support is planned for future releases

**Platforms Affected**:
- Telegram Live (requires RTMPS)
- Most modern streaming services
- Corporate/enterprise RTMP servers

**Example Proxy Setup** (using stunnel):
```
# /etc/stunnel/rtmp.conf
[rtmp-telegram]
accept = 1935
connect = dc1-1.rtmp.t.me:443
cert = /etc/ssl/certs/stunnel.pem
```

Then use: `rtmp://localhost:1935/s/` instead of `rtmps://dc1-1.rtmp.t.me/s/`

## Supported Platforms

### YouTube Live ✅ (Plain RTMP Supported)

```json
{
  "name": "youtube",
  "enabled": true,
  "url": "rtmp://a.rtmp.youtube.com/live2",
  "stream_key": "YOUR_YOUTUBE_STREAM_KEY",
  "retry_interval": 30,
  "max_retries": 5
}
```

**Requirements:**
- YouTube channel with live streaming enabled
- Minimum 50 subscribers (for mobile streaming)
- No live streaming restrictions in past 90 days

**Recommended Settings:**
- Bitrate: 1000-4000 kbps
- FPS: 30
- Resolution: 1920x1080 or 1280x720

**Status**: ✅ **Works with current implementation** (uses plain RTMP)

### Twitch ✅ (Plain RTMP Supported)

```json
{
  "name": "twitch",
  "enabled": true,
  "url": "rtmp://live.twitch.tv/live",
  "stream_key": "YOUR_TWITCH_STREAM_KEY",
  "retry_interval": 30,
  "max_retries": 5
}
```

**Requirements:**
- Twitch account
- Stream key from Twitch Dashboard

**Recommended Settings:**
- Bitrate: 2500-6000 kbps
- FPS: 30 or 60
- Resolution: 1920x1080

**Status**: ✅ **Works with current implementation** (uses plain RTMP)

### Facebook Live ⚠️ (May Require RTMPS)

```json
{
  "name": "facebook",
  "enabled": true,
  "url": "rtmp://live-api-s.facebook.com:80/rtmp",
  "stream_key": "YOUR_FACEBOOK_STREAM_KEY",
  "retry_interval": 30,
  "max_retries": 3
}
```

**Requirements:**
- Facebook Page or Profile
- Stream key from Facebook Creator Studio

**Recommended Settings:**
- Bitrate: 2000-4000 kbps
- FPS: 30
- Resolution: 1280x720 or 1920x1080

**Status**: ⚠️ **May work** (Facebook sometimes accepts plain RTMP on port 80)

### Instagram Live

```json
{
  "name": "instagram",
  "enabled": true,
  "url": "rtmp://rtmp-api.instagram.com:80/rtmp",
  "stream_key": "YOUR_INSTAGRAM_STREAM_KEY",
  "retry_interval": 30,
  "max_retries": 3
}
```

**Note:** Instagram Live streaming via RTMP requires special API access.

### TikTok Live

```json
{
  "name": "tiktok",
  "enabled": true,
  "url": "rtmp://rtmp-push.tiktok.com/live",
  "stream_key": "YOUR_TIKTOK_STREAM_KEY",
  "retry_interval": 30,
  "max_retries": 3
}
```

**Requirements:**
- TikTok account with 1000+ followers
- Live streaming feature enabled

### Twitter/X Live

```json
{
  "name": "twitter",
  "enabled": true,
  "url": "rtmp://broadcasting-api.twitter.com/live",
  "stream_key": "YOUR_TWITTER_STREAM_KEY",
  "retry_interval": 30,
  "max_retries": 3
}
```

**Note:** Twitter Live streaming requires Media Studio access.

### Telegram Live ❌ (Requires RTMPS)

```json
{
  "name": "telegram",
  "enabled": false,
  "url": "rtmps://dc1-1.rtmp.t.me/s/",
  "stream_key": "YOUR_TELEGRAM_STREAM_KEY",
  "retry_interval": 30,
  "max_retries": 3
}
```

**Requirements:**
- Telegram channel or group
- Bot API access for stream key generation

**Status**: ❌ **Requires TLS** (not yet implemented)

**Workaround**: Use stunnel or nginx proxy to handle TLS encryption

### Custom RTMP Server

```json
{
  "name": "custom",
  "enabled": true,
  "url": "rtmp://your-server.com/live",
  "stream_key": "your_stream_key",
  "retry_interval": 30,
  "max_retries": 3
}
```

Use this template for any RTMP-compatible server.

## Advanced Configuration

### Multiple Platform Streaming

Stream to multiple platforms simultaneously:

```json
{
  "rtmp_client": {
    "enabled": true,
    "streams": [
      {
        "name": "youtube",
        "enabled": true,
        "url": "rtmp://a.rtmp.youtube.com/live2",
        "stream_key": "youtube_key"
      },
      {
        "name": "twitch",
        "enabled": true,
        "url": "rtmp://live.twitch.tv/live",
        "stream_key": "twitch_key"
      },
      {
        "name": "facebook",
        "enabled": true,
        "url": "rtmp://live-api-s.facebook.com:80/rtmp",
        "stream_key": "facebook_key"
      }
    ]
  }
}
```

### Bandwidth Optimization

For limited bandwidth, configure bitrate limits:

```json
{
  "video": {
    "channel": 0,
    "bitrate_limit": 2000,
    "fps_limit": 25
  }
}
```

### Regional Servers

Some platforms offer regional servers for better performance:

**YouTube Regional Servers:**
- `rtmp://a.rtmp.youtube.com/live2` (Primary)
- `rtmp://b.rtmp.youtube.com/live2` (Backup)

**Twitch Regional Servers:**
- `rtmp://live.twitch.tv/live` (Auto-select)
- `rtmp://live-fra.twitch.tv/live` (Frankfurt)
- `rtmp://live-lax.twitch.tv/live` (Los Angeles)

## Troubleshooting

### Common Issues

#### Connection Refused
```
RTMP Client: Failed to connect to server.com:1935
```

**Solutions:**
1. Check internet connectivity
2. Verify RTMP URL is correct
3. Check firewall settings
4. Try different regional server

#### Authentication Failed
```
RTMP Client: RTMP connect failed
```

**Solutions:**
1. Verify stream key is correct
2. Check if stream key has expired
3. Ensure platform account has streaming permissions
4. Try regenerating stream key

#### Stream Disconnects Frequently
```
RTMP Client: Connection lost, will retry in 30 seconds
```

**Solutions:**
1. Check network stability
2. Reduce bitrate limit
3. Increase retry interval
4. Check platform streaming limits

#### Poor Stream Quality
**Solutions:**
1. Increase bitrate limit
2. Check available bandwidth
3. Optimize video encoder settings
4. Use wired connection instead of WiFi

### Debug Mode

Enable debug logging by setting log level to DEBUG in main configuration.

### Log Analysis

Monitor RTMP client logs:
```bash
tail -f /var/log/streamer.log | grep "RTMP_CLIENT"
```

Common log messages:
- `RTMP connection thread started` - Connection initiated
- `RTMP handshake completed` - Handshake successful
- `Sent RTMP connect command` - Authentication started
- `RTMP publishing started` - Streaming active
- `Connection lost` - Stream disconnected

## Performance Considerations

### Memory Usage
- Each RTMP connection uses ~250KB of memory
- Multiple streams increase memory usage proportionally
- Monitor system memory with multiple active streams

### CPU Usage
- RTMP client adds minimal CPU overhead
- Video encoding is the primary CPU consumer
- H.265 encoding uses more CPU than H.264

### Network Bandwidth
- Calculate total bandwidth: `bitrate × number_of_streams`
- Leave 20% bandwidth headroom for stability
- Monitor network usage during streaming

### Recommended Limits
- **T31X (128MB RAM)**: Maximum 3-4 simultaneous streams
- **Network**: Ensure upload bandwidth > total stream bitrate × 1.2

## Security

### Stream Key Protection
- Never share stream keys publicly
- Regenerate keys if compromised
- Use environment variables for sensitive keys

### Network Security
- Use RTMPS when available (encrypted RTMP)
- Consider VPN for additional security
- Monitor for unauthorized access attempts

### Access Control
- Restrict configuration file permissions:
  ```bash
  chmod 600 /etc/streamer.d/rtmp_client.json
  ```

## API Integration

### Status Monitoring

Check RTMP client status via HTTP API:
```bash
curl http://camera-ip/api/status
```

### Configuration Updates

Update configuration via API:
```bash
curl -X POST http://camera-ip/api/config \
  -H "Content-Type: application/json" \
  -d @rtmp_client.json
```

## Best Practices

1. **Test Before Going Live**: Always test streams before important broadcasts
2. **Monitor Bandwidth**: Keep track of upload bandwidth usage
3. **Use Stable Network**: Wired connections are more reliable than WiFi
4. **Backup Streams**: Configure multiple platforms for redundancy
5. **Regular Maintenance**: Update stream keys and check platform requirements
6. **Quality Settings**: Balance quality with available bandwidth
7. **Monitoring**: Set up alerts for connection failures

## Support

For additional help:
- Check the [Troubleshooting](#troubleshooting) section
- Review system logs for error messages
- Test with a single platform first
- Verify platform-specific requirements
- Check network connectivity and bandwidth

## Examples

### Example 1: YouTube Live Setup

Complete setup for YouTube Live streaming:

1. **Get YouTube Stream Key:**
   - Go to YouTube Studio (studio.youtube.com)
   - Click "Go Live" → "Stream"
   - Copy the "Stream Key"

2. **Configure RTMP Client:**
   ```json
   {
     "rtmp_client": {
       "enabled": true,
       "streams": [
         {
           "name": "youtube",
           "enabled": true,
           "url": "rtmp://a.rtmp.youtube.com/live2",
           "stream_key": "abcd-efgh-ijkl-mnop-qrst",
           "retry_interval": 30,
           "max_retries": 5,
           "connection_timeout": 30
         }
       ],
       "video": {
         "channel": 0,
         "bitrate_limit": 3000,
         "fps_limit": 30
       }
     }
   }
   ```

3. **Start Streaming:**
   ```bash
   /etc/init.d/S95streamer restart
   ```

4. **Verify Stream:**
   - Check YouTube Studio for "Live" indicator
   - Monitor logs: `tail -f /var/log/streamer.log | grep RTMP_CLIENT`

### Example 2: Multi-Platform Setup

Stream to YouTube, Twitch, and Facebook simultaneously:

```json
{
  "rtmp_client": {
    "enabled": true,
    "streams": [
      {
        "name": "youtube",
        "enabled": true,
        "url": "rtmp://a.rtmp.youtube.com/live2",
        "stream_key": "youtube_stream_key_here"
      },
      {
        "name": "twitch",
        "enabled": true,
        "url": "rtmp://live.twitch.tv/live",
        "stream_key": "live_123456789_abcdefghijklmnop"
      },
      {
        "name": "facebook",
        "enabled": true,
        "url": "rtmp://live-api-s.facebook.com:80/rtmp",
        "stream_key": "FB-123456789-0-AbCdEfGhIjKlMnOp"
      }
    ],
    "video": {
      "channel": 0,
      "bitrate_limit": 4000,
      "fps_limit": 30
    },
    "connection": {
      "timeout": 30,
      "chunk_size": 4096
    }
  }
}
```

### Example 3: Low Bandwidth Setup

Optimized for limited upload bandwidth:

```json
{
  "rtmp_client": {
    "enabled": true,
    "streams": [
      {
        "name": "youtube",
        "enabled": true,
        "url": "rtmp://a.rtmp.youtube.com/live2",
        "stream_key": "your_stream_key"
      }
    ],
    "video": {
      "channel": 1,
      "bitrate_limit": 1500,
      "fps_limit": 25
    }
  }
}
```

### Example 4: Custom RTMP Server

Stream to your own RTMP server (e.g., Nginx with RTMP module):

```json
{
  "rtmp_client": {
    "enabled": true,
    "streams": [
      {
        "name": "my_server",
        "enabled": true,
        "url": "rtmp://my-server.example.com/live",
        "stream_key": "camera_01_secret_key",
        "retry_interval": 15,
        "max_retries": 10,
        "connection_timeout": 20
      }
    ]
  }
}
```

## Monitoring and Statistics

### Real-time Status

Check RTMP client status via HTTP API:

```bash
# Get overall status
curl http://192.168.1.100/api/modules/rtmp_client/status

# Example response:
{
  "enabled": true,
  "active_connections": 2,
  "total_streams": 3,
  "connections": [
    {
      "name": "youtube",
      "state": "publishing",
      "connected_time": 1234567890,
      "bytes_sent": 15728640,
      "frames_sent": 1250,
      "last_frame_time": 1234567900
    },
    {
      "name": "twitch",
      "state": "publishing",
      "connected_time": 1234567890,
      "bytes_sent": 12582912,
      "frames_sent": 1000,
      "last_frame_time": 1234567900
    }
  ]
}
```

### Log Monitoring

Monitor RTMP client activity:

```bash
# Follow RTMP client logs
tail -f /var/log/streamer.log | grep "RTMP_CLIENT"

# Filter for specific events
tail -f /var/log/streamer.log | grep -E "(RTMP_CLIENT|publishing|connection)"

# Check for errors only
tail -f /var/log/streamer.log | grep -E "(RTMP_CLIENT.*ERROR|RTMP_CLIENT.*Failed)"
```

### Performance Metrics

Key metrics to monitor:

1. **Connection Status**: Active/disconnected streams
2. **Bandwidth Usage**: Bytes sent per second
3. **Frame Rate**: Frames sent per second
4. **Retry Count**: Number of reconnection attempts
5. **Latency**: Time between frame capture and transmission

### Alerting

Set up monitoring scripts:

```bash
#!/bin/bash
# rtmp_monitor.sh - Check RTMP client health

STATUS=$(curl -s http://localhost/api/modules/rtmp_client/status)
ACTIVE=$(echo $STATUS | jq '.active_connections')
TOTAL=$(echo $STATUS | jq '.total_streams')

if [ "$ACTIVE" -lt "$TOTAL" ]; then
    echo "WARNING: Only $ACTIVE of $TOTAL RTMP streams active"
    # Send notification (email, webhook, etc.)
fi
```

## Integration with Other Modules

### RTMP Server + Client

Run both RTMP server and client simultaneously:

**Use Case**: Receive streams from OBS while broadcasting to platforms

```json
{
  "rtmp_server": {
    "enabled": true,
    "port": 1935
  },
  "rtmp_client": {
    "enabled": true,
    "streams": [
      {
        "name": "youtube",
        "enabled": true,
        "url": "rtmp://a.rtmp.youtube.com/live2",
        "stream_key": "your_key"
      }
    ]
  }
}
```

### Motion Detection Integration

Start streaming when motion is detected:

```bash
#!/bin/bash
# motion_stream.sh - Start streaming on motion

# Enable YouTube stream
curl -X POST http://localhost/api/modules/rtmp_client/streams/youtube/enable

# Wait for motion to stop
sleep 300

# Disable stream
curl -X POST http://localhost/api/modules/rtmp_client/streams/youtube/disable
```

### Scheduled Streaming

Automate streaming with cron:

```bash
# Start streaming at 9 AM
0 9 * * * curl -X POST http://localhost/api/modules/rtmp_client/enable

# Stop streaming at 5 PM
0 17 * * * curl -X POST http://localhost/api/modules/rtmp_client/disable
```

## Platform-Specific Tips

### YouTube Live
- **Latency**: Use "Low latency" mode for real-time interaction
- **Quality**: 1080p30 recommended for most use cases
- **Thumbnails**: Set custom thumbnail in YouTube Studio
- **Chat**: Enable live chat for viewer interaction

### Twitch
- **Categories**: Set appropriate game/category
- **Tags**: Use relevant tags for discoverability
- **Quality**: Higher bitrates allowed for Partners/Affiliates
- **VODs**: Enable VOD saving for later viewing

### Facebook Live
- **Audience**: Set audience (Public, Friends, Custom)
- **Description**: Add engaging description and hashtags
- **Notifications**: Enable notifications to followers
- **Crossposting**: Share to multiple pages if applicable

### TikTok Live
- **Requirements**: 1000+ followers needed
- **Duration**: Streams can last up to 60 minutes
- **Interaction**: Engage with comments and gifts
- **Hashtags**: Use trending hashtags for visibility

## Troubleshooting Scenarios

### Scenario 1: Stream Starts Then Stops

**Symptoms:**
- Connection established successfully
- Stream starts but stops after few seconds
- Logs show "Connection lost"

**Diagnosis:**
```bash
# Check bandwidth usage
iftop -i eth0

# Monitor system resources
top -p $(pgrep streamer)

# Check stream settings
curl http://localhost/api/modules/rtmp_client/status
```

**Solutions:**
1. Reduce bitrate limit
2. Check network stability
3. Verify platform streaming limits
4. Test with single platform first

### Scenario 2: Authentication Errors

**Symptoms:**
- Connection to server successful
- Handshake completes
- Connect command fails

**Diagnosis:**
```bash
# Verify stream key format
echo "Stream key length: $(echo $STREAM_KEY | wc -c)"

# Test with curl
curl -v "rtmp://a.rtmp.youtube.com/live2/$STREAM_KEY"
```

**Solutions:**
1. Regenerate stream key
2. Check key format (no spaces, special characters)
3. Verify platform account permissions
4. Test with different platform

### Scenario 3: High CPU Usage

**Symptoms:**
- System becomes slow during streaming
- High CPU usage by streamer process
- Frame drops or quality issues

**Diagnosis:**
```bash
# Monitor CPU usage
top -p $(pgrep streamer)

# Check encoder settings
cat /etc/streamer.json | jq '.stream0'

# Monitor temperature
cat /sys/class/thermal/thermal_zone0/temp
```

**Solutions:**
1. Reduce video resolution
2. Lower frame rate
3. Use H.264 instead of H.265
4. Reduce number of simultaneous streams

## Advanced Features

### Dynamic Stream Control

Enable/disable streams via API:

```bash
# Enable specific stream
curl -X POST http://localhost/api/modules/rtmp_client/streams/youtube/enable

# Disable specific stream
curl -X POST http://localhost/api/modules/rtmp_client/streams/youtube/disable

# Update stream key
curl -X PUT http://localhost/api/modules/rtmp_client/streams/youtube/key \
  -d '{"stream_key": "new_key_here"}'
```

### Failover Configuration

Automatic failover to backup servers:

```json
{
  "streams": [
    {
      "name": "youtube_primary",
      "enabled": true,
      "url": "rtmp://a.rtmp.youtube.com/live2",
      "stream_key": "your_key",
      "max_retries": 3
    },
    {
      "name": "youtube_backup",
      "enabled": false,
      "url": "rtmp://b.rtmp.youtube.com/live2",
      "stream_key": "your_key",
      "max_retries": 5
    }
  ]
}
```

### Custom Headers and Parameters

For platforms requiring special authentication:

```json
{
  "name": "custom_platform",
  "url": "rtmp://api.platform.com/live",
  "stream_key": "key_with_params?token=abc123",
  "connection": {
    "custom_headers": {
      "X-API-Key": "your_api_key",
      "Authorization": "Bearer token"
    }
  }
}
```

The RTMP Client module provides professional-grade live streaming capabilities for your Thingino camera, enabling broadcast to major platforms with reliability and performance.
