# System Documentation Updates

This document summarizes all documentation changes made to reflect the new unified authentication system and snapshot fallback system implemented in the Thingino Streamer.

## New Documentation Files

### 1. `AUTHENTICATION.md` - **NEW**
Comprehensive authentication guide covering:
- Security model and behavior matrix
- Configuration format for all protocols
- Quick configuration commands
- Client usage examples
- Security considerations and troubleshooting

### 2. `ONVIF_SERVER.md` - **NEW**
Complete ONVIF server documentation including:
- ONVIF standards and features
- Configuration parameters
- Authentication setup
- Supported services and operations
- Client compatibility and troubleshooting

### 3. `SNAPSHOT_FALLBACK.md` - **NEW**
Comprehensive snapshot fallback system documentation including:
- Automatic activation logic and architecture
- Configuration parameters and examples
- WebUI package integration guidelines
- Performance characteristics and file management
- Troubleshooting and API reference

## Updated Documentation Files

### 4. `README.md` - **UPDATED**
- Added authentication system section to core architecture
- Added snapshot fallback system section to core architecture
- Updated RTSP server description to mention authentication
- Added auth_utils.c/h and snapshot_fallback.c/h to file structure documentation

### 5. `QUICK_CONFIG_REFERENCE.md` - **UPDATED**
- Replaced old RTSP authentication section with unified auth commands
- Added HTTP and ONVIF authentication configuration
- Updated to use new `auth.*` configuration format
- Added localhost bypass configuration examples
- Added snapshot fallback configuration section with examples

### 5. `RTSP_SERVER.md` - **UPDATED**
- Updated features to mention "HTTP Basic Authentication with localhost bypass"
- Replaced old configuration format with new `auth` object structure
- Updated configuration parameters table with new auth fields
- Added dedicated authentication configuration section
- Added localhost bypass explanation and legacy support notes

### 6. `HTTP_DYNAMIC_ROUTING.md` - **UPDATED**
- Added "Authentication checking before route dispatching" to HTTP module integration
- Added snapshot fallback system integration note

### 7. `TECHNICAL_IMPLEMENTATION.md` - **UPDATED**
- Added comprehensive authentication system section
- Added comprehensive snapshot fallback system section
- Included code examples for authentication flow and IMP encoder integration
- Documented localhost detection algorithm and circular buffer handling
- Showed protocol integration points for HTTP and RTSP
- Added performance optimization notes for both systems

## Configuration Format Changes

### Old Format (RTSP only)
```json
{
  "rtsp": {
    "auth_required": false,
    "username": "thingino",
    "password": "thingino"
  }
}
```

### New Format (All Protocols)
```json
{
  "http": {
    "auth": {
      "enabled": false,
      "localhost_bypass": true,
      "username": "admin",
      "password": "admin"
    }
  },
  "rtsp": {
    "auth": {
      "enabled": false,
      "localhost_bypass": true,
      "username": "admin",
      "password": "admin"
    }
  },
  "onvif": {
    "auth": {
      "enabled": false,
      "localhost_bypass": true,
      "username": "admin",
      "password": "admin"
    }
  },
  "snapshot_fallback": {
    "enabled": true,
    "output_dir": "/tmp",
    "update_interval_ms": 5000,
    "overwrite_existing": true,
    "max_file_age_seconds": 3600
  }
}
```

## Key Documentation Themes

### 1. **Unified Authentication**
All documentation now emphasizes the consistent authentication system across all protocols, using the same configuration format and behavior.

### 2. **Localhost Bypass**
Extensively documented the localhost bypass feature, including:
- Security behavior matrix
- Configuration options
- Use cases (development vs. production)
- IP range coverage (127.0.0.0/8)

### 3. **Backward Compatibility**
Clearly documented that legacy RTSP configuration (`auth_required`, `username`, `password`) is still supported for backward compatibility.

### 4. **Security Best Practices**
Added security considerations including:
- Strong password recommendations
- Network security suggestions
- Production vs. development configurations

### 5. **Client Integration**
Provided practical examples for:
- HTTP API access with curl
- RTSP streaming with VLC/ffplay
- ONVIF client integration
- Browser-based access

## Quick Reference Updates

### Configuration Commands
```bash
# Enable authentication for all services
jct /etc/streamer.json set http.auth.enabled true
jct /etc/streamer.json set rtsp.auth.enabled true
jct /etc/streamer.json set onvif.auth.enabled true

# Set credentials
jct /etc/streamer.json set http.auth.username "admin"
jct /etc/streamer.json set http.auth.password "mypassword"

# Control localhost bypass
jct /etc/streamer.json set rtsp.auth.localhost_bypass false

# Configure snapshot fallback
jct /etc/streamer.json set snapshot_fallback.enabled true
jct /etc/streamer.json set snapshot_fallback.update_interval_ms 10000
jct /etc/streamer.json set snapshot_fallback.output_dir "/var/snapshots"
```

### Client Usage
```bash
# HTTP with auth
curl -u admin:password http://192.168.1.100:8080/status.json

# RTSP with auth
ffplay rtsp://admin:password@192.168.1.100:554/stream0

# ONVIF with auth
curl -u admin:password -X POST http://192.168.1.100:8080/onvif/device_service

# Access snapshot fallback files
ls -la /tmp/snap*.jpg
curl http://127.0.0.1:80/tmp/snap0.jpg  # Via WebUI package
```

## Documentation Structure

The system documentation is now organized hierarchically:

1. **Primary Guides**
   - `AUTHENTICATION.md` - Authentication system guide
   - `SNAPSHOT_FALLBACK.md` - Snapshot fallback system guide
2. **Protocol-specific docs** - Detailed integration information
   - `RTSP_SERVER.md` - RTSP authentication details
   - `ONVIF_SERVER.md` - ONVIF authentication details
   - `HTTP_DYNAMIC_ROUTING.md` - HTTP authentication and snapshot integration
3. **`QUICK_CONFIG_REFERENCE.md`** - Quick commands and examples for all systems
4. **`TECHNICAL_IMPLEMENTATION.md`** - Implementation details and algorithms

This structure provides both high-level guidance and detailed technical information for different user needs.

## Migration Guide

For users upgrading from the old authentication system:

### Immediate Actions Required
- **None** - Legacy RTSP configuration continues to work

### Recommended Actions
1. Review new authentication and snapshot fallback documentation
2. Consider enabling authentication for HTTP and ONVIF
3. Migrate to new configuration format for consistency
4. Test localhost bypass behavior in your environment
5. Configure snapshot fallback if using external WebUI packages

### Breaking Changes
- **None** - All changes are backward compatible

### New Features Added
- **Snapshot Fallback System** - Automatic JPEG snapshot generation to `/tmp/` when HTTP module unavailable
- **WebUI Package Support** - External WebUI packages can access snapshots independently
- **Intelligent Activation** - System automatically detects when fallback is needed

The documentation updates ensure that users have comprehensive guidance for implementing and managing the new unified authentication system and snapshot fallback system while maintaining full backward compatibility.
