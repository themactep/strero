# Snapshot Fallback System

The Thingino Streamer includes an intelligent snapshot fallback system that automatically saves JPEG snapshots to the filesystem when the HTTP module is not available, ensuring continuous snapshot access for external applications.

## Overview

The snapshot fallback system provides automatic JPEG snapshot generation to `/tmp/` directory when:
- HTTP module is not compiled into the build
- HTTP module is disabled in configuration
- External WebUI packages need independent snapshot access

### Key Features

- **Automatic Activation** - Detects HTTP module availability and activates only when needed
- **Multi-Channel Support** - Captures snapshots from all enabled video channels
- **Configurable Intervals** - Adjustable capture frequency (default: 5 seconds)
- **File Management** - Automatic cleanup and overwrite capabilities
- **Zero Interference** - Operates independently without affecting other modules
- **Resource Efficient** - Minimal CPU and memory usage

## Architecture

### Activation Logic

```
┌─────────────────┐    ┌──────────────────┐    ┌─────────────────┐
│ HTTP Module     │    │ Snapshot         │    │ Result          │
│ Status          │    │ Fallback Config  │    │                 │
├─────────────────┤    ├──────────────────┤    ├─────────────────┤
│ ✅ Available    │ +  │ ✅ Enabled       │ =  │ HTTP serves     │
│ ❌ Not Available│ +  │ ✅ Enabled       │ =  │ Fallback active │
│ Any Status      │ +  │ ❌ Disabled      │ =  │ No snapshots    │
└─────────────────┘    └──────────────────┘    └─────────────────┘
```

### File Output Structure

```
/tmp/
├── snap0.jpg    # Channel 0 snapshot (main stream)
├── snap1.jpg    # Channel 1 snapshot (sub stream)
├── snap2.jpg    # Channel 2 snapshot (if enabled)
└── snap3.jpg    # Channel 3 snapshot (if enabled)
```

## Configuration

### Basic Configuration

Add to `/etc/streamer.json`:

```json
{
  "snapshot_fallback": {
    "enabled": true,
    "output_dir": "/tmp",
    "update_interval_ms": 5000,
    "overwrite_existing": true,
    "max_file_age_seconds": 3600
  }
}
```

### Configuration Parameters

| Parameter             | Type    | Default | Description                                        |
|-----------------------|---------|---------|----------------------------------------------------|
| `enabled`             | boolean | `true`  | Enable/disable snapshot fallback system            |
| `output_dir`          | string  | `"/tmp"`| Directory to save snapshot files                   |
| `update_interval_ms`  | integer | `5000`  | Capture interval in milliseconds                   |
| `overwrite_existing`  | boolean | `true`  | Overwrite existing snapshot files                  |
| `max_file_age_seconds`| integer | `3600`  | Maximum file age before cleanup (0 = no cleanup)   |

### Quick Configuration Commands

```bash
# Enable snapshot fallback
jct /etc/streamer.json set snapshot_fallback.enabled true

# Change capture interval to 10 seconds
jct /etc/streamer.json set snapshot_fallback.update_interval_ms 10000

# Change output directory
jct /etc/streamer.json set snapshot_fallback.output_dir "/var/snapshots"

# Disable automatic cleanup
jct /etc/streamer.json set snapshot_fallback.max_file_age_seconds 0
```

## Integration with External Applications

### WebUI Package Integration

The snapshot fallback system is designed to work with external WebUI packages running on port 80:

```bash
# WebUI can serve snapshots directly
GET /snapshot/0  -> serves /tmp/snap0.jpg
GET /snapshot/1  -> serves /tmp/snap1.jpg

# Or access files directly
curl http://127.0.0.1:80/tmp/snap0.jpg
```

### File Access Patterns

**Direct File Access:**
```bash
# Check if snapshots are available
ls -la /tmp/snap*.jpg

# View latest snapshot
cat /tmp/snap0.jpg > latest_snapshot.jpg

# Monitor file updates
watch -n 1 'ls -la /tmp/snap*.jpg'
```

**Programmatic Access:**
```c
// Check if snapshot exists
struct stat st;
if (stat("/tmp/snap0.jpg", &st) == 0) {
    printf("Snapshot size: %ld bytes\n", st.st_size);
    printf("Last modified: %s", ctime(&st.st_mtime));
}
```

## Performance Characteristics

### Resource Usage

- **Memory**: ~50KB during capture, minimal at rest
- **CPU**: <1% average usage with 5-second intervals
- **Disk I/O**: ~100KB write every 5 seconds per channel
- **Network**: Zero network usage

### File Characteristics

