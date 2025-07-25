# RTMP Server Quick Start Guide

## 🚀 Get Started in 5 Minutes

This guide will get you streaming to your Thingino camera via RTMP in just a few minutes.

## Prerequisites

- Thingino camera with RTMP module enabled
- Computer with OBS Studio or FFmpeg installed
- Network connection between camera and computer

## Step 1: Verify RTMP Server

### Check if RTMP is Running

SSH into your camera and run:

```bash
# Check if streamer is running
ps aux | grep streamer

# Check if RTMP port is listening
netstat -ln | grep :1935

# Should show: tcp 0 0 0.0.0.0:1935 0.0.0.0:* LISTEN
```

### Start Streamer (if not running)

```bash
# Start the streamer
/usr/bin/streamer

# Or restart if already running
killall streamer && /usr/bin/streamer
```

### Check RTMP Configuration

```bash
# View RTMP config
cat /etc/streamer.d/rtmp.json

# Should show enabled: true, port: 1935
```

## Step 2: Test with OBS Studio

### Quick OBS Setup

1. **Open OBS Studio**

2. **Add a Video Source**:
   - Click "+" in Sources panel
   - Select "Display Capture" or "Video Capture Device"
   - Configure your video source

3. **Configure Streaming**:
   - Go to `Settings` → `Stream`
   - Service: `Custom...`
   - Server: `rtmp://YOUR_CAMERA_IP:1935/live`
   - Stream Key: `test` (or leave empty)
   - Click `OK`

4. **Start Streaming**:
   - Click `Start Streaming`
   - Watch for "Streaming" indicator

### Expected Results

You should see in the camera logs:

```
[RTMP] Client connected from YOUR_PC_IP:XXXXX
[RTMP] RTMP handshake completed successfully
[RTMP] Client connecting to app: live
[RTMP] Client publishing stream: test
[RTMP] Sent publish status: NetStream.Publish.Start
[RTMP] RTMP: Sent frame from channel 0 to 1 connections
```

## Step 3: Test with FFmpeg

### Simple Test Pattern

```bash
# Replace YOUR_CAMERA_IP with actual camera IP
ffmpeg -f lavfi -i testsrc=size=1280x720:rate=30 \
       -c:v libx264 -preset ultrafast -tune zerolatency \
       -f flv rtmp://YOUR_CAMERA_IP:1935/live/test
```

### Stream from Webcam

**Linux:**
```bash
ffmpeg -f v4l2 -i /dev/video0 \
       -c:v libx264 -preset ultrafast -tune zerolatency \
       -f flv rtmp://YOUR_CAMERA_IP:1935/live/webcam
```

**Windows:**
```bash
ffmpeg -f dshow -i video="USB Camera" \
       -c:v libx264 -preset ultrafast -tune zerolatency \
       -f flv rtmp://YOUR_CAMERA_IP:1935/live/webcam
```

**macOS:**
```bash
ffmpeg -f avfoundation -i "0" \
       -c:v libx264 -preset ultrafast -tune zerolatency \
       -f flv rtmp://YOUR_CAMERA_IP:1935/live/webcam
```

## Step 4: Monitor Streaming

### Real-time Logs

```bash
# Monitor RTMP activity
tail -f /var/log/streamer.log | grep RTMP

# Monitor all streamer activity
tail -f /var/log/streamer.log
```

### Connection Status

```bash
# Check active RTMP connections
netstat -an | grep :1935 | grep ESTABLISHED

# Check system resources
free -h
top -p $(pgrep streamer)
```

## Troubleshooting

### ❌ Connection Refused

**Problem:** `Connection to tcp://IP:1935 failed`

**Solutions:**
```bash
# Check if streamer is running
ps aux | grep streamer

# Check if port is open
netstat -ln | grep :1935

# Restart streamer
killall streamer && /usr/bin/streamer

# Check firewall (if applicable)
iptables -L | grep 1935
```

### ❌ Handshake Failed

**Problem:** Connection drops immediately

**Solutions:**
```bash
# Check RTMP URL format
# Correct: rtmp://192.168.1.100:1935/live/stream
# Wrong:   rtmp://192.168.1.100/live/stream

# Test basic connectivity
telnet YOUR_CAMERA_IP 1935

# Check camera logs for errors
tail -f /var/log/streamer.log | grep ERROR
```

### ❌ No Video Frames

**Problem:** Connection successful but no video data

**Solutions:**
```bash
# Check if video encoder is working
cat /proc/jz/encoder/encoder

# Verify channel configuration
cat /etc/streamer.json | grep -A 10 streams

# Check if RTSP is working (shares same encoder)
ffplay rtsp://YOUR_CAMERA_IP:554/ch0
```

### ❌ Poor Performance

