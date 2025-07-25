# SSL Certificate Generation for Thingino Streamer

This document describes the automatic SSL certificate generation feature for Thingino Streamer's TLS-enabled protocols (RTSPS and RTMPS).

## Overview

When TLS support is enabled in the buildroot configuration, the Thingino Streamer package will automatically:

1. **Generate SSL certificates during package installation** if TLS backends are enabled
2. **Check and generate certificates at service startup** if they're missing
3. **Provide tools for manual certificate management**

## Automatic Certificate Generation

### During Package Installation

When building with TLS support enabled (`BR2_PACKAGE_THINGINO_STREAMER_RTSPS=y` or TLS backends enabled), the buildroot package will automatically generate self-signed SSL certificates during installation.

**Generated files:**
- Certificate: `/etc/ssl/certs/rtsp-server.crt`
- Private Key: `/etc/ssl/private/rtsp-server.key`

**Certificate details:**
- Algorithm: RSA 2048-bit
- Validity: 10 years (3650 days)
- Subject: `/C=US/ST=State/L=City/O=Thingino/CN=camera.local`

### During Service Startup

The S95streamer init script automatically checks for SSL certificates when starting:

1. **Checks if TLS is enabled** in `/etc/streamer.d/rtsp.json`
2. **Verifies certificate files exist** and are readable
3. **Generates missing certificates** using the certificate generation script
4. **Warns about expiring certificates** (within 30 days)

## Manual Certificate Management

### Generate New Certificates

Use the included certificate generation script:

```bash
# Generate with defaults
generate-ssl-certs.sh

# Generate with custom parameters
generate-ssl-certs.sh --common-name camera.example.com --days 365

# Force overwrite existing certificates
generate-ssl-certs.sh --force

# Quiet mode (minimal output)
generate-ssl-certs.sh --quiet
```

### Script Options

```
-c, --cert-file PATH     Certificate file path (default: /etc/ssl/certs/rtsp-server.crt)
-k, --key-file PATH      Private key file path (default: /etc/ssl/private/rtsp-server.key)
-d, --days DAYS          Certificate validity in days (default: 3650)
-s, --key-size SIZE      RSA key size in bits (default: 2048)
-n, --common-name CN     Common name for certificate (default: camera.local)
--country CODE           Country code (default: US)
--state STATE            State/Province (default: State)
--city CITY              City/Locality (default: City)
--org ORG                Organization (default: Thingino)
-f, --force              Overwrite existing certificates
-q, --quiet              Quiet mode (minimal output)
-h, --help               Show help message
```

### Check Certificate Status

```bash
# View certificate details
openssl x509 -in /etc/ssl/certs/rtsp-server.crt -noout -text

# Check certificate validity
openssl x509 -in /etc/ssl/certs/rtsp-server.crt -noout -dates

# Verify certificate and key match
openssl x509 -noout -modulus -in /etc/ssl/certs/rtsp-server.crt | openssl md5
openssl rsa -noout -modulus -in /etc/ssl/private/rtsp-server.key | openssl md5
```

## Configuration

### RTSPS Configuration

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

### RTMPS Configuration

RTMPS uses the same certificates for secure RTMP streaming to platforms like YouTube Live, Twitch, etc.

## Security Considerations

### Self-Signed Certificates

The automatically generated certificates are **self-signed** and suitable for:
- **Testing and development**
- **Internal networks** where certificate warnings can be accepted
- **IoT devices** where custom CA infrastructure is not practical

### Production Deployment

For production use, consider:

1. **Custom CA certificates** signed by your organization's CA
2. **Commercial certificates** from trusted Certificate Authorities
3. **Let's Encrypt certificates** for internet-facing devices

### Certificate Replacement

To use custom certificates:

1. **Replace the generated files**:
   ```bash
   cp your-certificate.crt /etc/ssl/certs/rtsp-server.crt
   cp your-private-key.key /etc/ssl/private/rtsp-server.key
   chmod 644 /etc/ssl/certs/rtsp-server.crt
   chmod 600 /etc/ssl/private/rtsp-server.key
   ```

2. **Restart the streamer**:
   ```bash
   /etc/init.d/S95streamer restart
   ```

## Troubleshooting

### Certificate Generation Fails

**Symptoms**: No certificates generated, TLS connections fail

**Solutions**:
1. Check if OpenSSL is installed: `which openssl`
2. Verify disk space: `df -h /etc/ssl`
3. Check permissions: `ls -la /etc/ssl/`
4. Run manually: `generate-ssl-certs.sh --force`

### TLS Connections Fail

**Symptoms**: RTSPS clients cannot connect

**Solutions**:
1. Verify certificates exist: `ls -la /etc/ssl/certs/rtsp-server.crt /etc/ssl/private/rtsp-server.key`
2. Check certificate validity: `openssl x509 -in /etc/ssl/certs/rtsp-server.crt -noout -dates`
3. Test TLS port: `openssl s_client -connect camera.local:322`
4. Check streamer logs for TLS errors

### Certificate Warnings in Clients

**Symptoms**: Clients show certificate warnings

**Solutions**:
1. **Accept the self-signed certificate** in client settings
2. **Add certificate to client's trusted store**
3. **Use custom certificates** signed by a trusted CA
4. **Configure client to skip certificate verification** (testing only)

## Integration with Buildroot

The SSL certificate generation is integrated into the buildroot package system:

- **Dependencies**: Automatically adds OpenSSL dependency when TLS is enabled
- **Installation**: Certificates generated during `make thingino-streamer`
- **Configuration**: Works with existing TLS configuration options
- **Compatibility**: Supports both OpenSSL and mbedTLS backends

## Related Documentation

- [RTSPS Quick Reference](../../../docs/RTSPS_QUICK_REFERENCE.md)
- [RTMP Client Documentation](../../../docs/RTMP_CLIENT.md)
- [Buildroot Package Documentation](../README.md)
