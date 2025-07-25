# Thingino Streamer

## Status: ACTIVE - CURRENT BUILD

These files are compiled and used in the current pure C implementation.

## File Structure

### Core Application

- `main.c` - **Main entry point** for pure C implementation
    - IMP system initialization following official Ingenic samples
    - Encoder setup and threading
    - RTSP server integration
    - Complete application lifecycle management

### Configuration System

- `config.c` - **Configuration parsing and management**
    - JSON configuration file parsing
    - Sensor information detection from `/proc/jz/sensor/`
    - Stream configuration validation
    - Default value handling
- `config.h` - Configuration system header and structures

### Authentication System

- `auth_utils.c` - **Unified authentication implementation**
    - HTTP Basic Authentication (RFC 7617)
    - Localhost bypass functionality (127.x.x.x detection)
    - Base64 encoding/decoding
    - Protocol-agnostic authentication checking
    - Client IP detection and validation
- `auth_utils.h` - Authentication system header and API

### Snapshot Fallback System

- `snapshot_fallback.c` - **Intelligent snapshot fallback implementation**
    - Automatic activation when HTTP module unavailable
    - Multi-channel JPEG snapshot capture to filesystem
    - Configurable capture intervals and file management
    - IMP encoder integration with proper resource handling
    - External WebUI package support via `/tmp/` directory
- `snapshot_fallback.h` - Snapshot fallback system header and API

### Logging System

- `logger.c` - **Pure C logging implementation**
    - Multiple log levels (DEBUG, INFO, WARN, ERROR)
    - Thread-safe logging
    - Configurable output destinations
- `logger.h` - Logging system header and macros

### RTSP Server

- `rtsp_server.c` - **Complete RTSP/RTSPS server implementation**
    - Full RTSP protocol support (OPTIONS, DESCRIBE, SETUP, PLAY)
    - RTSPS (RTSP over TLS) secure streaming support
    - HTTP Basic Authentication with localhost bypass
    - TCP and UDP transport modes
    - Perfect SDP generation with H.264/H.265 parameters
    - Multi-client support with proper state management
    - TLS encryption with OpenSSL backend
    - Professional-grade streaming infrastructure
- `rtsp_server.h` - RTSP server header and API
- `rtsp_module.c` - RTSP module integration and configuration

### Video Input

- `video_input.c` - **Video capture and encoding**
    - IMP FrameSource initialization and management
    - H.264 encoder configuration and control
    - Frame capture and streaming threads
    - Official Ingenic sample pattern implementation
- `video_input.h` - Video input header and structures

## Architecture

### Initialization Sequence (Official Ingenic Pattern)

1. **System Init** - IMP_System_Init() with ISP setup
2. **FrameSource Init** - Create channels (no enable yet)
3. **Encoder Init** - Create channels and groups
4. **Binding** - Bind FrameSource → Encoder
5. **Stream On** - Enable FrameSource channels AFTER binding
6. **Thread Creation** - Create encoder streaming threads
7. **Inside Threads** - sleep(1) → StartRecvPic → Polling loop

### Threading Model

- **Main Thread** - Initialization and RTSP server management
- **RTSP Thread** - Client connection handling
- **RTP Thread** - RTP packet transmission
- **Encoder Thread** - Frame capture and encoding (per channel)

### Memory Management

- **Stack-based allocation** where possible
- **Careful buffer management** for embedded constraints
- **Proper cleanup** on shutdown and error conditions

## Key Features

### ✅ **Implemented and Working**

- **Complete RTSP Protocol** - Full handshake and streaming
- **RTSPS Secure Streaming** - TLS-encrypted RTSP connections
- **Perfect H.264/H.265 SDP** - Matching working camera format
- **Multi-stream Support** - Main (1920x1080) and Sub (640x360) streams
- **Transport Flexibility** - TCP interleaved and UDP modes
- **TLS Security** - OpenSSL-based encryption and certificate support
- **Official Sample Pattern** - Exact Ingenic initialization sequence
- **Robust Error Handling** - Comprehensive error checking and recovery

### 🔄 **In Progress**

- **Frame Production** - Encoder polling and data flow optimization
- **Performance Tuning** - Timing and buffer optimization

### ❌ **Not Yet Implemented**

- **Audio Support** - Audio encoding and streaming
- **Motion Detection** - Video analysis features
- **OSD Functionality** - On-screen display
- **WebSocket Interface** - Web-based control
- **Advanced Features** - Adaptive bitrate, zone management

## Build System

The pure C implementation is built using the standard Makefile:

```bash
./build.sh  # Builds using Thingino buildroot
```

## Dependencies

### Required Libraries

- **libimp** - Ingenic IMP system library
- **libalog** - Audio logging library
- **libaudioProcess** - Audio processing library
- **libsysutils** - System utilities library
- **libjson-c** - JSON parsing library
- **libatomic** - Atomic operations library
- **libmuslshim** - Musl C library shim

### Optional Libraries (Feature Flags)

- **libwebsockets** - WebSocket support
- **libschrift** - Font rendering for OSD
- **libopus** - Opus audio codec
- **libfaac** - AAC audio codec
- **libhelix-aac** - Helix AAC decoder

## Testing

### RTSP Streaming Test

```bash
# Standard RTSP (unencrypted)
ffplay -v debug -rtsp_transport tcp rtsp://IP:554/ch0
ffplay -v debug -rtsp_transport udp rtsp://IP:554/ch0

# Secure RTSPS (encrypted)
ffplay -v debug -tls_verify 0 rtsps://IP:322/ch0

# With authentication
ffplay -v debug rtsp://username:password@IP:554/ch0
```

### Debug Output

The implementation provides extensive debug output for troubleshooting:

- IMP system initialization steps
- Encoder configuration details
- RTSP protocol messages
- Frame capture statistics
- Error conditions and recovery

## Legacy C++ Code

The original C++ implementation has been moved to `src-legacy-cpp/` for reference during the conversion process. See
`src-legacy-cpp/README.md` for details.

## Documentation

### RTSP/RTSPS Documentation
- [RTSP Server Documentation](RTSP_SERVER.md) - Complete RTSP/RTSPS server guide
- [RTSPS Quick Reference](RTSPS_QUICK_REFERENCE.md) - Quick setup and troubleshooting
- [RTSP Client Compatibility](RTSP_CLIENTS.md) - Tested clients and compatibility
- [RTSP Protocol Flow](RTSP_FLOW.md) - Technical protocol details

### RTMP Documentation
- [RTMP Client Documentation](RTMP_CLIENT.md) - RTMP streaming setup
- [RTMP Quick Start](RTMP_QUICK_START.md) - Quick configuration guide
- [RTMP Technical Details](RTMP_TECHNICAL.md) - Implementation details

### General Documentation
- [Architecture Overview](ARCHITECTURE.md) - System architecture
- [Configuration Reference](QUICK_CONFIG_REFERENCE.md) - Configuration options
- [Debugging Guide](DEBUGGING_GUIDE.md) - Troubleshooting and debugging

## Development Notes

- **Follow official Ingenic samples** for IMP API usage
- **Minimize resource usage** for embedded device constraints
- **Extensive debugging output** for development and troubleshooting
- **Clean C code style** with proper error handling
- **Thread safety** for multi-threaded operations