**Problem:** Dropped frames or high CPU usage

**Solutions:**
```bash
# Reduce connections in config
echo '{"rtmp": {"max_connections": 2}}' > /etc/streamer.d/rtmp.json

# Check memory usage
free -h

# Monitor CPU usage
top -p $(pgrep streamer)

# Use lower quality settings in OBS/FFmpeg
# OBS: Settings → Output → Bitrate: 1000
# FFmpeg: Add -b:v 1000k
```

## Configuration Examples

### Basic Configuration

```json
{
  "rtmp": {
    "enabled": true,
    "port": 1935,
    "max_connections": 5,
    "auth_required": false,
    "app_name": "live"
  }
}
```

### Secure Configuration

```json
{
  "rtmp": {
    "enabled": true,
    "port": 1935,
    "max_connections": 2,
    "auth_required": true,
    "stream_key": "my_secret_key_123",
    "app_name": "live",
    "connection_timeout": 30
  }
}
```

### High Performance Configuration

```json
{
  "rtmp": {
    "enabled": true,
    "port": 1935,
    "max_connections": 10,
    "chunk_size": 8192,
    "auth_required": false,
    "app_name": "live"
  }
}
```

## Testing Checklist

- [ ] Camera is powered on and accessible via network
- [ ] Streamer process is running (`ps aux | grep streamer`)
- [ ] RTMP port 1935 is listening (`netstat -ln | grep :1935`)
- [ ] RTMP module is enabled in configuration
- [ ] OBS/FFmpeg can connect to RTMP URL
- [ ] Handshake completes successfully (check logs)
- [ ] Publish command is accepted
- [ ] Video frames are being transmitted
- [ ] No error messages in logs

## Success Indicators

### ✅ Successful Connection

```
[RTMP] RTMP server started on port 1935
[RTMP] Client connected from 192.168.1.100:54321
[RTMP] RTMP handshake completed successfully for fd 8
[RTMP] Handling RTMP command: connect (transaction_id=1)
[RTMP] Sent connect result (transaction_id=1)
[RTMP] Handling RTMP command: createStream (transaction_id=2)
[RTMP] Sent createStream result (transaction_id=2, stream_id=1)
[RTMP] Handling RTMP command: publish (transaction_id=0)
[RTMP] Client publishing stream: test
[RTMP] Sent publish status: NetStream.Publish.Start - Publishing stream
```

### ✅ Video Streaming

```
[RTMP] RTMP: Sent frame from channel 0 to 1 connections (1920 bytes)
[RTMP] RTMP: Sent frame from channel 0 to 1 connections (1856 bytes)
[RTMP] RTMP: Sent frame from channel 0 to 1 connections (1792 bytes)
```

### ✅ OBS Studio Status

- Green "Streaming" indicator in OBS
- Bitrate showing data transfer (e.g., "2000 kb/s")
- No dropped frames or minimal drops
- Stable connection time

### ✅ FFmpeg Output

```
frame= 1234 fps= 30 q=23.0 size=   15360kB time=00:00:41.13 bitrate=3059.2kbits/s speed=   1x
```

## Next Steps

Once basic streaming is working:

1. **Optimize Settings**: Adjust bitrate, resolution, and quality
2. **Add Authentication**: Enable stream key authentication
3. **Monitor Performance**: Set up logging and monitoring
4. **Integrate with Applications**: Use RTMP for live streaming platforms
5. **Explore Advanced Features**: Recording, transcoding, webhooks

## Support

For additional help:

- Check the full [RTMP Server Documentation](RTMP_SERVER.md)
- Review [Technical Implementation](TECHNICAL_IMPLEMENTATION.md)
- Search existing issues on GitHub
- Join the Thingino community forums

## Common RTMP URLs

Replace `YOUR_CAMERA_IP` with your camera's IP address:

- **Basic streaming**: `rtmp://YOUR_CAMERA_IP:1935/live/stream`
- **With stream key**: `rtmp://YOUR_CAMERA_IP:1935/live/YOUR_STREAM_KEY`
- **Custom app**: `rtmp://YOUR_CAMERA_IP:1935/myapp/stream`

## Performance Expectations

### Typical Performance

- **Connections**: 5-10 simultaneous streams
- **Latency**: 1-3 seconds end-to-end
- **CPU Usage**: 5-15% per stream
- **Memory Usage**: 50-100KB per connection
- **Bandwidth**: Matches encoder bitrate settings

### Optimal Settings

- **Resolution**: 1280x720 or 1920x1080
- **Framerate**: 30 fps
- **Bitrate**: 2000-4000 kbps
- **Codec**: H.264 (libx264)
- **Preset**: ultrafast or fast
- **Tune**: zerolatency

Happy streaming! 🎥📡
