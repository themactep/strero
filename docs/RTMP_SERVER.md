# RTMP Server Documentation

## Overview

The Thingino Streamer RTMP server is a complete implementation of the Real-Time Messaging Protocol (RTMP) that allows the camera to receive live video streams from broadcasting software like OBS Studio, FFmpeg, and other RTMP clients. This enables the camera to act as an RTMP ingest server for live streaming applications.

## Features

### Core RTMP Protocol
- **Complete RTMP handshake** - 3-way handshake (C0/S0, C1/S1, C2/S2)
- **Chunking protocol** - All 4 chunk types with header compression
- **AMF encoding/decoding** - Action Message Format for command messages
- **Command handling** - connect, createStream, publish, play, deleteStream
- **Response generation** - _result, _error, onStatus messages
- **Control messages** - chunk size, acknowledgement, window size

### Video Streaming
- **Real-time video streaming** - Direct integration with IMP encoder
- **H.264/H.265 codec support** - Works with both video codecs
- **Multi-connection support** - Up to 10 simultaneous RTMP connections
- **Frame distribution** - Efficient delivery to all connected clients
- **Timestamp synchronization** - Proper RTMP timestamp handling

### Integration
- **Modular architecture** - Follows existing module patterns
- **Configuration system** - JSON-based configuration
- **Build system integration** - Buildroot package support
- **Memory management** - Proper allocation and cleanup
- **Error handling** - Robust error recovery

## Configuration

### RTMP Module Configuration

The RTMP server is configured via `/etc/streamer.d/rtmp.json`:

```json
{
  "rtmp": {
    "enabled": true,
    "port": 1935,
    "max_connections": 10,
    "chunk_size": 4096,
    "auth_required": false,
    "stream_key": "",
    "app_name": "live",
    "connection_timeout": 30
  }
}
```

### Configuration Parameters

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `enabled` | boolean | `true` | Enable/disable RTMP server |
| `port` | integer | `1935` | RTMP server listening port |
| `max_connections` | integer | `10` | Maximum simultaneous connections |
| `chunk_size` | integer | `4096` | RTMP chunk size in bytes |
| `auth_required` | boolean | `false` | Require stream key authentication |
| `stream_key` | string | `""` | Required stream key (if auth enabled) |
| `app_name` | string | `"live"` | RTMP application name |
| `connection_timeout` | integer | `30` | Connection timeout in seconds |

### Build Configuration

The RTMP module is enabled by default in buildroot:

```
config BR2_PACKAGE_THINGINO_STREAMER_RTMP
    bool "RTMP streaming server module support"
    depends on BR2_PACKAGE_THINGINO_STREAMER
    default y
```

## Architecture

### Module Structure

```
src/modules/rtmp/
├── rtmp_module.c          # Main module implementation
├── rtmp_module.h          # Module header and structures
└── Makefile.rtmp         # Build configuration
```

### Key Components

1. **RTMP Server** - Main server accepting connections
2. **Connection Manager** - Handles multiple client connections
3. **Protocol Handler** - RTMP handshake and command processing
4. **Chunk Engine** - RTMP chunking protocol implementation
5. **AMF Codec** - Action Message Format encoding/decoding
6. **Video Streamer** - Integration with IMP encoder

### Data Flow

```
IMP Encoder → RTSP Callback → RTMP Module → RTMP Connections → RTMP Clients
     ↓              ↓              ↓              ↓              ↓
  H.264/H.265   Frame Poll    Frame Dist.   Chunk Protocol   OBS/FFmpeg
   Frames       (10ms poll)   (All conns)   (Video msgs)    (Receive)
```

## Testing with OBS Studio

### Setup OBS Studio

1. **Install OBS Studio** on your computer
2. **Add Video Source**:
   - Click "+" in Sources
   - Add "Video Capture Device" or "Display Capture"
   - Configure your video source

