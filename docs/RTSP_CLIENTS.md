# RTSP Clients

This document provides an overview of various RTSP clients and their compatibility with the Thingino RTSP Server, including support for RTSPS (RTSP over TLS) secure connections.

## Protocol Support

- **RTSP**: Standard unencrypted RTSP on port 554
- **RTSPS**: Secure RTSP over TLS on port 322

## Tested Clients

### mpv

- **RTSP Compatibility**: Excellent
- **RTSPS Compatibility**: Excellent
- **Features**: Supports RTSP PAUSE/PLAY commands, TLS encryption
- **Pros**: Fast startup, smooth playback, good RTSPS support
- **Cons**: None significant
- **Recommendation**: Primary client for testing and production

**Connection Examples:**
```bash
# Standard RTSP
mpv rtsp://192.168.1.109:554/ch0

# Secure RTSPS
mpv rtsps://192.168.1.109:322/ch0
```

MPV handles pause/resume differently than other RTSP clients. MPV implements client-side pause rather than sending RTSP PAUSE/PLAY commands to the server.

### How MPV Handles Pause:

**Client-Side Pause (MPV):**
- Pause: MPV stops reading/displaying frames locally but keeps the RTSP connection alive
- Resume: MPV resumes reading/displaying frames from the buffer
- No RTSP commands sent - Server continues streaming normally

**Server-Side Pause (Other Clients):**
- Pause: Client sends RTSP PAUSE command to server
- Resume: Client sends RTSP PLAY command to server
- Server stops/starts streaming - More network efficient

### VLC

- **RTSP Compatibility**: Excellent
- **RTSPS Compatibility**: Excellent
- **Features**: Supports RTSP PAUSE/PLAY commands, TLS encryption
- **Pros**: User-friendly GUI, wide format support, good RTSPS support
- **Cons**: Slightly slower startup than mpv
- **Recommendation**: Secondary client for testing and production

**Connection Examples:**
```bash
# Standard RTSP
vlc rtsp://192.168.1.109:554/ch0

# Secure RTSPS
vlc rtsps://192.168.1.109:322/ch0

# With authentication
vlc rtsp://username:password@192.168.1.109:554/ch0
```

### FFmpeg

- **RTSP Compatibility**: Excellent
- **RTSPS Compatibility**: Excellent
- **Features**: Supports RTSP PAUSE/PLAY commands, TLS encryption, advanced options
- **Pros**: Highly configurable, command-line interface, excellent RTSPS support
- **Cons**: Steeper learning curve for complex configurations
- **Recommendation**: Useful for advanced testing and debugging

**Connection Examples:**
```bash
# Standard RTSP
ffplay rtsp://192.168.1.109:554/ch0

# Secure RTSPS
ffplay rtsps://192.168.1.109:322/ch0

# RTSPS with self-signed certificate (ignore verification)
ffplay -tls_verify 0 rtsps://192.168.1.109:322/ch0

# Advanced options
ffplay -rtsp_flags prefer_tcp -fflags nobuffer rtsps://192.168.1.109:322/ch0
```

## RTSPS (Secure RTSP) Support

### Certificate Handling

Most clients support RTSPS but may require additional configuration for self-signed certificates:

**FFmpeg/FFplay:**
```bash
# Ignore certificate verification (for self-signed certificates)
ffplay -tls_verify 0 rtsps://192.168.1.109:322/ch0

# Specify custom CA certificate
ffplay -ca_file /path/to/ca.crt rtsps://192.168.1.109:322/ch0
```

**VLC:**
- VLC typically handles self-signed certificates with user prompts
- For automation, configure certificate trust in system certificate store

**mpv:**
- Generally handles certificates well
- May require `--tls-verify=no` for self-signed certificates

### Mobile Applications

| Application | RTSP | RTSPS | Notes |
|-------------|------|-------|-------|
| **VLC Mobile** | ✅ | ✅ | Good RTSPS support |
| **RTSP Player** | ✅ | ⚠️ | Limited RTSPS support |
| **IP Cam Viewer** | ✅ | ⚠️ | RTSPS support varies |
| **tinyCam Monitor** | ✅ | ⚠️ | Basic RTSPS support |

### Troubleshooting RTSPS Connections

**Common Issues:**

1. **Certificate Verification Errors**
   - Use `-tls_verify 0` for testing with self-signed certificates
   - Install proper CA certificates for production

2. **TLS Handshake Failures**
   - Check certificate validity and permissions
   - Verify TLS port (322) is accessible

3. **Connection Timeouts**
   - Ensure firewall allows port 322
   - Check if RTSPS is enabled in server configuration
