RTSP Streaming Targets
======================

This document outlines the recommended streaming settings for various RTMP targets.

Facebook
--------

```
      {
        "name": "facebook",
        "enabled": false,
        "url": "rtmp://live-api-s.facebook.com:80/rtmp",
        "stream_key": "YOUR_FACEBOOK_STREAM_KEY",
        "retry_interval": 30,
        "max_retries": 3,
        "connection_timeout": 30
      }
```

Your server URL and stream key can only be used for a single live stream preview or live video post connection.
You must use the same server URL or stream key to preview and post; if you preview and then stop the stream, you won't not be able to resume later.
The server URL and stream key are valid for as long as you are logged into Facebook.
Once you preview the stream, you have up to 5 hours to go live.
If you need more time, create a new server URL or stream key closer to the event time.

With persistent stream key (PSK), you can reuse the same stream key across multiple live broadcasts.

**Video Options:**

- Protocol: RTMPS Streaming
- Video codec:
  - H.264, Level 4.1 for up to 1080p @ 30 FPS
  - H.264, Level 4.2 for 1080p @ 60 FPS
  - Live API: H264 encoded video and AAC encoded audio only. Other formats may be rejected by the Facebook Live platform.
- Resolution, frame rate, and bitrate:
  - 1080p (1920 x 1080) @ 60 fps, 4500-9000 Kbps
  - 1080p (1920 x 1080) @ 30 fps, 3000-6000 Kbps
  - 720p (1280 x 720) @ 60 fps, 2250-6000 Kbps
  - 720p (1280 x 720) @ 30 fps, 1500-4000 Kbps
  - 480p (854 x 480) @ 30 fps, 600-2000 Kbps
  - 360p (640 x 360) @ 30 fps, 400-1000 Kbps
- Keyframe: 2 seconds (recommended) to 4 seconds (max)
- Frame types: Progressive Scan
- Bitrate encoding: CBR
- Aspect ratio: 16:9
- Video length: 8-hour time limit

**Audio Options:**

- Sample rate: 44.1 kHz or 48 kHz
- Channel layout: Stereo
- Codec: AAC-LC
- Bitrate: 128 Kbps (preferred) to 256 Kbps (max)

Instagram
---------

```
      {
        "name": "instagram",
        "enabled": false,
        "url": "rtmp://rtmp-api.instagram.com:80/rtmp",
        "stream_key": "YOUR_INSTAGRAM_STREAM_KEY",
        "retry_interval": 30,
        "max_retries": 3,
        "connection_timeout": 30
      },
```

**Video Options:**

- Resolution: 1080p (1920 x 1080)
- Frames per second: 30 fps
- Bitrate range: 3,000-6,000 Kbps
- Keyframe: 2 seconds
- Video codec: H.264, Level 4.1
- 8-hour time limit

**Audio Options:**

- Sample rate: 44.1 kHz
- Channel layout: Stereo
- Bitrate: 128 Kbps

Periscope
---------

```
      {
        "name": "periscope",
        "enabled": false,
        "url": "rtmp://rtmp.pscp.tv:80/x",
        "stream_key": "YOUR_PERISCOPE_STREAM_KEY",
        "retry_interval": 30,
        "max_retries": 3,
        "connection_timeout": 30
      }
```

Telegram
--------

```
      {
        "name": "telegram",
        "enabled": true,
        "url": "rtmps://dc1-1.rtmp.t.me/s/",
        "stream_key": "YOUR_TELEGRAM_STREAM_KEY",
        "retry_interval": 30,
        "max_retries": 3,
        "connection_timeout": 30
      }
```

1. Click on streaming icon in your Telegram channel and select "Stream with..."
2. Copy **Telegram Server URL** and **Stream Key**.
   Don't close these settings, as you'd need to access them again to click "Start Streaming" after you set up your stream.
3. Add the Telegram server URL and stream key to the `rtmp_client.json` config file.
4. Set `"enabled": true` for the telegram client in the `rtmp_client.json` config file.
5. Restart the streamer service: `service restart streamer`
6. Go back to Telegram and click "Start Streaming".
7. Verify that the stream is working by checking the Telegram channel.
8. When you want to end the stream, you'll have to click "End live stream" button in the Telegram channel.


TikTok
------

```
      {
        "name": "tiktok",
        "enabled": false,
        "url": "rtmp://rtmp-push.tiktok.com/live",
        "stream_key": "YOUR_TIKTOK_STREAM_KEY",
        "retry_interval": 30,
        "max_retries": 3,
        "connection_timeout": 30
      }
```

Twitch
------

```
      {
        "name": "twitch",
        "enabled": false,
        "url": "rtmp://live.twitch.tv/live",
        "stream_key": "YOUR_TWITCH_STREAM_KEY",
        "retry_interval": 30,
        "max_retries": 5,
        "connection_timeout": 30
      }
```

Twitter
-------

```
      {
        "name": "twitter",
        "enabled": false,
        "url": "rtmp://broadcasting-api.twitter.com/live",
        "stream_key": "YOUR_TWITTER_STREAM_KEY",
        "retry_interval": 30,
        "max_retries": 3,
        "connection_timeout": 30
      }
```

YouTube
-------

```
      {
        "name": "youtube",
        "enabled": false,
        "url": "rtmp://a.rtmp.youtube.com/live2",
        "stream_key": "YOUR_YOUTUBE_STREAM_KEY",
        "retry_interval": 30,
        "max_retries": 5,
        "connection_timeout": 30
      }
```