3. **Configure RTMP Output**:
   - Go to Settings → Stream
   - Service: "Custom..."
   - Server: `rtmp://CAMERA_IP:1935/live`
   - Stream Key: `test_stream` (or leave empty if auth disabled)

### Start Streaming

1. **Start the camera streamer**:
   ```bash
   /usr/bin/streamer
   ```

2. **Check RTMP server status**:
   ```bash
   # Check if RTMP port is listening
   netstat -ln | grep :1935
   
   # Check streamer logs
   tail -f /var/log/streamer.log
   ```

3. **Start streaming from OBS**:
   - Click "Start Streaming" in OBS
   - Monitor the camera logs for connection messages

### Expected Log Output

```
[RTMP] RTMP server started on port 1935
[RTMP] Client connected from 192.168.1.100:54321
[RTMP] Processing RTMP handshake for fd 8
[RTMP] RTMP handshake completed successfully for fd 8
[RTMP] Handling RTMP command: connect (transaction_id=1)
[RTMP] Client connecting to app: live
[RTMP] Sent connect result (transaction_id=1)
[RTMP] Handling RTMP command: createStream (transaction_id=2)
[RTMP] Sent createStream result (transaction_id=2, stream_id=1)
[RTMP] Handling RTMP command: publish (transaction_id=0)
[RTMP] Client publishing stream: test_stream
[RTMP] Sent publish status: NetStream.Publish.Start - Publishing stream
[RTMP] RTMP: Sent frame from channel 0 to 1 connections (1920 bytes)
```

## Testing with FFmpeg

### Basic Streaming Test

```bash
# Stream a test pattern to the camera
ffmpeg -f lavfi -i testsrc=size=1280x720:rate=30 \
       -c:v libx264 -preset ultrafast -tune zerolatency \
       -f flv rtmp://CAMERA_IP:1935/live/test_stream
```

### Stream from File

```bash
# Stream a video file to the camera
ffmpeg -re -i input_video.mp4 \
       -c:v libx264 -preset ultrafast \
       -f flv rtmp://CAMERA_IP:1935/live/my_stream
```

### Stream from Webcam

```bash
# Stream from webcam (Linux)
ffmpeg -f v4l2 -i /dev/video0 \
       -c:v libx264 -preset ultrafast -tune zerolatency \
       -f flv rtmp://CAMERA_IP:1935/live/webcam_stream

# Stream from webcam (Windows)
ffmpeg -f dshow -i video="USB Camera" \
       -c:v libx264 -preset ultrafast -tune zerolatency \
       -f flv rtmp://CAMERA_IP:1935/live/webcam_stream
```

### Advanced Options

```bash
# High quality stream with audio
ffmpeg -f v4l2 -i /dev/video0 -f alsa -i default \
       -c:v libx264 -preset medium -crf 23 \
       -c:a aac -b:a 128k \
       -f flv rtmp://CAMERA_IP:1935/live/hq_stream

# Low latency stream
ffmpeg -f v4l2 -i /dev/video0 \
       -c:v libx264 -preset ultrafast -tune zerolatency \
       -g 30 -keyint_min 30 -sc_threshold 0 \
       -b:v 2000k -maxrate 2000k -bufsize 1000k \
       -f flv rtmp://CAMERA_IP:1935/live/lowlatency_stream
```

## Troubleshooting

### Common Issues

1. **Connection Refused**
   ```
   Error: Connection refused
   ```
   - Check if RTMP server is running: `netstat -ln | grep :1935`
   - Verify RTMP module is enabled in configuration
   - Check firewall settings

2. **Handshake Failed**
   ```
   [RTMP] Failed to read C0 version byte
   ```
   - Check network connectivity
   - Verify RTMP URL format: `rtmp://IP:1935/app/stream`
   - Try different RTMP client

3. **Authentication Failed**
   ```
   [RTMP] Stream key authentication failed
   ```
   - Check stream key in configuration
   - Disable authentication: `"auth_required": false`
   - Verify stream key matches configuration

