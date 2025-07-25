# Thingino Streamer HTTP API

## Overview

Thingino Streamer includes a comprehensive HTTP API server that provides multiple endpoints for monitoring, configuration, and media access. The HTTP server runs on port 8080 and offers real-time video streaming, system metrics, health checks, and more.

## Available Endpoints

### Monitoring Endpoints

#### Prometheus Metrics
**URL:** `http://<camera-ip>:8080/metrics`

Serves metrics in Prometheus format for Grafana or any Prometheus-compatible monitoring system.

```
{
  "enabled": "Enable/disable system metrics collection",
  "collection_interval_ms": "Interval between metrics collection in milliseconds",
  "history_size": "Number of historical metric samples to keep in memory",
  "collect": {
    "process": "Collect process metrics (CPU, memory, threads)",
    "streams": "Collect stream metrics (FPS, frames, errors, clients)",
    "clients": "Collect client connection metrics",
    "system": "Collect system-wide metrics (uptime, load, memory)",
    "network": "Collect network statistics (bytes, packets)"
  },
  "exporters": {
    "prometheus": "Enable Prometheus metrics format export",
    "json_status": "Enable JSON status endpoint",
    "json_health": "Enable JSON health check endpoint",
    "json_info": "Enable JSON system info endpoint"
  },
  "endpoints": {
    "metrics_path": "HTTP path for Prometheus metrics endpoint",
    "status_path": "HTTP path for JSON status endpoint",
    "health_path": "HTTP path for JSON health endpoint",
    "info_path": "HTTP path for JSON info endpoint",
    "config_path": "HTTP path for JSON config endpoint"
  },
  "enable_detailed_logging": "Enable detailed debug logging of metrics collection",
  "max_clients_per_endpoint": "Maximum concurrent clients per HTTP endpoint"
}
```

#### Health Check
**URL:** `http://<camera-ip>:8080/health`

Simple health check endpoint for load balancers and monitoring systems.

#### System Status
**URL:** `http://<camera-ip>:8080/status.json`

Detailed system status including real-time FPS, frame counts, and error statistics.

#### Configuration Info
**URL:** `http://<camera-ip>:8080/config.json`

Current streamer configuration and endpoint information.

#### Hardware Info
**URL:** `http://<camera-ip>:8080/info.json`

Hardware platform, sensor details, and software version information.

### Media Endpoints

#### JPEG Snapshots
- **`http://<camera-ip>:8080/snap0.jpg`** - Main stream snapshot (1920x1080)
- **`http://<camera-ip>:8080/snap1.jpg`** - Sub stream snapshot (640x360)

#### MJPEG Live Streaming
- **`http://<camera-ip>:8080/stream0.mjpeg`** - Main stream MJPEG (1920x1080 @ 10 FPS)
- **`http://<camera-ip>:8080/stream1.mjpeg`** - Sub stream MJPEG (640x360 @ 10 FPS)

#### H.264 Video Segments
- **`http://<camera-ip>:8080/stream0.h264`** - Main stream H.264 segment (5 seconds)
- **`http://<camera-ip>:8080/stream1.h264`** - Sub stream H.264 segment (5 seconds)
- **`http://<camera-ip>:8080/stream0.mp4`** - Alias for H.264 segment

*Note: H.264 segments are currently disabled to prevent interference with main video streams.*

## Prometheus Metrics Details

### Frame Rate (FPS)
```
thingino_streamer_fps{channel="0",stream="main"} 30.01
thingino_streamer_fps{channel="1",stream="sub"} 15.02
```

### Frame Count (Total frames processed)
```
thingino_streamer_frame_count{channel="0",stream="main"} 1081
thingino_streamer_frame_count{channel="1",stream="sub"} 541
```

### Error Count (Total errors encountered)
```
thingino_streamer_error_count{channel="0",stream="main"} 0
thingino_streamer_error_count{channel="1",stream="sub"} 0
```

### Frame Size (Average frame size in bytes)
```
thingino_streamer_frame_size_bytes{channel="0",stream="main"} 128718
thingino_streamer_frame_size_bytes{channel="1",stream="sub"} 61060
```

## Grafana Configuration

### 1. Add Prometheus Data Source

In Grafana, add a new Prometheus data source:
- **URL:** `http://<camera-ip>:8080`
- **Scrape interval:** 5s (recommended)
- **HTTP Method:** GET

### 2. Create Dashboard

Import or create a dashboard with the following queries:

#### Frame Rate Panel
```promql
thingino_streamer_fps
```

#### Frame Count Panel
```promql
rate(thingino_streamer_frame_count[1m]) * 60
```

#### Error Rate Panel
```promql
rate(thingino_streamer_error_count[5m]) * 300
```

#### Frame Size Panel
```promql
thingino_streamer_frame_size_bytes
```

### 3. Sample Dashboard JSON

```json
{
  "dashboard": {
    "title": "Thingino Streamer Metrics",
    "panels": [
      {
        "title": "Frame Rate (FPS)",
        "type": "stat",
        "targets": [
          {
            "expr": "thingino_streamer_fps",
            "legendFormat": "{{stream}} (ch{{channel}})"
          }
        ]
      },
      {
        "title": "Frame Size",
        "type": "timeseries",
        "targets": [
          {
            "expr": "thingino_streamer_frame_size_bytes",
            "legendFormat": "{{stream}} (ch{{channel}})"
          }
        ]
      }
    ]
  }
}
```

