#!/bin/bash

# TLS Connection Debug Script for Thingino Streamer
# This script helps diagnose TLS handshake issues with RTSPS

CAMERA_IP="${1:-192.168.1.109}"
RTSP_PORT="${2:-322}"

echo "=== TLS Connection Debug for Thingino Streamer ==="
echo "Camera IP: $CAMERA_IP"
echo "RTSP Port: $RTSP_PORT"
echo ""

# Test 1: Basic connectivity
echo "1. Testing basic connectivity..."
if timeout 5 nc -z "$CAMERA_IP" "$RTSP_PORT" 2>/dev/null; then
    echo "✓ Port $RTSP_PORT is open and reachable"
else
    echo "✗ Port $RTSP_PORT is not reachable"
    exit 1
fi
echo ""

# Test 2: TLS handshake with OpenSSL s_client
echo "2. Testing TLS handshake with OpenSSL..."
if command -v openssl >/dev/null 2>&1; then
    echo "Attempting TLS handshake..."
    timeout 10 openssl s_client -connect "$CAMERA_IP:$RTSP_PORT" -servername camera.local -verify_return_error -brief 2>&1 | head -20
else
    echo "OpenSSL not available for testing"
fi
echo ""

# Test 3: Check what TLS versions are supported
echo "3. Testing different TLS versions..."
if command -v openssl >/dev/null 2>&1; then
    for version in tls1 tls1_1 tls1_2 tls1_3; do
        echo -n "Testing $version: "
        if timeout 5 openssl s_client -connect "$CAMERA_IP:$RTSP_PORT" -$version -quiet 2>/dev/null </dev/null | grep -q "CONNECTED"; then
            echo "✓ Supported"
        else
            echo "✗ Not supported or failed"
        fi
    done
else
    echo "OpenSSL not available for version testing"
fi
echo ""

# Test 4: Try with curl (if available)
echo "4. Testing with curl..."
if command -v curl >/dev/null 2>&1; then
    echo "Attempting HTTPS connection..."
    timeout 10 curl -v -k "https://$CAMERA_IP:$RTSP_PORT/" 2>&1 | head -15
else
    echo "curl not available for testing"
fi
echo ""

# Test 5: Check certificate details (if we can get them)
echo "5. Checking certificate details..."
if command -v openssl >/dev/null 2>&1; then
    echo "Retrieving certificate information..."
    timeout 10 openssl s_client -connect "$CAMERA_IP:$RTSP_PORT" -showcerts 2>/dev/null </dev/null | openssl x509 -noout -text 2>/dev/null | head -20
else
    echo "OpenSSL not available for certificate inspection"
fi
echo ""

# Test 6: Try with different cipher suites
echo "6. Testing cipher compatibility..."
if command -v openssl >/dev/null 2>&1; then
    # Test with common cipher suites
    for cipher in "ECDHE-RSA-AES256-GCM-SHA384" "ECDHE-RSA-AES128-GCM-SHA256" "AES256-SHA" "AES128-SHA"; do
        echo -n "Testing cipher $cipher: "
        if timeout 5 openssl s_client -connect "$CAMERA_IP:$RTSP_PORT" -cipher "$cipher" -quiet 2>/dev/null </dev/null | grep -q "CONNECTED"; then
            echo "✓ Works"
        else
            echo "✗ Failed"
        fi
    done
else
    echo "OpenSSL not available for cipher testing"
fi
echo ""

echo "=== Debug completed ==="
echo ""
echo "If TLS handshake is failing:"
echo "1. Check if certificates exist on camera: /etc/ssl/certs/rtsp-server.crt"
echo "2. Verify certificate format (should be PEM)"
echo "3. Check server logs for detailed error messages"
echo "4. Try regenerating certificates with: generate-ssl-certs.sh --force"
