# Authentication System

The Thingino Streamer includes a comprehensive authentication system that secures HTTP, RTSP, and ONVIF services while providing convenient localhost bypass functionality.

## Overview

The authentication system provides:
- **HTTP Basic Authentication** for all protocols
- **Localhost Bypass** - automatic authentication skip for local connections
- **Per-Protocol Configuration** - independent auth settings for HTTP, RTSP, and ONVIF
- **Backward Compatibility** - legacy RTSP configuration still supported

## Security Model

| Client Source | Auth Enabled | Localhost Bypass | Result              |
|---------------|--------------|------------------|---------------------|
| 127.x.x.x     | ✅           | ✅               | **Allow** (bypass)  |
| 127.x.x.x     | ✅           | ❌               | **Require Auth**    |
| External IP   | ✅           | ✅/❌            | **Require Auth**    |
| Any IP        | ❌           | ✅/❌            | **Allow** (disabled)|

## Configuration

All modules use the same authentication configuration format:

```json
{
  "auth": {
    "enabled": false,
    "localhost_bypass": true,
    "username": "admin",
    "password": "admin"
  }
}
```

### Configuration Parameters

- **`enabled`** (boolean): Enable/disable authentication
- **`localhost_bypass`** (boolean): Allow localhost (127.x.x.x) to skip authentication
- **`username`** (string): Username for Basic Authentication
- **`password`** (string): Password for Basic Authentication

## Protocol-Specific Configuration

### HTTP Authentication

Configure in `/etc/streamer.json` under the `http` section:

```json
{
  "http": {
    "enabled": true,
    "port": 8080,
    "auth": {
      "enabled": true,
      "localhost_bypass": true,
      "username": "admin",
      "password": "secretpassword"
    }
  }
}
```

### RTSP Authentication

Configure in `/etc/streamer.json` under the `rtsp` section:

```json
{
  "rtsp": {
    "enabled": true,
    "port": 554,
    "auth": {
      "enabled": true,
      "localhost_bypass": true,
      "username": "admin",
      "password": "secretpassword"
    }
  }
}
```

**Legacy Support**: The old `auth_required`, `username`, and `password` fields are still supported for backward compatibility.

### ONVIF Authentication

Configure in `/etc/streamer.json` under the `onvif` section:

```json
{
  "onvif": {
    "enabled": true,
    "auth": {
      "enabled": true,
      "localhost_bypass": true,
      "username": "admin",
      "password": "secretpassword"
    }
  }
}
```

## Quick Configuration Commands

### Enable Authentication for All Services

```bash
# HTTP
jct /etc/streamer.json set http.auth.enabled true
jct /etc/streamer.json set http.auth.username "admin"
jct /etc/streamer.json set http.auth.password "mypassword"

# RTSP
jct /etc/streamer.json set rtsp.auth.enabled true
jct /etc/streamer.json set rtsp.auth.username "admin"
jct /etc/streamer.json set rtsp.auth.password "mypassword"

# ONVIF
jct /etc/streamer.json set onvif.auth.enabled true
jct /etc/streamer.json set onvif.auth.username "admin"
jct /etc/streamer.json set onvif.auth.password "mypassword"
```

### Disable Localhost Bypass (Force Auth for All Connections)

```bash
jct /etc/streamer.json set http.auth.localhost_bypass false
jct /etc/streamer.json set rtsp.auth.localhost_bypass false
jct /etc/streamer.json set onvif.auth.localhost_bypass false
```

## Client Usage Examples

### HTTP API Access

```bash
# Localhost - no auth needed (if bypass enabled)
curl http://127.0.0.1:8080/status.json

# External - requires auth
curl -u admin:mypassword http://192.168.1.100:8080/status.json

# Browser access will prompt for credentials
```

### RTSP Streaming

```bash
# VLC/ffplay with authentication
ffplay -rtsp_flags prefer_tcp rtsp://admin:mypassword@192.168.1.100:554/stream0

# OBS Studio URL format
rtsp://admin:mypassword@192.168.1.100:554/stream0

# Localhost access (no auth if bypass enabled)
ffplay rtsp://127.0.0.1:554/stream0
```

### ONVIF Discovery and Access

```bash
# ONVIF Device Manager will prompt for credentials
# Use the configured username/password

# Direct SOAP requests require Basic Auth header
curl -u admin:mypassword -X POST \
  -H "Content-Type: application/soap+xml" \
  -H "SOAPAction: http://www.onvif.org/ver10/device/wsdl/GetDeviceInformation" \
  http://192.168.1.100:8080/onvif/device_service
```

## Security Considerations

### Strong Passwords
Always use strong passwords in production:
```bash
jct /etc/streamer.json set rtsp.auth.password "$(openssl rand -base64 32)"
```

### Localhost Bypass
- **Enable** for development and local tools
- **Disable** for maximum security in production environments
- Localhost detection covers 127.0.0.0/8 range

### Network Security
- Use RTSPS (RTSP over TLS) for encrypted streaming
- Consider VPN or firewall rules for additional protection
- Monitor access logs for unauthorized attempts

## Troubleshooting

### Authentication Not Working
1. Check configuration syntax: `jct /etc/streamer.json validate`
2. Restart streamer: `service streamer restart`
3. Check logs: `tail -f /var/log/streamer.log`

### Localhost Bypass Issues
1. Verify client IP: Check logs for actual client IP
2. Test with explicit 127.0.0.1: `curl http://127.0.0.1:8080/status.json`
3. Check bypass setting: `jct /etc/streamer.json get http.auth.localhost_bypass`

### Legacy RTSP Configuration
Old format still works but new format is recommended:
```bash
# Old (still supported)
jct /etc/streamer.json set rtsp.auth_required true
jct /etc/streamer.json set rtsp.username "admin"
jct /etc/streamer.json set rtsp.password "password"

# New (recommended)
jct /etc/streamer.json set rtsp.auth.enabled true
jct /etc/streamer.json set rtsp.auth.username "admin"
jct /etc/streamer.json set rtsp.auth.password "password"
```

## Implementation Details

The authentication system uses:
- **HTTP Basic Authentication** (RFC 7617)
- **Base64 encoding** for credentials
- **Socket-level client IP detection** for localhost bypass
- **Per-request authentication** checking
- **Proper HTTP/RTSP status codes** (401 Unauthorized, 403 Forbidden)

For technical implementation details, see `src/auth_utils.h` and `src/auth_utils.c`.
