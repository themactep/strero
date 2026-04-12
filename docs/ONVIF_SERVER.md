# ONVIF Server Documentation

The Thingino ONVIF Server provides ONVIF-compliant device management and media streaming services, enabling integration with professional IP camera management systems.

## Table of Contents

- [Overview](#overview)
- [Features](#features)
- [Configuration](#configuration)
- [Authentication](#authentication)
- [Supported Services](#supported-services)
- [Client Compatibility](#client-compatibility)
- [Troubleshooting](#troubleshooting)

## Overview

The ONVIF server implements the Open Network Video Interface Forum (ONVIF) standards, allowing Thingino cameras to integrate with professional video management systems, NVRs, and ONVIF-compatible applications.

### Supported Standards

- **ONVIF Profile S** - Video streaming and basic device management
- **WS-Discovery** - Automatic device discovery on the network
- **SOAP/HTTP** - Web services communication protocol

### Default Endpoints

- **Device Service**: `/onvif/device_service`
- **Media Service**: `/onvif/media_service`
- **Event Service**: `/onvif/event_service`
- **Snapshot**: `/onvif/snapshot`

## Features

### Core Features

- **Device Discovery**: WS-Discovery for automatic network detection
- **Device Management**: System information, capabilities, and configuration
- **Media Profiles**: Video stream configuration and management
- **Snapshot Service**: JPEG image capture
- **Authentication**: HTTP Basic Authentication with localhost bypass

### Supported Operations

#### Device Service
- `GetDeviceInformation` - Device details and capabilities
- `GetCapabilities` - Service capabilities and endpoints
- `GetSystemDateAndTime` - System time information
- `GetServiceCapabilities` - Device service capabilities

#### Media Service
- `GetProfiles` - Available media profiles
- `GetVideoSources` - Video input sources
- `GetStreamUri` - RTSP streaming URLs
- `GetSnapshotUri` - Snapshot capture URLs

#### Event Service
- Basic event service support (limited implementation)

## Configuration

The ONVIF server is configured via `/etc/streamer.d/onvif.json`:

```json
{
  "enabled": true,
  "auth": {
    "enabled": false,
    "localhost_bypass": true,
    "username": "admin",
    "password": "admin"
  },
  "device_name": "Thingino Camera",
  "device_location": "Embedded",
  "manufacturer": "Thingino",
  "model": "Streamer",
  "serial_number": "123456789",
  "firmware_version": "1.0.0",
  "hardware_id": "thingino-hw1"
}
```

### Configuration Parameters

| Parameter               | Type    | Default             | Description                       |
|-------------------------|---------|---------------------|-----------------------------------|
| `enabled`               | boolean | `true`              | Enable/disable ONVIF server       |
| `auth.enabled`          | boolean | `false`             | Enable authentication             |
| `auth.localhost_bypass` | boolean | `true`              | Allow localhost to skip auth      |
| `auth.username`         | string  | `"admin"`           | Authentication username           |
| `auth.password`         | string  | `"admin"`           | Authentication password           |
| `device_name`           | string  | `"Thingino Camera"` | Device display name               |
| `device_location`       | string  | `"Embedded"`        | Device location description       |
| `manufacturer`          | string  | `"Thingino"`        | Manufacturer name                 |
| `model`                 | string  | `"Streamer"`        | Device model                      |
| `serial_number`         | string  | `"123456789"`       | Device serial number              |
| `firmware_version`      | string  | `"1.0.0"`           | Firmware version                  |
| `hardware_id`           | string  | `"thingino-hw1"`    | Hardware identifier               |

## Authentication

The ONVIF server supports HTTP Basic Authentication with localhost bypass functionality:

### Enable Authentication

```bash
# Enable ONVIF authentication
jct /etc/streamer.json set onvif.auth.enabled true
jct /etc/streamer.json set onvif.auth.username "admin"
jct /etc/streamer.json set onvif.auth.password "mypassword"

# Disable localhost bypass (force auth for all connections)
jct /etc/streamer.json set onvif.auth.localhost_bypass false
```

### Authentication Behavior

| Client Source | Auth Enabled | Localhost Bypass | Result              |
|---------------|--------------|------------------|---------------------|
| 127.x.x.x     | ✅           | ✅               | **Allow** (bypass)  |
| 127.x.x.x     | ✅           | ❌               | **Require Auth**    |
| External IP   | ✅           | ✅/❌            | **Require Auth**    |
| Any IP        | ❌           | ✅/❌            | **Allow** (disabled)|

### Client Usage

Most ONVIF clients will prompt for credentials when authentication is enabled:

```bash
# Direct SOAP request with authentication
curl -u admin:mypassword -X POST \
  -H "Content-Type: application/soap+xml" \
  -H "SOAPAction: http://www.onvif.org/ver10/device/wsdl/GetDeviceInformation" \
  http://192.168.1.100:8080/onvif/device_service

# Localhost access (no auth if bypass enabled)
curl -X POST \
  -H "Content-Type: application/soap+xml" \
  http://127.0.0.1:8080/onvif/device_service
```

## Supported Services

### Device Service (`/onvif/device_service`)

Provides device information and capabilities:

- Device identification and capabilities
- System date and time
- Network configuration information
- Service endpoints and versions

### Media Service (`/onvif/media_service`)

Manages video profiles and streaming:

- Media profile enumeration
- Video source configuration
- RTSP stream URI generation
- Snapshot URI generation

### Event Service (`/onvif/event_service`)

Basic event service support:

- Event service capabilities
- Limited event subscription support

### Snapshot Service (`/onvif/snapshot`)

Direct JPEG snapshot capture:

```bash
# Get snapshot from channel 0
curl http://192.168.1.100:8080/onvif/snapshot

# Get snapshot from specific channel
curl http://192.168.1.100:8080/onvif/snapshot?channel=1
```

## Client Compatibility

### Tested ONVIF Clients

- **ONVIF Device Manager** - Full compatibility
- **VLC Media Player** - Stream discovery and playback
- **Blue Iris** - Device discovery and streaming
- **Milestone XProtect** - Basic integration
- **Generic NVR Systems** - Standard ONVIF Profile S support

### Integration Examples

#### ONVIF Device Manager
1. Launch ONVIF Device Manager
2. Click "Discovery" to find devices
3. Enter credentials if authentication is enabled
4. Access device information and live streams

#### VLC Media Player
1. Open VLC → Media → Open Network Stream
2. Use RTSP URL from GetStreamUri response
3. Enter credentials if required

## Troubleshooting

### Device Not Discovered

1. **Check WS-Discovery**: Ensure multicast is working on your network
2. **Firewall**: Verify ports 3702 (discovery) and 8080 (HTTP) are open
3. **Network**: Ensure client and camera are on the same subnet

### Authentication Issues

1. **Check credentials**: Verify username/password in configuration
2. **Test localhost**: Try accessing from 127.0.0.1 to test bypass
3. **Client support**: Ensure ONVIF client supports Basic Authentication

### Service Errors

1. **Check logs**: Monitor `/var/log/streamer.log` for ONVIF errors
2. **Restart service**: `service streamer restart`
3. **Validate config**: `jct /etc/streamer.json validate`

### Common Error Messages

- **"ONVIF service unavailable"**: Module is disabled in configuration
- **"401 Unauthorized"**: Authentication required but not provided
- **"Could not determine SOAP action"**: Malformed SOAP request

## Technical Implementation

The ONVIF server is implemented as a modular component:

- **`onvif_module.c`** - Module initialization and configuration
- **`onvif_services.c`** - SOAP service implementations
- **`onvif_discovery.c`** - WS-Discovery service
- **HTTP Integration** - Routes registered with HTTP dynamic routing system

For detailed technical information, see the source code in `src/modules/onvif/`.
