# RTMP Server Testing and Validation Guide

## Overview

This guide provides comprehensive testing procedures for the RTMP server implementation, covering functional testing, performance validation, and compatibility verification.

## Test Environment Setup

### Prerequisites

- Thingino camera with RTMP module compiled and installed
- Test computer with network access to camera
- OBS Studio (latest version)
- FFmpeg (4.0 or later)
- Network monitoring tools (Wireshark, tcpdump)
- Performance monitoring tools (htop, iotop)

### Network Configuration

```bash
# Ensure camera and test computer are on same network
ping CAMERA_IP

# Verify RTMP port accessibility
telnet CAMERA_IP 1935

# Check for firewall issues
nmap -p 1935 CAMERA_IP
```

## Functional Testing

### 1. Basic Connection Tests

#### Test 1.1: RTMP Server Startup
```bash
# Start streamer and verify RTMP server
/usr/bin/streamer &
sleep 2
netstat -ln | grep :1935
# Expected: tcp 0 0 0.0.0.0:1935 0.0.0.0:* LISTEN
```

#### Test 1.2: Basic Connection
```bash
# Test basic TCP connection
telnet CAMERA_IP 1935
# Expected: Connection established, then timeout after 30 seconds
```

#### Test 1.3: RTMP Handshake
```bash
# Use FFmpeg to test handshake
timeout 10 ffmpeg -f lavfi -i testsrc=size=320x240:rate=1 \
                  -c:v libx264 -preset ultrafast \
                  -f flv rtmp://CAMERA_IP:1935/live/test
# Expected: Connection successful, handshake complete
```

### 2. Protocol Compliance Tests

#### Test 2.1: RTMP Version Validation
```bash
# Test with different RTMP versions (should accept version 3)
echo -ne '\x03' | nc CAMERA_IP 1935
# Expected: Server responds with version 3
```

#### Test 2.2: Handshake Sequence
```bash
# Monitor handshake with tcpdump
tcpdump -i any -s 0 -X host CAMERA_IP and port 1935 &
ffmpeg -f lavfi -i testsrc=size=320x240:rate=1 \
       -c:v libx264 -preset ultrafast -t 5 \
       -f flv rtmp://CAMERA_IP:1935/live/handshake_test
# Expected: C0/S0, C1/S1, C2/S2 sequence visible in capture
```

#### Test 2.3: Command Processing
```bash
# Test connect command
ffmpeg -f lavfi -i testsrc=size=320x240:rate=1 \
       -c:v libx264 -preset ultrafast -t 5 \
       -f flv rtmp://CAMERA_IP:1935/live/connect_test
# Expected: connect → _result, createStream → _result, publish → onStatus
```

### 3. Video Streaming Tests

#### Test 3.1: Basic Video Streaming
```bash
# Stream test pattern
ffmpeg -f lavfi -i testsrc=size=1280x720:rate=30 \
       -c:v libx264 -preset ultrafast -tune zerolatency \
       -t 30 -f flv rtmp://CAMERA_IP:1935/live/video_test
# Expected: 30 seconds of streaming, no errors
```

#### Test 3.2: Different Resolutions
```bash
# Test various resolutions
for res in "320x240" "640x480" "1280x720" "1920x1080"; do
    echo "Testing resolution: $res"
    timeout 10 ffmpeg -f lavfi -i testsrc=size=$res:rate=30 \
                      -c:v libx264 -preset ultrafast \
                      -f flv rtmp://CAMERA_IP:1935/live/res_$res
done
# Expected: All resolutions accepted and processed
```

#### Test 3.3: Different Frame Rates
```bash
# Test various frame rates
for fps in "15" "30" "60"; do
    echo "Testing frame rate: $fps fps"
    timeout 10 ffmpeg -f lavfi -i testsrc=size=1280x720:rate=$fps \
                      -c:v libx264 -preset ultrafast \
                      -f flv rtmp://CAMERA_IP:1935/live/fps_$fps
done
# Expected: All frame rates handled correctly
```

### 4. Multi-Client Tests

#### Test 4.1: Concurrent Connections
```bash
# Start multiple streams simultaneously
for i in {1..5}; do
    timeout 30 ffmpeg -f lavfi -i testsrc=size=640x480:rate=30 \
                      -c:v libx264 -preset ultrafast \
                      -f flv rtmp://CAMERA_IP:1935/live/client_$i &
done
wait
# Expected: All 5 connections accepted and handled
```

#### Test 4.2: Connection Limits
```bash
# Test maximum connection limit (default: 10)
for i in {1..12}; do
    timeout 60 ffmpeg -f lavfi -i testsrc=size=320x240:rate=15 \
                      -c:v libx264 -preset ultrafast \
                      -f flv rtmp://CAMERA_IP:1935/live/limit_$i &
done
wait
# Expected: First 10 connections accepted, 11th and 12th rejected
```

### 5. Error Handling Tests

#### Test 5.1: Invalid RTMP Version
```bash
# Send invalid version
echo -ne '\x04' | nc CAMERA_IP 1935
# Expected: Connection rejected or closed
```

