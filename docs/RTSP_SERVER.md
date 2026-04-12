# RTSP Server Documentation

The Thingino RTSP Server provides RFC-compliant RTSP streaming with support for both unencrypted RTSP and secure RTSPS (RTSP over TLS) connections.

## Table of Contents

- [Overview](#overview)
- [Features](#features)
- [Configuration](#configuration)
- [RTSPS (Secure RTSP)](#rtsps-secure-rtsp)
- [Client Compatibility](#client-compatibility)
- [Troubleshooting](#troubleshooting)
- [Technical Details](#technical-details)

## Overview

The RTSP server enables real-time video streaming from Thingino cameras to RTSP clients like VLC, FFmpeg, and mobile applications. It supports multiple concurrent clients and provides both H.264 and H.265 video streaming.

### Supported Protocols

- **RTSP** (Real-Time Streaming Protocol) - RFC 2326
- **RTP** (Real-time Transport Protocol) - RFC 3984 (H.264), RFC 7798 (H.265)
- **RTSPS** (RTSP over TLS) - Encrypted RTSP connections

### Default Ports

- **RTSP**: 554 (standard unencrypted)
- **RTSPS**: 322 (standard encrypted)

## Features

### Core Features

- **Multi-client support**: Up to 8 concurrent clients
- **Dual codec support**: H.264 and H.265 video streaming
- **Multi-stream support**: Multiple video channels (e.g., main/sub streams)
- **Session management**: Automatic client timeout and cleanup
- **Authentication**: HTTP Basic Authentication with localhost bypass
- **Transport modes**: UDP and TCP transport support

### Security Features

- **RTSPS support**: TLS-encrypted RTSP connections
- **Certificate management**: Custom TLS certificates
- **Client verification**: Optional client certificate validation
- **Modern TLS**: TLS 1.2+ with secure cipher suites

### Performance Features

- **Zero-copy streaming**: Efficient memory usage
- **Frame manager integration**: Optimized frame distribution
- **Timeout protection**: Prevents infinite waiting for parameters
- **Resource monitoring**: Connection and bandwidth tracking

## Configuration

The RTSP server is configured via `/etc/streamer.d/rtsp.json`:

```json
{
  "enabled": true,
  "port": 554,
  "session_reclaim": 65,
  "auth": {
    "enabled": false,
    "localhost_bypass": true,
    "username": "admin",
    "password": "admin"
  },
  "server_name": "Thingino RTSP Server",
  "max_clients": 8,
  "max_sessions": 16,
  "tls_enabled": true,
  "tls_port": 322,
  "cert_file": "/etc/ssl/certs/rtsp-server.crt",
  "key_file": "/etc/ssl/private/rtsp-server.key",
  "tls_verify_client": false
}
```

### Configuration Parameters

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| Parameter               | Type    | Default                   | Description                      |
|-------------------------|---------|---------------------------|----------------------------------|
| `enabled`               | boolean | `true`                    | Enable/disable RTSP server      |
| `port`                  | integer | `554`                     | RTSP server port                 |
| `session_reclaim`       | integer | `65`                      | Session timeout in seconds       |
| `auth.enabled`          | boolean | `false`                   | Enable authentication            |
| `auth.localhost_bypass` | boolean | `true`                    | Allow localhost to skip auth     |
| `auth.username`         | string  | `"admin"`                 | Authentication username          |
| `auth.password`         | string  | `"admin"`                 | Authentication password          |
| `server_name`           | string  | `"Thingino RTSP Server"`  | Server identification            |
| `max_clients`           | integer | `8`                       | Maximum concurrent clients       |
| `max_sessions`          | integer | `16`                      | Maximum total sessions           |
| `tls_enabled`           | boolean | `true`                    | Enable RTSPS (TLS)               |
| `tls_port`              | integer | `322`                     | RTSPS server port                |
| `cert_file`             | string  | `""`                      | Path to TLS certificate          |
| `key_file`              | string  | `""`                      | Path to TLS private key          |
| `tls_verify_client`     | boolean | `false`                   | Require client certificates      |

### Authentication Configuration

The RTSP server supports HTTP Basic Authentication with localhost bypass:

```bash
# Enable authentication
jct /etc/streamer.json set rtsp.auth.enabled true
jct /etc/streamer.json set rtsp.auth.username "admin"
jct /etc/streamer.json set rtsp.auth.password "mypassword"

# Disable localhost bypass (force auth for all connections)
jct /etc/streamer.json set rtsp.auth.localhost_bypass false
```

**Localhost Bypass**: When enabled (default), clients connecting from 127.x.x.x addresses skip authentication. This is convenient for local development and tools while still securing external access.

**Legacy Support**: The old `auth_required`, `username`, and `password` fields are still supported for backward compatibility.

## RTSPS (Secure RTSP)

RTSPS provides encrypted RTSP connections using TLS, ensuring secure video streaming over untrusted networks.

### Certificate Generation

Generate TLS certificates directly on the camera using OpenSSL:

```bash
# Create certificate directory
mkdir -p /etc/ssl/certs /etc/ssl/private

# Generate self-signed certificate (for testing)
openssl req -x509 -newkey rsa:2048 -keyout /etc/ssl/private/rtsp-server.key \
            -out /etc/ssl/certs/rtsp-server.crt -days 365 -nodes \
            -subj "/C=US/ST=State/L=City/O=Organization/CN=camera.local"

# Set proper permissions
chmod 600 /etc/ssl/private/rtsp-server.key
chmod 644 /etc/ssl/certs/rtsp-server.crt
```

### Production Certificates

For production use, obtain certificates from a Certificate Authority:

```bash
# Copy your certificates to the camera
scp server.crt root@camera:/etc/ssl/certs/rtsp-server.crt
scp server.key root@camera:/etc/ssl/private/rtsp-server.key

# Set proper permissions
chmod 600 /etc/ssl/private/rtsp-server.key
chmod 644 /etc/ssl/certs/rtsp-server.crt
```

### RTSPS Client Connection

Connect to RTSPS using the `rtsps://` protocol:

```bash
# VLC
vlc rtsps://192.168.1.109:322/ch0

# FFplay
ffplay rtsps://192.168.1.109:322/ch0

# FFmpeg (ignore certificate errors for self-signed)
ffplay -rtsp_flags prefer_tcp -fflags nobuffer \
       -tls_verify 0 rtsps://192.168.1.109:322/ch0
```

### TLS Configuration Options

- **Certificate Files**: Specify custom certificate and key files
- **Client Verification**: Optionally require client certificates
- **TLS Version**: Uses TLS 1.2+ with secure cipher suites
- **SNI Support**: Server Name Indication for virtual hosting

## Client Compatibility

### Tested Clients

| Client | RTSP | RTSPS | Authentication | Notes |
|--------|------|-------|----------------|-------|
| **VLC** | ✅ | ✅ | ✅ | Excellent compatibility |
| **FFmpeg/FFplay** | ✅ | ✅ | ✅ | Highly configurable |
| **mpv** | ✅ | ✅ | ✅ | Fast startup, smooth playback |
| **GStreamer** | ✅ | ✅ | ✅ | Good for embedded applications |
| **Mobile Apps** | ✅ | ⚠️ | ✅ | RTSPS support varies |

### Connection Examples

```bash
# Standard RTSP (unencrypted)
rtsp://192.168.1.109:554/ch0
rtsp://192.168.1.109:554/ch1

# Secure RTSPS (encrypted)
rtsps://192.168.1.109:322/ch0
rtsps://192.168.1.109:322/ch1

# With authentication
rtsp://username:password@192.168.1.109:554/ch0
rtsps://username:password@192.168.1.109:322/ch0
```

### Stream Endpoints

- `/ch0` - Main stream (high resolution)
- `/ch1` - Sub stream (low resolution)
- Custom endpoints configured per stream

## Troubleshooting

### Common Issues

#### Connection Refused
```
Connection to tcp://192.168.1.109:322 failed: Connection refused
```

**Solutions:**
- Verify RTSP server is running: `ps | grep streamer`
- Check configuration: `cat /etc/streamer.d/rtsp.json`
- Verify ports are listening: `netstat -ln | grep :322`
- Check firewall settings

#### Certificate Errors
```
TLS handshake failed
```

**Solutions:**
- Verify certificate files exist and have correct permissions
- Check certificate validity: `openssl x509 -in /etc/ssl/certs/rtsp-server.crt -text -noout`
- For self-signed certificates, use `-tls_verify 0` in FFmpeg
- Ensure certificate matches hostname/IP

#### Black Screen/No Video
```
Stream starts but shows black screen
```

**Solutions:**
- Check if SPS/PPS parameters are being sent
- Verify codec compatibility (H.264 vs H.265)
- Try different transport mode (UDP vs TCP)
- Check encoder configuration

#### Authentication Failures
```
401 Unauthorized
```

**Solutions:**
- Verify username/password in configuration
- Check URL format includes credentials
- Ensure `auth_required` is set correctly

### Debug Commands

```bash
# Check RTSP server status
ps aux | grep streamer

# View server logs
logread | grep RTSP

# Test connectivity
telnet 192.168.1.109 554
telnet 192.168.1.109 322

# Check certificates
openssl s_client -connect 192.168.1.109:322 -servername camera.local

# Monitor network traffic
tcpdump -i any port 554 or port 322
```

## Technical Details

### Architecture

The RTSP server uses a modular architecture:

- **Server Thread**: Handles client connections and RTSP commands
- **RTP Thread**: Manages media packet transmission
- **Frame Manager**: Distributes video frames to clients
- **TLS Layer**: Provides encryption for RTSPS connections

### Memory Usage

- **Base RTSP**: ~200KB for server and client management
- **RTSPS Addition**: ~100KB for TLS contexts and encryption
- **Per Client**: ~50KB for session state and buffers

### Performance Characteristics

- **Startup Time**: < 1 second for server initialization
- **Client Connection**: < 500ms for RTSP, < 1s for RTSPS
- **Frame Latency**: < 100ms typical
- **Concurrent Clients**: Up to 8 clients tested

### Protocol Compliance

- **RTSP 1.0**: RFC 2326 compliant
- **RTP**: RFC 3984 (H.264), RFC 7798 (H.265)
- **SDP**: RFC 4566 compliant
- **TLS**: TLS 1.2+ with modern cipher suites

### Security Considerations

- **Certificate Management**: Use proper CA-signed certificates in production
- **Key Protection**: Secure private key file permissions (600)
- **Client Authentication**: Enable for sensitive deployments
- **Network Security**: Use RTSPS over untrusted networks
- **Access Control**: Implement firewall rules as needed

For more detailed technical information, see:
- [RTSP Flow Documentation](RTSP_FLOW.md)
- [RTSP Client Compatibility](RTSP_CLIENTS.md)
- [Architecture Overview](ARCHITECTURE.md)
