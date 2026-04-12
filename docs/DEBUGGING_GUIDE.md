# Thingino Streamer Debugging and Troubleshooting Guide

## Overview

This guide documents debugging techniques, common issues, and troubleshooting procedures discovered during the Thingino Streamer conversion. These methods are essential for embedded development where traditional debugging tools may be limited.

## Table of Contents

1. [Debug Build Configuration](#debug-build-configuration)
2. [Remote GDB Debugging](#remote-gdb-debugging)
3. [Common Issues and Solutions](#common-issues-and-solutions)
4. [Performance Analysis](#performance-analysis)
5. [Network Debugging](#network-debugging)
6. [Hardware-Specific Issues](#hardware-specific-issues)
7. [Logging Strategies](#logging-strategies)

## Debug Build Configuration

### Buildroot Package Modification

To enable debugging, modify the buildroot package configuration:

```makefile
# In package/thingino-streamer/thingino-streamer.mk

# Debug build flags
THINGINO_STREAMER_CFLAGS += \
    -DNO_OPENSSL=1 -g -O0 \
    -I$(STAGING_DIR)/usr/include \
    # ... other includes

# Disable symbol stripping
define THINGINO_STREAMER_BUILD_CMDS
    $(MAKE) \
        ARCH=$(TARGET_ARCH) \
        CROSS_COMPILE=$(TARGET_CROSS) \
        CFLAGS="$(THINGINO_STREAMER_CFLAGS)" \
        LDFLAGS="$(THINGINO_STREAMER_LDFLAGS)" \
        DEBUG_STRIP=0 \
        -C $(@D) all
endef
```

### Makefile Debug Support

```makefile
# In main Makefile
STRIP_FLAG := $(if $(filter 0,$(DEBUG_STRIP)),,"-s")

$(TARGET): $(OBJECTS)
    $(CC) $(LDFLAGS) -o $@ $(OBJECTS) $(LIBS) $(STRIP_FLAG)
```

**Key Points**:
- `-g`: Include debug symbols
- `-O0`: Disable optimizations for accurate debugging
- `DEBUG_STRIP=0`: Prevent symbol stripping
- Result: Binary with full debugging information

## Remote GDB Debugging

### Setup Process

1. **Build with debug symbols** (see above)
2. **Deploy to target device** via NFS or SCP
3. **Start gdbserver on target**:
   ```bash
   gdbserver :1234 /path/to/streamer
   ```
4. **Connect from host**:
   ```bash
   /path/to/cross-gdb streamer
   (gdb) target remote 192.168.1.109:1234
   ```

### Essential GDB Commands

```bash
# Set breakpoints
(gdb) break process_encoded_frame
(gdb) break main.c:779

# Watch variables (limited on MIPS)
(gdb) watch timestamp.tv_sec
(gdb) watch timestamp.tv_usec

# Examine memory
(gdb) print timestamp
(gdb) print &timestamp
(gdb) x/16x &timestamp

# Step through code
(gdb) next      # Next line
(gdb) step      # Step into functions
(gdb) continue  # Continue execution

# Examine call stack
(gdb) backtrace
(gdb) frame 1
```

### Hardware Watchpoint Limitations

**Issue**: MIPS hardware has limited watchpoint support
**Solution**: Use software breakpoints and manual inspection

```bash
# Instead of hardware watchpoints
(gdb) watch variable

# Use breakpoints with manual checks
(gdb) break function_name
(gdb) commands
> print variable
> continue
> end
```

## Common Issues and Solutions

### 1. Timestamp Corruption Bug

**Symptoms**:
- Debug output shows incorrect timestamp values
- GDB shows correct values but printf shows wrong values
- Streaming works but debug logs are confusing

**Root Cause**: Variable naming collision

**Solution**:
```c
// Before (problematic)
struct timeval timestamp;

// After (fixed)
struct timeval frame_timestamp;
```

**Debugging Process**:
1. Set breakpoints at timestamp calculation
2. Verify values in GDB are correct
3. Check printf output for discrepancies
4. Look for variable naming conflicts

### 2. Startup Delay Issues

**Symptoms**:
- 20+ second delay before first frame
- Encoder appears to initialize but no output

**Root Cause**: Encoder not started immediately after setup

**Solution**:
```c
// Start encoder immediately after channel creation
setup_encoder_channels();
IMP_Encoder_StartRecvPic(0);  // Critical: start immediately
```

**Debugging**:
- Add timestamps to initialization sequence
- Monitor IMP encoder state
- Check for blocking operations in startup path

### 3. Frame Rate Issues

**Symptoms**:
- Inconsistent frame timing
- Dropped frames
- Client buffering

**Root Cause**: Incorrect timestamp calculation

**Solution**:
```c
// Ensure consistent 40ms intervals for 25fps
uint64_t total_usec = (uint64_t)frame_count * 40000ULL;
frame_timestamp.tv_sec = (long)(total_usec / 1000000ULL);
frame_timestamp.tv_usec = (long)(total_usec % 1000000ULL);
```

**Debugging**:
- Log timestamp values for consecutive frames
- Verify 40ms intervals in debug output
- Check RTP timestamp progression

### 4. Memory Corruption

**Symptoms**:
- Segmentation faults
- Unexpected variable values
- Intermittent crashes

**Debugging Techniques**:
```bash
# Use Valgrind (if available on target)
valgrind --tool=memcheck ./streamer

# Add memory debugging
void* debug_malloc(size_t size, const char* file, int line) {
    void* ptr = malloc(size);
    printf("MALLOC: %p size=%zu at %s:%d\n", ptr, size, file, line);
    return ptr;
}

#define malloc(size) debug_malloc(size, __FILE__, __LINE__)
```

**Prevention**:
- Always initialize structures with `memset()`
- Use consistent cleanup patterns
- Validate pointers before use

### 5. Network Connection Issues

**Symptoms**:
- RTSP clients cannot connect
- Connection drops during streaming
- Intermittent network errors

**Debugging**:
```bash
# Check network connectivity
ping 192.168.1.109

# Test RTSP handshake
telnet 192.168.1.109 554

# Monitor network traffic
tcpdump -i eth0 port 554

# Test with different clients
ffplay rtsp://192.168.1.109/ch0
mpv rtsp://192.168.1.109/ch0
```

**Common Solutions**:
- Verify firewall settings
- Check network interface configuration
- Ensure RTSP port (554) is available

## Performance Analysis

### Startup Time Measurement

```c
#include <time.h>

static void measure_startup_time(void) {
    static struct timespec start_time;
    static bool first_call = true;
    
    if (first_call) {
        clock_gettime(CLOCK_MONOTONIC, &start_time);
        first_call = false;
        printf("STARTUP: Timer started\n");
    } else {
        struct timespec current_time;
        clock_gettime(CLOCK_MONOTONIC, &current_time);
        
        long elapsed_ms = (current_time.tv_sec - start_time.tv_sec) * 1000 +
                         (current_time.tv_nsec - start_time.tv_nsec) / 1000000;
        
        printf("STARTUP: First frame at %ld ms\n", elapsed_ms);
    }
}
```

### Frame Rate Analysis

```c
static void analyze_frame_rate(void) {
    static uint32_t frame_count = 0;
    static struct timespec last_time = {0};
    struct timespec current_time;
    
    clock_gettime(CLOCK_MONOTONIC, &current_time);
    
    if (last_time.tv_sec != 0) {
        long interval_ms = (current_time.tv_sec - last_time.tv_sec) * 1000 +
                          (current_time.tv_nsec - last_time.tv_nsec) / 1000000;
        
        if (frame_count % 25 == 0) {  // Log every second at 25fps
            printf("FRAMERATE: Frame %u, interval=%ld ms\n", 
                   frame_count, interval_ms);
        }
    }
    
    last_time = current_time;
    frame_count++;
}
```

### Memory Usage Monitoring

```c
static void log_memory_usage(void) {
    FILE* status = fopen("/proc/self/status", "r");
    if (!status) return;
    
    char line[256];
    while (fgets(line, sizeof(line), status)) {
        if (strncmp(line, "VmRSS:", 6) == 0) {
            printf("MEMORY: %s", line);
            break;
        }
    }
    
    fclose(status);
}
```

## Network Debugging

### RTSP Protocol Analysis

```bash
# Capture RTSP traffic
tcpdump -i eth0 -s 0 -w rtsp_capture.pcap port 554

# Analyze with Wireshark
wireshark rtsp_capture.pcap
```

### RTP Stream Validation

```c
static void validate_rtp_stream(uint32_t timestamp, uint16_t seq) {
    static uint32_t last_timestamp = 0;
    static uint16_t last_seq = 0;
    static bool first_packet = true;
    
    if (!first_packet) {
        // Check sequence number continuity
        uint16_t expected_seq = last_seq + 1;
        if (seq != expected_seq) {
            printf("RTP: Sequence gap! Expected %u, got %u\n", 
                   expected_seq, seq);
        }
        
        // Check timestamp progression (should be 3600 for 25fps)
        uint32_t timestamp_diff = timestamp - last_timestamp;
        if (timestamp_diff != 3600) {
            printf("RTP: Timestamp gap! Expected 3600, got %u\n", 
                   timestamp_diff);
        }
    }
    
    last_timestamp = timestamp;
    last_seq = seq;
    first_packet = false;
}
```

### Client Compatibility Testing

```bash
# Test with multiple clients
ffplay -v debug -rtsp_transport tcp rtsp://192.168.1.109/ch0
mpv --log-level=debug rtsp://192.168.1.109/ch0
vlc -vvv rtsp://192.168.1.109/ch0

# Test UDP vs TCP transport
ffplay -rtsp_transport udp rtsp://192.168.1.109/ch0
ffplay -rtsp_transport tcp rtsp://192.168.1.109/ch0
```

## Hardware-Specific Issues

### T31 SoC Debugging

**IMP System Issues**:
```c
// Check IMP system status
int imp_status = IMP_System_GetVersion();
printf("IMP Version: 0x%x\n", imp_status);

// Verify encoder channel state
IMPEncoderChnStat stat;
int ret = IMP_Encoder_Query(0, &stat);
if (ret == 0) {
    printf("Encoder: registered=%d, leftPics=%u, leftBytes=%u\n",
           stat.registered, stat.leftPics, stat.leftBytes);
}
```

**Memory Constraints**:
```c
// Monitor available memory
static void check_memory_pressure(void) {
    FILE* meminfo = fopen("/proc/meminfo", "r");
    if (!meminfo) return;
    
    char line[256];
    unsigned long mem_free = 0, mem_available = 0;
    
    while (fgets(line, sizeof(line), meminfo)) {
        if (sscanf(line, "MemFree: %lu kB", &mem_free) == 1) {
            continue;
        }
        if (sscanf(line, "MemAvailable: %lu kB", &mem_available) == 1) {
            break;
        }
    }
    
    fclose(meminfo);
    
    if (mem_available < 10240) {  // Less than 10MB
        printf("WARNING: Low memory! Available: %lu kB\n", mem_available);
    }
}
```

## Logging Strategies

### Structured Logging System

```c
typedef enum {
    LOG_LEVEL_ERROR = 0,
    LOG_LEVEL_WARN = 1,
    LOG_LEVEL_INFO = 2,
    LOG_LEVEL_DEBUG = 3
} log_level_t;

static log_level_t current_log_level = LOG_LEVEL_INFO;

#define LOG_ERROR(fmt, ...) \
    log_message(LOG_LEVEL_ERROR, __FILE__, __LINE__, fmt, ##__VA_ARGS__)

#define LOG_DEBUG(fmt, ...) \
    do { \
        if (current_log_level >= LOG_LEVEL_DEBUG) { \
            log_message(LOG_LEVEL_DEBUG, __FILE__, __LINE__, fmt, ##__VA_ARGS__); \
        } \
    } while(0)

static void log_message(log_level_t level, const char* file, int line,
                       const char* fmt, ...) {
    const char* level_str[] = {"ERROR", "WARN", "INFO", "DEBUG"};
    
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    
    printf("[%ld.%03ld] %s %s:%d: ",
           ts.tv_sec, ts.tv_nsec / 1000000,
           level_str[level], file, line);
    
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    
    printf("\n");
    fflush(stdout);
}
```

### Performance Logging

```c
#define PERF_LOG_ENABLED 1

#if PERF_LOG_ENABLED
#define PERF_START(name) \
    struct timespec perf_start_##name; \
    clock_gettime(CLOCK_MONOTONIC, &perf_start_##name);

#define PERF_END(name) \
    do { \
        struct timespec perf_end_##name; \
        clock_gettime(CLOCK_MONOTONIC, &perf_end_##name); \
        long perf_elapsed_##name = \
            (perf_end_##name.tv_sec - perf_start_##name.tv_sec) * 1000000 + \
            (perf_end_##name.tv_nsec - perf_start_##name.tv_nsec) / 1000; \
        printf("PERF: %s took %ld microseconds\n", #name, perf_elapsed_##name); \
    } while(0)
#else
#define PERF_START(name)
#define PERF_END(name)
#endif

// Usage example
void process_frame(void) {
    PERF_START(frame_processing);
    
    // ... frame processing code ...
    
    PERF_END(frame_processing);
}
```

## Troubleshooting Checklist

### Before Starting Development
- [ ] Verify cross-compilation toolchain
- [ ] Test basic connectivity to target device
- [ ] Confirm NFS mount or file transfer method
- [ ] Validate target hardware specifications

### During Development
- [ ] Build with debug symbols for testing
- [ ] Use consistent logging throughout
- [ ] Test on actual hardware regularly
- [ ] Monitor memory usage and performance
- [ ] Validate network protocols with packet capture

### Before Deployment
- [ ] Test with multiple RTSP clients
- [ ] Verify performance under load
- [ ] Check memory leaks with extended runtime
- [ ] Validate startup time and stability
- [ ] Test network error recovery

### Common Debug Commands

```bash
# System information
cat /proc/cpuinfo
cat /proc/meminfo
free -h

# Process monitoring
top -p $(pgrep streamer)
ps aux | grep streamer

# Network status
netstat -tlnp | grep 554
ss -tlnp | grep streamer

# File system
df -h
mount | grep nfs

# Kernel messages
dmesg | tail -20
```

## Quick Reference

### Essential Debug Build Commands

```bash
# Build with debug symbols
./build.sh

# Or manually modify package
# Edit: package/thingino-streamer/thingino-streamer.mk
# Change: -Os to -g -O0
# Add: DEBUG_STRIP=0

# Deploy and debug
scp binary root@camera:/tmp/streamer-debug
ssh root@camera
gdbserver :1234 /tmp/streamer-debug

# From host
cross-gdb streamer
(gdb) target remote camera_ip:1234
```

### Critical Breakpoints

```bash
# Timestamp calculation
(gdb) break process_encoded_frame
(gdb) break main.c:779

# RTSP protocol
(gdb) break minimal_rtsp_server_send_frame
(gdb) break send_rtp_packet

# System initialization
(gdb) break main
(gdb) break setup_encoder_channels
```

### Performance Validation

```bash
# Startup time (should be ~2 seconds)
time_to_first_frame < 3000ms

# Frame rate (should be exactly 25fps)
frame_interval = 40ms ± 1ms

# Memory usage (should be stable)
VmRSS < 50MB for 128MB devices

# Network throughput
RTP packet rate = 25 packets/second
No sequence number gaps
Timestamp increment = 3600 per frame
```

---

*This debugging guide represents real-world experience debugging embedded streaming applications. Each technique has been tested and proven effective for troubleshooting complex issues in resource-constrained environments.*
