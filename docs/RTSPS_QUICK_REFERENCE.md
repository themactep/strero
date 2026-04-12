# RTSPS Quick Reference

RTSPS (RTSP over TLS) provides secure, encrypted video streaming from Thingino cameras.

## Quick Setup

### 1. Generate Certificates

```bash
# Create directories
mkdir -p /etc/ssl/certs /etc/ssl/private

# Generate self-signed certificate (testing)
openssl req -x509 -newkey rsa:2048 -keyout /etc/ssl/private/rtsp-server.key \
            -out /etc/ssl/certs/rtsp-server.crt -days 365 -nodes \
            -subj "/C=US/ST=State/L=City/O=Thingino/CN=camera.local"

# Set permissions
chmod 600 /etc/ssl/private/rtsp-server.key
chmod 644 /etc/ssl/certs/rtsp-server.crt
```

### 2. Configure RTSPS

Edit `/etc/streamer.d/rtsp.json`:

```json
{
  "enabled": true,
  "port": 554,
  "tls_enabled": true,
  "tls_port": 322,
  "cert_file": "/etc/ssl/certs/rtsp-server.crt",
  "key_file": "/etc/ssl/private/rtsp-server.key",
  "tls_verify_client": false
}
```

### 3. Restart Streamer

```bash
/etc/init.d/S95streamer restart
```

## Client Connections

### VLC
```bash
# GUI: Open Network Stream
rtsps://192.168.1.109:322/ch0

# Command line
vlc rtsps://192.168.1.109:322/ch0
```

### FFmpeg/FFplay
```bash
# Standard connection
ffplay rtsps://192.168.1.109:322/ch0

# Ignore certificate verification (self-signed)
ffplay -tls_verify 0 rtsps://192.168.1.109:322/ch0

# With advanced options
ffplay -rtsp_flags prefer_tcp -fflags nobuffer -tls_verify 0 rtsps://192.168.1.109:322/ch0
```

### mpv
```bash
# Standard connection
mpv rtsps://192.168.1.109:322/ch0

# Ignore certificate verification
mpv --tls-verify=no rtsps://192.168.1.109:322/ch0
```

## Stream Endpoints

- **Main Stream**: `rtsps://IP:322/ch0` (high resolution)
- **Sub Stream**: `rtsps://IP:322/ch1` (low resolution)

## Authentication

With authentication enabled:

```bash
# URL format
rtsps://username:password@192.168.1.109:322/ch0

# Example
rtsps://thingino:thingino@192.168.1.109:322/ch0
```

## Troubleshooting

### Check Server Status
```bash
# Verify streamer is running
ps aux | grep streamer

# Check listening ports
netstat -ln | grep -E ':(554|322)'

# View logs
logread | grep RTSP
```

### Test Connectivity
```bash
# Test RTSP port
telnet 192.168.1.109 554

# Test RTSPS port
telnet 192.168.1.109 322

# Test TLS handshake
openssl s_client -connect 192.168.1.109:322 -servername camera.local
```

### Certificate Issues
```bash
# Check certificate validity
openssl x509 -in /etc/ssl/certs/rtsp-server.crt -text -noout

# Verify certificate and key match
openssl x509 -noout -modulus -in /etc/ssl/certs/rtsp-server.crt | openssl md5
openssl rsa -noout -modulus -in /etc/ssl/private/rtsp-server.key | openssl md5
```

### Common Errors

#### "Connection refused"
- Check if RTSPS is enabled in configuration
- Verify port 322 is not blocked by firewall
- Ensure streamer service is running

#### "TLS handshake failed"
- Check certificate file permissions
- Verify certificate is valid and not expired
- For self-signed certificates, use `-tls_verify 0`

#### "Certificate verification failed"
- Use `-tls_verify 0` for self-signed certificates
- Install CA certificate in client system
- Check certificate hostname matches connection IP

## Configuration Options

### Basic RTSPS
```json
{
  "tls_enabled": true,
  "tls_port": 322,
  "cert_file": "/etc/ssl/certs/rtsp-server.crt",
  "key_file": "/etc/ssl/private/rtsp-server.key"
}
```

### With Client Verification
```json
{
  "tls_enabled": true,
  "tls_port": 322,
  "cert_file": "/etc/ssl/certs/rtsp-server.crt",
  "key_file": "/etc/ssl/private/rtsp-server.key",
  "tls_verify_client": true
}
```

### Custom Port
```json
{
  "tls_enabled": true,
  "tls_port": 8322,
  "cert_file": "/etc/ssl/certs/rtsp-server.crt",
  "key_file": "/etc/ssl/private/rtsp-server.key"
}
```

## Security Best Practices

### Certificate Management
- Use CA-signed certificates in production
- Regularly rotate certificates (annually)
- Secure private key file permissions (600)
- Store certificates in secure locations

### Network Security
- Use RTSPS over untrusted networks
- Implement firewall rules to restrict access
- Consider VPN for additional security layer
- Monitor connection logs for suspicious activity

### Access Control
- Enable authentication for sensitive deployments
- Use strong passwords
- Limit concurrent client connections
- Implement session timeouts

## Performance Notes

- **RTSPS Overhead**: ~10-15% CPU increase for encryption
- **Memory Usage**: +100KB per TLS connection
- **Latency**: Minimal impact (<50ms additional)
- **Bandwidth**: No significant increase

## Production Deployment

### Certificate Authority Setup
```bash
# Generate CA private key
openssl genrsa -out ca.key 4096

# Generate CA certificate
openssl req -new -x509 -days 3650 -key ca.key -out ca.crt \
            -subj "/C=US/ST=State/L=City/O=Company/CN=Company CA"

# Generate server private key
openssl genrsa -out server.key 2048

# Generate certificate signing request
openssl req -new -key server.key -out server.csr \
            -subj "/C=US/ST=State/L=City/O=Company/CN=camera.company.com"

# Sign server certificate with CA
openssl x509 -req -days 365 -in server.csr -CA ca.crt -CAkey ca.key \
             -CAcreateserial -out server.crt
```

### Automated Certificate Renewal
```bash
#!/bin/bash
# /etc/cron.monthly/renew-rtsp-cert

# Renew certificate (example with Let's Encrypt)
certbot renew --quiet

# Copy new certificates
cp /etc/letsencrypt/live/camera.company.com/fullchain.pem /etc/ssl/certs/rtsp-server.crt
cp /etc/letsencrypt/live/camera.company.com/privkey.pem /etc/ssl/private/rtsp-server.key

# Set permissions
chmod 644 /etc/ssl/certs/rtsp-server.crt
chmod 600 /etc/ssl/private/rtsp-server.key

# Restart streamer
/etc/init.d/S95streamer restart
```

## Related Documentation

- [RTSP Server Documentation](RTSP_SERVER.md)
- [RTSP Client Compatibility](RTSP_CLIENTS.md)
- [RTSP Protocol Flow](RTSP_FLOW.md)