## Testing the Endpoint

### Using curl
```bash
curl http://<camera-ip>:8080/metrics
```

### Using wget
```bash
wget -qO- http://<camera-ip>:8080/metrics
```

### Expected Output
```
# HELP thingino_streamer_fps Frames per second for each channel
# TYPE thingino_streamer_fps gauge
thingino_streamer_fps{channel="0",stream="main"} 30.01 1703123456
thingino_streamer_fps{channel="1",stream="sub"} 15.02 1703123456

# HELP thingino_streamer_frame_count Total frames processed
# TYPE thingino_streamer_frame_count counter
thingino_streamer_frame_count{channel="0",stream="main"} 1081 1703123456
thingino_streamer_frame_count{channel="1",stream="sub"} 541 1703123456

# HELP thingino_streamer_error_count Total errors encountered
# TYPE thingino_streamer_error_count counter
thingino_streamer_error_count{channel="0",stream="main"} 0 1703123456
thingino_streamer_error_count{channel="1",stream="sub"} 0 1703123456

# HELP thingino_streamer_frame_size_bytes Average frame size in bytes
# TYPE thingino_streamer_frame_size_bytes gauge
thingino_streamer_frame_size_bytes{channel="0",stream="main"} 128718 1703123456
thingino_streamer_frame_size_bytes{channel="1",stream="sub"} 61060 1703123456
```

## Usage Examples

### Health Check
```bash
# Simple health check
curl http://192.168.1.100:8080/health

# Expected response:
{
  "status": "healthy",
  "timestamp": 1751908224,
  "uptime_seconds": 1234,
  "services": {
    "main_stream": {
      "status": "healthy",
      "fps": 30.02
    },
    "sub_stream": {
      "status": "healthy",
      "fps": 15.01
    },
    "http_api": {
      "status": "healthy",
      "port": 8080
    }
  },
  "errors": {
    "main_stream_errors": 0,
    "sub_stream_errors": 0
  }
}
```

### System Status
```bash
# Get detailed system status
curl http://192.168.1.100:8080/status.json

# Expected response includes FPS, frame counts, error statistics
```

### Configuration Info
```bash
# Get current configuration
curl http://192.168.1.100:8080/config.json

# Expected response includes stream settings and endpoint info
```

### Hardware Info
```bash
# Get hardware and software information
curl http://192.168.1.100:8080/info.json

# Expected response includes platform, sensor, version details
```

### JPEG Snapshots
```bash
# Capture main stream snapshot
curl http://192.168.1.100:8080/snap0.jpg -o snapshot_main.jpg

# Capture sub stream snapshot
curl http://192.168.1.100:8080/snap1.jpg -o snapshot_sub.jpg
```

### MJPEG Live Streaming
```bash
# Stream main camera in browser
http://192.168.1.100:8080/stream0.mjpeg

# Stream sub camera in browser
http://192.168.1.100:8080/stream1.mjpeg

# Save MJPEG stream to file
curl http://192.168.1.100:8080/stream0.mjpeg -o stream.mjpeg
```

### H.264 Video Segments
```bash
# Download 5-second H.264 segment (when enabled)
curl http://192.168.1.100:8080/stream0.h264 -o segment.h264

# Alternative MP4 endpoint
curl http://192.168.1.100:8080/stream0.mp4 -o segment.h264
```

### Prometheus Metrics
```bash
# Get Prometheus metrics
curl http://192.168.1.100:8080/metrics
```

## Troubleshooting

### Port 8080 Not Accessible
- Check firewall settings
- Verify the camera is running Thingino Streamer with HTTP API enabled
- Check if another service is using port 8080

### No Metrics Data
- Ensure video streaming is active
- Check Thingino Streamer logs for HTTP server startup messages
- Verify the HTTP server thread is running

### MJPEG Streaming Issues
- Check if video channels are enabled and working
- Verify frame rate is sufficient (>5 FPS)
- Test with different browsers or media players

### H.264 Segment Issues
- H.264 segments are currently disabled to prevent main stream interference
- Use MJPEG streaming or RTSP for live video access
- Check logs for encoder conflicts if re-enabling

### Grafana Connection Issues
- Test the endpoint manually with curl first
- Check Grafana data source configuration
- Verify network connectivity between Grafana and camera

## Performance Impact

The HTTP API system is designed to be lightweight:
- **Memory usage:** ~8KB for HTTP server and metrics storage
- **CPU impact:** Minimal (metrics updated only during frame processing)
- **Network overhead:** Only when endpoints are accessed
- **No external dependencies:** Pure socket-based HTTP server
- **MJPEG streaming:** Uses existing JPEG encoder, minimal additional overhead
- **Snapshots:** On-demand capture, no continuous processing

## Integration Notes

- Metrics are updated in real-time as frames are processed
- The HTTP server runs in a separate thread
- All endpoints persist for the lifetime of the Thingino Streamer process
- No authentication required (suitable for internal networks)
- Compatible with any Prometheus-compatible monitoring system
- MJPEG streams work directly in web browsers
- Health check endpoint suitable for load balancer probes

## Security Considerations

- All endpoints are read-only except for media streaming
- No authentication is required (suitable for internal networks)
- Consider firewall rules to restrict access if needed
- Endpoints contain system information and media access
- Health check endpoint is safe for public load balancer access
