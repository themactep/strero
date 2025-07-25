# RTMP Client Quick Reference

## Quick Setup

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
        "stream_key": "YOUR_STREAM_KEY"
      }
    ]
  }
}
```

### 2. Restart Service
```bash
/etc/init.d/S95streamer restart
```

### 3. Monitor Status
```bash
tail -f /var/log/streamer.log | grep RTMP_CLIENT
```

## Platform URLs

| Platform | RTMP URL |
|----------|----------|
| YouTube Live | `rtmp://a.rtmp.youtube.com/live2` |
| Twitch | `rtmp://live.twitch.tv/live` |
| Facebook Live | `rtmp://live-api-s.facebook.com:80/rtmp` |
| Instagram Live | `rtmp://rtmp-api.instagram.com:80/rtmp` |
| TikTok Live | `rtmp://rtmp-push.tiktok.com/live` |
| Twitter/X Live | `rtmp://broadcasting-api.twitter.com/live` |
| Telegram Live | `rtmps://dc1-1.rtmp.t.me/s/` |
| Periscope | `rtmp://rtmp.pscp.tv:80/x` |

## Common Commands

### Check Status
```bash
curl http://localhost/api/modules/rtmp_client/status
```

### Enable/Disable Stream
```bash
# Enable
curl -X POST http://localhost/api/modules/rtmp_client/streams/youtube/enable

# Disable
curl -X POST http://localhost/api/modules/rtmp_client/streams/youtube/disable
```

### Monitor Logs
```bash
# All RTMP client logs
tail -f /var/log/streamer.log | grep RTMP_CLIENT

# Errors only
tail -f /var/log/streamer.log | grep "RTMP_CLIENT.*ERROR"

# Connection events
tail -f /var/log/streamer.log | grep -E "(publishing|connected|disconnected)"
```

## Configuration Templates

### Single Platform (YouTube)
```json
{
  "rtmp_client": {
    "enabled": true,
    "streams": [
      {
        "name": "youtube",
        "enabled": true,
        "url": "rtmp://a.rtmp.youtube.com/live2",
        "stream_key": "YOUR_YOUTUBE_KEY"
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

### Multi-Platform
```json
{
  "rtmp_client": {
    "enabled": true,
    "streams": [
      {
        "name": "youtube",
        "enabled": true,
        "url": "rtmp://a.rtmp.youtube.com/live2",
        "stream_key": "YOUTUBE_KEY"
      },
      {
        "name": "twitch",
        "enabled": true,
        "url": "rtmp://live.twitch.tv/live",
        "stream_key": "TWITCH_KEY"
      }
    ]
  }
}
```

### Low Bandwidth
```json
{
  "rtmp_client": {
    "enabled": true,
    "streams": [
      {
        "name": "youtube",
        "enabled": true,
        "url": "rtmp://a.rtmp.youtube.com/live2",
        "stream_key": "YOUR_KEY"
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

## Troubleshooting

### Connection Issues
1. Check internet connectivity
2. Verify RTMP URL and stream key
3. Test with single platform first
4. Check firewall settings

### Authentication Errors
1. Regenerate stream key
2. Check platform account permissions
3. Verify key format (no spaces)
4. Test with different platform

### Performance Issues
1. Reduce bitrate limit
2. Lower frame rate
3. Use channel 1 (lower resolution)
4. Monitor system resources

### Stream Quality Issues
1. Increase bitrate limit
2. Check available bandwidth
3. Use wired connection
4. Optimize encoder settings

## Log Messages

| Message | Meaning |
|---------|---------|
| `RTMP connection thread started` | Connection initiated |
| `RTMP handshake completed` | Handshake successful |
| `Sent RTMP connect command` | Authentication started |
| `RTMP publishing started` | Streaming active |
| `Connection lost` | Stream disconnected |
| `Failed to connect` | Network/server issue |
| `RTMP connect failed` | Authentication failed |

## Performance Guidelines

### T31X (128MB RAM)
- Maximum 3-4 simultaneous streams
- Bitrate limit: 4000 kbps total
- Monitor memory usage

### Network Requirements
- Upload bandwidth > total bitrate × 1.2
- Stable connection (wired preferred)
- Low latency to streaming servers

### Recommended Settings

| Platform | Bitrate | FPS | Resolution |
|----------|---------|-----|------------|
| YouTube | 1000-4000 kbps | 30 | 1080p/720p |
| Twitch | 2500-6000 kbps | 30/60 | 1080p |
| Facebook | 2000-4000 kbps | 30 | 720p/1080p |
| TikTok | 1000-3000 kbps | 30 | 720p |

## API Endpoints

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/api/modules/rtmp_client/status` | GET | Get status |
| `/api/modules/rtmp_client/enable` | POST | Enable client |
| `/api/modules/rtmp_client/disable` | POST | Disable client |
| `/api/modules/rtmp_client/streams/{name}/enable` | POST | Enable stream |
| `/api/modules/rtmp_client/streams/{name}/disable` | POST | Disable stream |
| `/api/modules/rtmp_client/config` | GET/PUT | Get/update config |

## Security Notes

- Keep stream keys private
- Use RTMPS when available
- Restrict config file permissions: `chmod 600 /etc/streamer.d/rtmp_client.json`
- Monitor for unauthorized access
- Regenerate keys if compromised

## Getting Stream Keys

### YouTube
1. Go to YouTube Studio
2. Click "Go Live" → "Stream"
3. Copy "Stream Key"

### Twitch
1. Go to Twitch Dashboard
2. Settings → Stream
3. Copy "Primary Stream Key"

### Facebook
1. Go to Creator Studio
2. Live → Use Stream Key
3. Copy stream key

### TikTok
1. Go to TikTok Live Studio
2. Get stream key (requires 1000+ followers)

For detailed information, see the full [RTMP Client Guide](RTMP_CLIENT.md).