4. **No Video Data**
   ```
   [RTMP] No publishing connections found
   ```
   - Ensure publish command was successful
   - Check video encoder is running
   - Verify channel configuration

### Debug Commands

```bash
# Check RTMP server status
ps aux | grep streamer

# Monitor RTMP connections
netstat -an | grep :1935

# Check system resources
free -h
top -p $(pgrep streamer)

# Monitor logs in real-time
tail -f /var/log/streamer.log | grep RTMP

# Test RTMP connectivity
telnet CAMERA_IP 1935
```

### Log Levels

Set debug logging in `/etc/streamer.json`:

```json
{
  "general": {
    "loglevel": "DEBUG"
  }
}
```

Available log levels: `ERROR`, `WARN`, `INFO`, `DEBUG`

## Performance Considerations

### Memory Usage

- **Base RTMP module**: ~300KB
- **Per connection**: ~50KB
- **Frame buffers**: ~4KB per frame per connection

### CPU Usage

- **Handshake**: Minimal CPU impact
- **Frame processing**: ~2-5% CPU per connection
- **AMF encoding**: Negligible impact

### Network Bandwidth

- **Control messages**: <1KB/s per connection
- **Video data**: Depends on encoder settings
  - 1080p H.264: ~2-8 Mbps
  - 720p H.264: ~1-4 Mbps
  - 480p H.264: ~0.5-2 Mbps

### Optimization Tips

1. **Reduce connections**: Limit `max_connections` for better performance
2. **Adjust chunk size**: Larger chunks reduce overhead
3. **Monitor memory**: Use `free -h` to check available memory
4. **Network tuning**: Ensure stable network connection

## Security Considerations

### Authentication

Enable stream key authentication:

```json
{
  "rtmp": {
    "auth_required": true,
    "stream_key": "your_secret_key_here"
  }
}
```

### Network Security

- **Firewall**: Restrict RTMP port (1935) access
- **VPN**: Use VPN for remote access
- **SSL/TLS**: Consider RTMPS for encrypted connections (future enhancement)

### Access Control

- **IP filtering**: Implement IP-based access control
- **Rate limiting**: Prevent connection flooding
- **Monitoring**: Log all connection attempts

## API Integration

### Status Endpoints

The RTMP module integrates with the HTTP API:

```bash
# Get RTMP server status
curl http://CAMERA_IP/api/rtmp/status

# Get active connections
curl http://CAMERA_IP/api/rtmp/connections

# Get streaming statistics
curl http://CAMERA_IP/api/rtmp/stats
```

### Configuration API

```bash
# Get RTMP configuration
curl http://CAMERA_IP/api/config/rtmp

# Update RTMP configuration
curl -X POST http://CAMERA_IP/api/config/rtmp \
     -H "Content-Type: application/json" \
     -d '{"enabled": true, "port": 1935}'
```

## Future Enhancements

### Planned Features

1. **RTMPS Support** - SSL/TLS encryption
2. **Audio Streaming** - Audio input support
3. **Recording** - Save incoming streams to disk
4. **Transcoding** - Multiple output formats
5. **Authentication** - Advanced user management
6. **Statistics** - Detailed streaming metrics
7. **Webhooks** - Stream start/stop notifications

### Contributing

To contribute to the RTMP server:

1. Follow the existing code style
2. Add comprehensive tests
3. Update documentation
4. Submit pull requests with detailed descriptions

## References

- [RTMP Specification](https://rtmp.veriskope.com/docs/spec/)
- [AMF0 Specification](https://rtmp.veriskope.com/pdf/amf0-file-format-specification.pdf)
- [OBS Studio Documentation](https://obsproject.com/wiki/)
- [FFmpeg RTMP Documentation](https://ffmpeg.org/ffmpeg-protocols.html#rtmp)