#### Test 5.2: Malformed Handshake
```bash
# Send incomplete handshake
echo -ne '\x03\x00\x00\x00' | nc CAMERA_IP 1935
# Expected: Connection timeout or graceful close
```

#### Test 5.3: Invalid Commands
```bash
# Test with malformed RTMP commands
timeout 10 ffmpeg -f lavfi -i testsrc=size=320x240:rate=1 \
                  -c:v libx264 -preset ultrafast \
                  -f flv rtmp://CAMERA_IP:1935/invalid_app/test
# Expected: Connection rejected or error response
```

## Performance Testing

### 1. Throughput Tests

#### Test 1.1: Maximum Bitrate
```bash
# Test high bitrate streaming
ffmpeg -f lavfi -i testsrc=size=1920x1080:rate=30 \
       -c:v libx264 -preset ultrafast -b:v 10M \
       -t 60 -f flv rtmp://CAMERA_IP:1935/live/high_bitrate
# Monitor: CPU usage, memory usage, network utilization
```

#### Test 1.2: Sustained Streaming
```bash
# Long duration test
ffmpeg -f lavfi -i testsrc=size=1280x720:rate=30 \
       -c:v libx264 -preset ultrafast -b:v 2M \
       -t 3600 -f flv rtmp://CAMERA_IP:1935/live/sustained
# Monitor: Memory leaks, performance degradation over time
```

### 2. Resource Monitoring

#### Test 2.1: Memory Usage
```bash
# Monitor memory during streaming
while true; do
    ps aux | grep streamer | grep -v grep
    free -h
    sleep 5
done &

# Start streaming test
ffmpeg -f lavfi -i testsrc=size=1280x720:rate=30 \
       -c:v libx264 -preset ultrafast \
       -t 300 -f flv rtmp://CAMERA_IP:1935/live/memory_test
# Expected: Stable memory usage, no significant leaks
```

#### Test 2.2: CPU Usage
```bash
# Monitor CPU during multiple streams
top -p $(pgrep streamer) &

# Start multiple concurrent streams
for i in {1..5}; do
    timeout 120 ffmpeg -f lavfi -i testsrc=size=640x480:rate=30 \
                       -c:v libx264 -preset ultrafast \
                       -f flv rtmp://CAMERA_IP:1935/live/cpu_$i &
done
wait
# Expected: CPU usage scales reasonably with connection count
```

### 3. Network Performance

#### Test 3.1: Bandwidth Utilization
```bash
# Monitor network traffic
iftop -i eth0 &

# Stream high bitrate content
ffmpeg -f lavfi -i testsrc=size=1920x1080:rate=30 \
       -c:v libx264 -preset ultrafast -b:v 5M \
       -t 180 -f flv rtmp://CAMERA_IP:1935/live/bandwidth_test
# Expected: Network usage matches configured bitrate
```

#### Test 3.2: Packet Loss Handling
```bash
# Simulate packet loss with tc (traffic control)
tc qdisc add dev eth0 root netem loss 1%

# Test streaming with packet loss
ffmpeg -f lavfi -i testsrc=size=1280x720:rate=30 \
       -c:v libx264 -preset ultrafast \
       -t 120 -f flv rtmp://CAMERA_IP:1935/live/packet_loss

# Clean up
tc qdisc del dev eth0 root
# Expected: Graceful handling of packet loss
```

## Compatibility Testing

### 1. OBS Studio Compatibility

#### Test 1.1: Basic OBS Streaming
1. Configure OBS with camera RTMP URL
2. Add video source (screen capture or webcam)
3. Start streaming for 5 minutes
4. Verify stable connection and video quality

#### Test 1.2: OBS Advanced Features
1. Test different encoder settings (x264, NVENC)
2. Test various bitrate configurations
3. Test stream interruption and reconnection
4. Verify OBS statistics show stable streaming

### 2. FFmpeg Compatibility

#### Test 2.1: Different Input Sources
```bash
# Test various input formats
ffmpeg -f v4l2 -i /dev/video0 \
       -c:v libx264 -preset ultrafast \
       -f flv rtmp://CAMERA_IP:1935/live/webcam

ffmpeg -i input.mp4 \
       -c:v libx264 -preset ultrafast \
       -f flv rtmp://CAMERA_IP:1935/live/file

ffmpeg -f x11grab -i :0.0 \
       -c:v libx264 -preset ultrafast \
       -f flv rtmp://CAMERA_IP:1935/live/screen
```

#### Test 2.2: Codec Compatibility
```bash
# Test different video codecs
ffmpeg -f lavfi -i testsrc=size=1280x720:rate=30 \
       -c:v libx264 -preset ultrafast \
       -f flv rtmp://CAMERA_IP:1935/live/h264

ffmpeg -f lavfi -i testsrc=size=1280x720:rate=30 \
       -c:v libx265 -preset ultrafast \
       -f flv rtmp://CAMERA_IP:1935/live/h265
```

### 3. Third-Party Software

#### Test 3.1: Streaming Software
- XSplit Broadcaster
- Streamlabs OBS
- Wirecast
- vMix

