# ONVIF Troubleshooting Guide

## Common Issues and Solutions

### Authentication Problems

#### Issue: 403 Invalid Credentials
```
[W] HTTP_MODULE: 403 - Invalid credentials from 192.168.1.127
```

**Causes:**
- Client sending empty credentials (`Authorization: Basic Og==`)
- Credential mismatch between client and server
- HTTP module blocking ONVIF requests

**Solutions:**
1. **Check ONVIF configuration** in `/etc/streamer.d/onvif.json`:
   ```json
   {
     "auth": {
       "enabled": true,
       "username": "thingino",
       "password": "thingino"
     }
   }
   ```

2. **Configure client credentials** to match server:
   - Username: `thingino`
   - Password: `thingino`

3. **Verify HTTP authentication bypass**:
   ```bash
   journalctl -f | grep "ONVIF request detected - bypassing HTTP authentication"
   ```

#### Issue: WS-Security Authentication Failure
```
[W] AUTH_UTILS: WS-Security authentication failed: username mismatch
```

**Causes:**
- Client sending different username than configured
- Malformed WS-Security token
- Missing SOAP security headers

**Solutions:**
1. **Check WS-Security token parsing**:
   ```bash
   journalctl -f | grep "Found WS-Security UsernameToken"
   ```

2. **Verify client SOAP headers** include:
   ```xml
   <Security xmlns="http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-secext-1.0.xsd">
     <UsernameToken>
       <Username>thingino</Username>
       <Password Type="...">...</Password>
     </UsernameToken>
   </Security>
   ```

### Route and Service Issues

#### Issue: 404 Unknown Path
```
[W] HTTP_MODULE: 404 - Unknown path in request: POST /onvif/imaging_service
```

**Causes:**
- Missing route registration
- ONVIF module not enabled
- Service not implemented

**Solutions:**
1. **Check available routes**:
   ```bash
   journalctl -f | grep "HTTP_ROUTER.*onvif"
   ```

2. **Verify ONVIF module is enabled**:
   ```bash
   grep "ONVIF module" /var/log/messages
   ```

3. **Supported ONVIF routes**:
   - `/onvif/device_service`
   - `/onvif/media_service`
   - `/onvif/event_service`
   - `/onvif/imaging_service`
   - `/onvif/ptz_service`
   - `/onvif/snapshot`

#### Issue: Unsupported ONVIF Action
```
[W] ONVIF_SERVICES: Unsupported ONVIF action: http://www.onvif.org/ver10/device/wsdl/GetUsers
```

**Causes:**
- Client requesting unimplemented operation
- Service partially implemented

**Solutions:**
1. **Check supported operations** per service:
   - **Device Service**: GetCapabilities, GetDeviceInformation, SystemReboot
   - **Media Service**: GetProfiles, GetStreamUri, GetSnapshotUri
   - **Imaging Service**: GetOptions
   - **PTZ Service**: GetServiceCapabilities

2. **For unsupported operations**, the server returns `501 Not Implemented`

### RTSP Backchannel Issues

#### Issue: Backchannel Not Working
```
[D] RTSP: No backchannel support detected
```

**Causes:**
- Client not sending required ONVIF header
- SDP not including backchannel streams
- RTSP setup missing backchannel tracks

**Solutions:**
1. **Verify client sends ONVIF header**:
   ```
   Require: www.onvif.org/ver20/backchannel
   ```

2. **Check SDP includes backchannel streams**:
   ```bash
   journalctl -f | grep "Adding ONVIF audio backchannel streams"
   ```

3. **Supported backchannel tracks**:
   - `/G711_audiobackchannel` - G.711 PCMU
   - `/G726_audiobackchannel` - G.726-16

### Configuration Issues

#### Issue: ONVIF Module Not Starting
```
[E] ONVIF module not initialized
```

**Causes:**
- Module disabled in configuration
- Initialization failure
- Missing dependencies

**Solutions:**
1. **Check ONVIF configuration**:
   ```json
   {
     "enabled": true
   }
   ```

2. **Verify module registration**:
   ```bash
   grep "ONVIF module registered" /var/log/messages
   ```

3. **Check buildroot compilation**:
   ```bash
   grep "ENABLE_ONVIF=1" /tmp/thingino-streamer-build.log
   ```

### Client-Specific Issues

#### TinyCam Monitor Pro

**Issue: Connection Timeout**
- **Solution**: Ensure all required services are implemented
- **Check**: Device, Media, Imaging, and PTZ services responding

**Issue: No Video Stream**
- **Solution**: Verify RTSP server is running and accessible
- **Check**: GetStreamUri returns correct RTSP URL

#### ONVIF Device Manager

**Issue: Discovery Fails**
- **Solution**: Implement WS-Discovery service (future enhancement)
- **Workaround**: Add device manually by IP address

**Issue: Authentication Required**
- **Solution**: Use HTTP Basic Authentication instead of WS-Security
- **Configure**: Client to send Basic Auth headers

### Debug Commands

#### Enable Debug Logging
```bash
# View all ONVIF-related logs
journalctl -f | grep -E "(ONVIF|AUTH_UTILS|HTTP_ROUTER)"

# View authentication flow
journalctl -f | grep "AUTH_UTILS"

# View HTTP routing
journalctl -f | grep "HTTP_ROUTER"

# View RTSP backchannel
journalctl -f | grep "backchannel"
```

#### Test ONVIF Endpoints
```bash
# Test device service
curl -X POST http://192.168.1.121:8080/onvif/device_service \
  -H "Content-Type: application/soap+xml" \
  -d '<soap:Envelope>...</soap:Envelope>'

# Test with authentication
curl -X POST http://192.168.1.121:8080/onvif/device_service \
  -u thingino:thingino \
  -H "Content-Type: application/soap+xml" \
  -d '<soap:Envelope>...</soap:Envelope>'
```

#### Check Service Status
```bash
# Verify streamer is running
ps aux | grep streamer

# Check listening ports
netstat -tlnp | grep -E "(8080|554)"

# Test HTTP server
curl -I http://192.168.1.121:8080/status.json
```

### Performance Troubleshooting

#### High Memory Usage
- **Monitor**: ONVIF module uses ~500KB base memory
- **Check**: Large SOAP responses may increase memory temporarily
- **Solution**: Restart streamer if memory usage grows excessively

#### Slow Response Times
- **Cause**: Complex SOAP parsing and XML generation
- **Monitor**: Response times should be <100ms for most operations
- **Solution**: Optimize SOAP response caching (future enhancement)

### Log Analysis

#### Normal Operation Logs
```
[I] ONVIF module initialized successfully
[I] HTTP_MODULE: Authenticated request from 192.168.1.127
[D] HTTP_ROUTER: Dispatching POST /onvif/device_service to onvif
[I] ONVIF_SERVICES: Authenticated ONVIF request from 192.168.1.127
```

#### Error Patterns to Watch
```
[E] Failed to register ONVIF routes
[W] Unsupported ONVIF action
[E] ONVIF module not initialized
[W] 404 - Unknown path in request
```

### Getting Help

#### Information to Collect
1. **Streamer logs** with debug enabled
2. **Client application** and version
3. **Network configuration** (IP addresses, ports)
4. **ONVIF configuration** file contents
5. **SOAP request/response** samples (if available)

#### Reporting Issues
- Include full error messages and context
- Provide client application details
- Share relevant configuration files
- Include network packet captures if possible