| Channel   | Typical Size | Resolution | Quality    |
|-----------|--------------|------------|------------|
| Channel 0 | 70-100KB     | 1920x1080  | High       |
| Channel 1 | 15-25KB      | 640x360    | Medium     |
| Channel 2+| Varies       | Configured | Configured |

### Timing Behavior

```
Timeline: 0s ----5s----10s----15s----20s
Channel 0: [📸]----[📸]----[📸]----[📸]
Channel 1:   [📸]----[📸]----[📸]----[📸]
Files:     snap0  snap1  snap0  snap1
```

## Troubleshooting

### Common Issues

#### Snapshots Not Generated

**Check system status:**
```bash
# Verify fallback is running
tail -f /var/log/streamer.log | grep SNAPSHOT_FALLBACK

# Check configuration
jct /etc/streamer.json get snapshot_fallback

# Verify output directory
ls -la /tmp/snap*.jpg
```

**Common causes:**
- HTTP module is available (fallback not needed)
- Snapshot fallback disabled in configuration
- Output directory not writable
- No enabled video channels

#### File Permission Issues

```bash
# Check directory permissions
ls -ld /tmp

# Fix permissions if needed
chmod 755 /tmp
chown root:root /tmp
```

#### Disk Space Issues

```bash
# Check available space
df -h /tmp

# Manual cleanup if needed
find /tmp -name "snap*.jpg" -mtime +1 -delete
```

### Debug Information

**Enable debug logging:**
```bash
# Add to streamer configuration
jct /etc/streamer.json set logging.level "DEBUG"

# Restart streamer
service streamer restart

# Monitor logs
tail -f /var/log/streamer.log | grep SNAPSHOT_FALLBACK
```

**Expected log messages:**
```
[I] SNAPSHOT_FALLBACK: Snapshot fallback initialized (dir=/tmp, interval=5000ms)
[I] SNAPSHOT_FALLBACK: HTTP module not available, starting snapshot fallback
[I] SNAPSHOT_FALLBACK: Snapshot fallback worker thread started
[D] SNAPSHOT_FALLBACK: Capturing snapshot for channel 0 to /tmp/snap0.jpg
[I] SNAPSHOT_FALLBACK: Snapshot saved: /tmp/snap0.jpg (76718 bytes)
```

## Advanced Configuration

### Custom Output Directory

```json
{
  "snapshot_fallback": {
    "enabled": true,
    "output_dir": "/var/www/html/snapshots",
    "update_interval_ms": 2000,
    "max_file_age_seconds": 1800
  }
}
```

### High-Frequency Capture

```json
{
  "snapshot_fallback": {
    "enabled": true,
    "update_interval_ms": 1000,
    "max_file_age_seconds": 300
  }
}
```

### Production Deployment

```json
{
  "snapshot_fallback": {
    "enabled": true,
    "output_dir": "/var/snapshots",
    "update_interval_ms": 10000,
    "overwrite_existing": true,
    "max_file_age_seconds": 7200
  }
}
```

## API Reference

### Manual Snapshot Capture

The system provides programmatic access for manual snapshot capture:

```c
#include "snapshot_fallback.h"

// Trigger immediate snapshot for channel 0
int result = snapshot_fallback_capture_now(0);
if (result == 0) {
    printf("Snapshot captured successfully\n");
}

// Get path to latest snapshot
char path[320];
if (snapshot_fallback_get_latest_path(0, path) == 0) {
    printf("Latest snapshot: %s\n", path);
}

// Check if system is running
if (snapshot_fallback_is_running()) {
    printf("Snapshot fallback is active\n");
}
```

### System Status

```c
// Check if HTTP module is available
bool http_available = snapshot_fallback_is_http_available();

// Cleanup old files manually
int cleaned = snapshot_fallback_cleanup_old_files();
printf("Cleaned %d old snapshot files\n", cleaned);
```

## Security Considerations

### File Access

- Snapshot files are created with standard permissions (644)
- Output directory should be readable by WebUI process
- Consider using dedicated snapshot user/group for production

### Disk Usage

- Monitor disk space in output directory
- Configure appropriate `max_file_age_seconds` for cleanup
- Consider logrotate-style management for high-frequency capture

### Network Exposure

- Snapshots in `/tmp` are not directly network-accessible
- WebUI package controls network exposure
- Consider firewall rules for WebUI port access

## Implementation Details

The snapshot fallback system is implemented as a core component:

- **`src/snapshot_fallback.h`** - API definitions and configuration structures
- **`src/snapshot_fallback.c`** - Core implementation with IMP encoder integration
- **Integration** - Automatic lifecycle management in main application
- **Configuration** - Integrated with main configuration system

For technical implementation details, see the source code and `TECHNICAL_IMPLEMENTATION.md`.