#### Test 3.2: Mobile Apps
- Larix Broadcaster (iOS/Android)
- CameraFi Live (Android)
- Broadcast Me (iOS)

## Automated Testing

### 1. Test Scripts

#### Basic Functionality Test
```bash
#!/bin/bash
# rtmp_basic_test.sh

CAMERA_IP="192.168.1.100"
TEST_DURATION=30

echo "Starting RTMP basic functionality test..."

# Test 1: Connection test
echo "Test 1: Basic connection"
timeout 5 telnet $CAMERA_IP 1935 && echo "PASS" || echo "FAIL"

# Test 2: Handshake test
echo "Test 2: RTMP handshake"
timeout 10 ffmpeg -f lavfi -i testsrc=size=320x240:rate=1 \
                  -c:v libx264 -preset ultrafast -t 5 \
                  -f flv rtmp://$CAMERA_IP:1935/live/test \
                  -y /dev/null 2>&1 && echo "PASS" || echo "FAIL"

# Test 3: Video streaming
echo "Test 3: Video streaming"
timeout $TEST_DURATION ffmpeg -f lavfi -i testsrc=size=640x480:rate=30 \
                              -c:v libx264 -preset ultrafast \
                              -f flv rtmp://$CAMERA_IP:1935/live/video \
                              -y /dev/null 2>&1 && echo "PASS" || echo "FAIL"

echo "Basic functionality test completed."
```

#### Performance Test
```bash
#!/bin/bash
# rtmp_performance_test.sh

CAMERA_IP="192.168.1.100"
MAX_CLIENTS=5

echo "Starting RTMP performance test..."

# Monitor system resources
top -b -n 1 -p $(pgrep streamer) > performance_before.txt

# Start multiple concurrent streams
for i in $(seq 1 $MAX_CLIENTS); do
    timeout 60 ffmpeg -f lavfi -i testsrc=size=640x480:rate=30 \
                      -c:v libx264 -preset ultrafast \
                      -f flv rtmp://$CAMERA_IP:1935/live/perf_$i \
                      -y /dev/null 2>&1 &
    echo "Started client $i"
    sleep 2
done

# Wait for all streams to complete
wait

# Check final system resources
top -b -n 1 -p $(pgrep streamer) > performance_after.txt

echo "Performance test completed."
echo "Check performance_before.txt and performance_after.txt for resource usage."
```

### 2. Continuous Integration

#### GitHub Actions Workflow
```yaml
name: RTMP Server Tests

on: [push, pull_request]

jobs:
  rtmp-tests:
    runs-on: ubuntu-latest
    steps:
    - uses: actions/checkout@v2
    
    - name: Build RTMP module
      run: |
        ./build.sh
    
    - name: Start test environment
      run: |
        # Start camera emulator or test environment
        ./tests/start_test_env.sh
    
    - name: Run RTMP tests
      run: |
        ./tests/rtmp_basic_test.sh
        ./tests/rtmp_performance_test.sh
    
    - name: Collect test results
      run: |
        ./tests/collect_results.sh
```

## Test Results Documentation

### 1. Test Report Template

```
RTMP Server Test Report
Date: [DATE]
Version: [VERSION]
Tester: [NAME]

Environment:
- Camera Model: [MODEL]
- Firmware Version: [VERSION]
- Network: [NETWORK_INFO]
- Test Computer: [SPECS]

Test Results:
┌─────────────────────────┬────────┬─────────────────┐
│ Test Case               │ Result │ Notes           │
├─────────────────────────┼────────┼─────────────────┤
│ Basic Connection        │ PASS   │                 │
│ RTMP Handshake         │ PASS   │                 │
│ Video Streaming        │ PASS   │                 │
│ Multi-Client (5)       │ PASS   │                 │
│ Connection Limits      │ PASS   │ Max 10 clients  │
│ OBS Compatibility     │ PASS   │                 │
│ FFmpeg Compatibility  │ PASS   │                 │
│ Performance (60min)    │ PASS   │ Stable memory   │
└─────────────────────────┴────────┴─────────────────┘

Performance Metrics:
- Memory Usage: [BASELINE] → [PEAK] MB
- CPU Usage: [AVERAGE]% (peak [PEAK]%)
- Network Throughput: [MBPS] Mbps
- Connection Latency: [MS] ms

Issues Found:
[LIST ANY ISSUES]

Recommendations:
[LIST RECOMMENDATIONS]
```

### 2. Regression Testing

Maintain a test suite that runs automatically on code changes:

```bash
#!/bin/bash
# regression_test.sh

echo "Running RTMP regression tests..."

# Core functionality
./tests/rtmp_basic_test.sh
./tests/rtmp_protocol_test.sh
./tests/rtmp_video_test.sh

# Performance benchmarks
./tests/rtmp_performance_test.sh
./tests/rtmp_memory_test.sh

# Compatibility tests
./tests/rtmp_obs_test.sh
./tests/rtmp_ffmpeg_test.sh

# Generate report
./tests/generate_report.sh
```

This comprehensive testing guide ensures the RTMP server implementation is robust, performant, and compatible with standard RTMP clients and streaming software.
