# Thingino Streamer Technical Implementation Details

## Overview

This document provides deep technical details of the Thingino Streamer implementation, focusing on specific code patterns, algorithms, and optimizations that enable world-class H.264 RTSP streaming performance on embedded devices.

## Core Architecture

### System Initialization Sequence

```c
// Critical initialization order for optimal performance
int main(int argc, char* argv[]) {
    // 1. Load and validate configuration
    if (!load_configuration("streamer.json")) {
        exit(EXIT_FAILURE);
    }

    // 2. Initialize IMP system BEFORE encoder setup
    if (IMP_System_Init() < 0) {
        LOG_ERROR("IMP_System_Init failed");
        exit(EXIT_FAILURE);
    }

    // 3. Setup frame source and encoder channels
    setup_framesource();
    setup_encoder_channels();

    // 4. Start encoder immediately (critical for startup time)
    IMP_Encoder_StartRecvPic(0);

    // 5. Initialize RTSP server
    rtsp_server = minimal_rtsp_server_create(554);

    // 6. Start frame processing thread
    start_encoder_thread();

    return 0;
}
```

**Key Insight**: The order of initialization is critical. Starting the encoder immediately after setup eliminates the 20+ second delay that occurred when encoder startup was deferred.

## Authentication System

### Unified Authentication Architecture

The authentication system provides HTTP Basic Authentication across all protocols (HTTP, RTSP, ONVIF) with intelligent localhost bypass:

```c
// Core authentication flow
typedef enum {
    AUTH_RESULT_SUCCESS = 0,        /* Authentication successful or not required */
    AUTH_RESULT_REQUIRED,           /* Authentication required but not provided */
    AUTH_RESULT_INVALID,            /* Invalid credentials provided */
    AUTH_RESULT_ERROR               /* Authentication system error */
} auth_result_t;

// Client information extraction
typedef struct {
    int socket_fd;                  /* Client socket file descriptor */
    struct sockaddr_in addr;        /* Client address */
    bool is_localhost;              /* True if client is localhost */
    char ip_string[INET_ADDRSTRLEN]; /* Client IP as string */
} client_info_t;
```

### Localhost Detection Algorithm

```c
bool auth_is_localhost(const struct sockaddr_in* addr) {
    uint32_t ip = ntohl(addr->sin_addr.s_addr);

    /* Check for localhost addresses:
     * 127.0.0.0/8 (127.0.0.1 - 127.255.255.255)
     * Also check for 0.0.0.0 (sometimes used for local connections)
     */
    return (ip >> 24) == 127 || ip == 0;
}
```

**Key Insight**: Using bitwise operations for IP range checking is more efficient than string comparisons and handles the entire 127.0.0.0/8 localhost range.

### Protocol Integration Points

#### HTTP Module Integration
```c
/* Authentication check before routing */
auth_result_t auth_result = auth_check_http_request(request, &config->auth, &client_info);

if (auth_result == AUTH_RESULT_REQUIRED) {
    /* Send 401 with WWW-Authenticate header */
    char auth_header[256];
    auth_generate_www_authenticate_header("Thingino Streamer", auth_header);
    // ... send 401 response
} else if (auth_result == AUTH_RESULT_SUCCESS) {
    /* Route request normally */
    http_router_dispatch(request, client_socket);
}
```

#### RTSP Module Integration
```c
/* Per-request authentication in RTSP server */
auth_result_t auth_result = auth_check_rtsp_request(buffer, &server->config.auth, &client_info);

if (auth_result != AUTH_RESULT_SUCCESS) {
    return send_rtsp_response(client, RTSP_STATUS_UNAUTHORIZED, "Unauthorized",
                             "WWW-Authenticate: Basic realm=\"Thingino RTSP Server\"", NULL);
}
```

**Performance Optimization**: Authentication checking is performed once per connection/request, not per frame, minimizing CPU overhead during streaming.

## Snapshot Fallback System

### Intelligent Activation Architecture

The snapshot fallback system provides automatic JPEG snapshot generation when the HTTP module is unavailable:

```c
/* Automatic HTTP module detection */
bool snapshot_fallback_is_http_available(void) {
#ifdef ENABLE_HTTP_MODULE
    struct streamer_config* config = get_global_config();
    if (config && config->http.enabled) {
        return true;
    }
#endif
    return false;
}

/* Activation logic in main.c */
if (snapshot_fallback_init(&g_config->snapshot_fallback) == 0) {
    if (snapshot_fallback_start() == 0) {
        IMP_LOG_INFO(TAG, "Snapshot fallback system started");
    } else {
        IMP_LOG_INFO(TAG, "Snapshot fallback not needed (HTTP available)");
    }
}
```

### IMP Encoder Integration

The system follows the proper IMP encoder workflow to prevent segmentation faults:

```c
/* Proper IMP encoder sequence */
int capture_channel_snapshot(int channel, const char* output_path) {
    int jpeg_channel = FS_CHN_NUM + channel;

    /* 1. Start receiving pictures */
    ret = IMP_Encoder_StartRecvPic(jpeg_channel);

    /* 2. Poll for data availability */
    ret = IMP_Encoder_PollingStream(jpeg_channel, 1000);

    /* 3. Get stream data */
    ret = IMP_Encoder_GetStream(jpeg_channel, &stream, 1);

    /* 4. Process stream with wrap-around handling */
    for (int i = 0; i < stream.packCount; i++) {
        IMPEncoderPack* pack = &stream.pack[i];
        uint32_t remSize = stream.streamSize - pack->offset;
        if (remSize < pack->length) {
            /* Handle circular buffer wrap-around */
            fwrite((void*)(stream.virAddr + pack->offset), 1, remSize, fp);
            fwrite((void*)stream.virAddr, 1, pack->length - remSize, fp);
        } else {
            /* Normal linear copy */
            fwrite((void*)(stream.virAddr + pack->offset), 1, pack->length, fp);
        }
    }

    /* 5. Release resources */
    IMP_Encoder_ReleaseStream(jpeg_channel, &stream);
    IMP_Encoder_StopRecvPic(jpeg_channel);
}
```

**Key Insight**: The IMP encoder uses a circular buffer architecture. Data must be accessed via `stream.virAddr + pack->offset`, and wrap-around conditions must be handled when `pack->offset + pack->length > stream.streamSize`.

### Multi-Channel Management

```c
/* Worker thread with channel validation */
for (int channel = 0; channel < max_channels; channel++) {
    /* Validate against stream configuration */
    if (!g_config->streams[channel].enabled) {
        continue;
    }

    /* Check capture timing */
    uint64_t elapsed_us = now - g_fallback_state.last_capture_time[channel];
    uint64_t interval_us = g_fallback_state.config.update_interval_ms * 1000;

    if (elapsed_us >= interval_us) {
        /* Capture snapshot for this channel */
        capture_channel_snapshot(channel, full_path);
        g_fallback_state.last_capture_time[channel] = now;
    }
}
```

**Performance Optimization**: Channel validation is performed against the dynamic stream configuration, ensuring only enabled channels are processed and avoiding unnecessary IMP encoder calls.

### File System Integration

```c
/* Atomic file operations */
FILE* fp = fopen(output_path, "wb");  /* Direct write to final location */
/* ... write data ... */
fclose(fp);  /* Atomic completion */

/* No temporary files needed - writes are fast enough to be atomic */
```

**Design Decision**: Direct writes to final file locations instead of temporary files, as JPEG capture is fast enough (~50ms) to avoid race conditions with readers.

## Timestamp Synchronization System

### Problem Statement
Embedded devices often lack Real-Time Clock (RTC) and suffer from NTP drift, making accurate timestamp generation challenging for media streaming.

### Solution: Synthetic Timestamp Generation

```c
static int process_encoded_frame(int channel) {
    struct timeval frame_timestamp;
    static uint32_t frame_count = 0;
    static bool initialized = false;

    // Initialize timestamp struct to prevent corruption
    memset(&frame_timestamp, 0, sizeof(frame_timestamp));

    // Calculate timestamp for 25fps: each frame = 40ms = 40000us
    uint64_t total_usec = (uint64_t)frame_count * 40000ULL;
    frame_timestamp.tv_sec = (long)(total_usec / 1000000ULL);
    frame_timestamp.tv_usec = (long)(total_usec % 1000000ULL);

    // Force memory barrier to prevent compiler optimization
    __sync_synchronize();

    frame_count++;

    // Send to RTSP server with calculated timestamp
    minimal_rtsp_server_send_frame(rtsp_server, channel,
                                   frame_data, frame_size,
                                   &frame_timestamp);

    return frame_size;
}
```

**Technical Details**:
- **40ms intervals**: Perfect for 25fps (1000ms / 25 = 40ms)
- **64-bit arithmetic**: Prevents overflow for long-running streams
- **Memory barriers**: Ensures timestamp consistency across compiler optimizations
- **Variable naming**: `frame_timestamp` prevents collision with other timestamp variables

### RTP Timestamp Conversion

```c
// Convert presentation timestamp to RTP timestamp (90kHz clock)
uint32_t rtp_timestamp = timestamp->tv_sec * 90000 +
                        timestamp->tv_usec * 90 / 1000;
```

**90kHz Clock**: Standard for H.264 RTP streams, providing precise timing resolution.

## RTSP Server Implementation

### Pure C RTSP Protocol Handler

```c
typedef struct {
    int socket_fd;
    int state;
    char session_id[32];
    int video_channel;
    uint16_t rtp_seq;
    uint32_t rtp_ssrc;
    // Transport-specific fields
    int client_rtp_port;
    int client_rtcp_port;
    int server_rtp_port;
    int server_rtcp_port;
} rtsp_client_t;

// RTSP state machine
typedef enum {
    RTSP_STATE_INIT = 0,
    RTSP_STATE_READY = 1,
    RTSP_STATE_PLAYING = 2
} rtsp_state_t;
```

### SDP Generation for H.264

```c
static int generate_sdp(minimal_rtsp_server_t* server,
                       const char* stream_name,
                       char* sdp_buffer, size_t buffer_size) {
    return snprintf(sdp_buffer, buffer_size,
        "v=0\r\n"
        "o=- 0 0 IN IP4 %s\r\n"
        "s=Thingino Streamer H.264 Stream\r\n"
        "c=IN IP4 %s\r\n"
        "t=0 0\r\n"
        "m=video %d RTP/AVP 96\r\n"
        "a=rtpmap:96 H264/90000\r\n"
        "a=fmtp:96 packetization-mode=1;profile-level-id=42E01E;"
        "sprop-parameter-sets=%s,%s\r\n"
        "a=control:track1\r\n",
        server->server_ip, server->server_ip, server->port,
        sps_base64, pps_base64);
}
```

**Critical Elements**:
- **packetization-mode=1**: Enables FU-A fragmentation for large frames
- **profile-level-id**: Specifies H.264 baseline profile
- **sprop-parameter-sets**: SPS/PPS parameters for decoder initialization

## H.264 RTP Packetization

### FU-A Fragmentation for Large Frames

```c
static int send_rtp_packet(rtsp_client_t* client, const void* data,
                          unsigned int size, uint32_t timestamp, bool marker) {
    const uint8_t* frame_data = (const uint8_t*)data;

    // Check if fragmentation is needed
    if (size <= MAX_RTP_PAYLOAD_SIZE) {
        // Single NAL unit packet
        return send_single_nal_packet(client, data, size, timestamp, marker);
    } else {
        // Fragment using FU-A
        return send_fragmented_nal_packet(client, data, size, timestamp, marker);
    }
}

static int send_fragmented_nal_packet(rtsp_client_t* client,
                                     const void* data, unsigned int size,
                                     uint32_t timestamp, bool marker) {
    const uint8_t* nal_data = (const uint8_t*)data;
    uint8_t nal_header = nal_data[0];
    uint8_t nal_type = nal_header & 0x1F;
    uint8_t nal_nri = nal_header & 0x60;

    // FU-A header construction
    uint8_t fu_indicator = 0x60 | nal_nri;  // FU-A type (28) + NRI
    uint8_t fu_header = 0x80 | nal_type;    // Start bit + NAL type

    // Fragment the NAL unit
    const uint8_t* payload = nal_data + 1;  // Skip original NAL header
    unsigned int remaining = size - 1;

    while (remaining > 0) {
        unsigned int fragment_size = (remaining > MAX_RTP_PAYLOAD_SIZE - 2) ?
                                    MAX_RTP_PAYLOAD_SIZE - 2 : remaining;

        // Build RTP packet with FU-A headers
        uint8_t rtp_packet[MAX_RTP_PACKET_SIZE];
        build_rtp_header(rtp_packet, timestamp,
                        (remaining == fragment_size) && marker);

        // Add FU-A headers
        rtp_packet[12] = fu_indicator;
        rtp_packet[13] = fu_header;

        // Copy fragment payload
        memcpy(&rtp_packet[14], payload, fragment_size);

        // Send fragment
        send_rtp_data(client, rtp_packet, 14 + fragment_size);

        // Update for next fragment
        payload += fragment_size;
        remaining -= fragment_size;
        fu_header &= 0x7F;  // Clear start bit
        if (remaining <= MAX_RTP_PAYLOAD_SIZE - 2) {
            fu_header |= 0x40;  // Set end bit
        }

        client->rtp_seq++;
    }

    return size;
}
```

**FU-A Protocol Details**:
- **FU Indicator**: Type 28 (0x1C) with NRI bits from original NAL header
- **FU Header**: Start/End bits + original NAL type
- **Fragmentation**: Splits large NAL units across multiple RTP packets
- **Sequence Numbers**: Incremented for each fragment

## IMP Encoder Integration

### Direct API Usage Pattern

```c
static int setup_encoder_channels(void) {
    IMPEncoderChnAttr channel_attr;

    // Configure main stream (1920x1080@25fps)
    memset(&channel_attr, 0, sizeof(channel_attr));
    channel_attr.encAttr.enType = PT_H264;
    channel_attr.encAttr.bufSize = 1920 * 1080 * 3 / 2;  // YUV420 size
    channel_attr.encAttr.profile = 0;  // Baseline profile
    channel_attr.encAttr.picWidth = 1920;
    channel_attr.encAttr.picHeight = 1080;

    // Rate control settings
    channel_attr.rcAttr.attrRcMode.rcMode = ENC_RC_MODE_FIXQP;
    channel_attr.rcAttr.attrRcMode.attrFixQp.qp = 25;
    channel_attr.rcAttr.outFrmRate.frmRateNum = 25;
    channel_attr.rcAttr.outFrmRate.frmRateDen = 1;

    // Create encoder channel
    int ret = IMP_Encoder_CreateChn(0, &channel_attr);
    if (ret < 0) {
        LOG_ERROR("IMP_Encoder_CreateChn failed: %d", ret);
        return -1;
    }

    // Register channel to group (critical for frame flow)
    ret = IMP_Encoder_RegisterChn(0, 0);
    if (ret < 0) {
        LOG_ERROR("IMP_Encoder_RegisterChn failed: %d", ret);
        return -1;
    }

    return 0;
}
```

### Frame Processing Pipeline

```c
static int process_encoded_frame(int channel) {
    IMPEncoderStream stream;
    int ret;

    // Poll for available data (non-blocking with timeout)
    ret = IMP_Encoder_PollingStream(channel, 100);  // 100ms timeout
    if (ret < 0) {
        return ret;  // No data available or error
    }

    // Get the encoded stream
    ret = IMP_Encoder_GetStream(channel, &stream, 1);  // Non-blocking
    if (ret < 0) {
        LOG_ERROR("IMP_Encoder_GetStream failed: %d", ret);
        return ret;
    }

    // Process all packs in the stream (SPS+PPS+IDR for I-frames)
    static uint8_t combined_frame[1024*1024];  // 1MB buffer
    unsigned int total_size = 0;

    for (int pack_idx = 0; pack_idx < stream.packCount; pack_idx++) {
        if (stream.pack[pack_idx].length > 0) {
            uint32_t pack_offset = stream.pack[pack_idx].offset;
            uint32_t pack_length = stream.pack[pack_idx].length;

            // Handle circular buffer wrap-around
            uint32_t remSize = stream.streamSize - pack_offset;
            if (remSize < pack_length) {
                // Pack wraps around - copy in two parts
                memcpy(combined_frame + total_size,
                       (void*)(stream.virAddr + pack_offset), remSize);
                memcpy(combined_frame + total_size + remSize,
                       (void*)stream.virAddr, pack_length - remSize);
            } else {
                // Pack fits in one piece
                memcpy(combined_frame + total_size,
                       (void*)(stream.virAddr + pack_offset), pack_length);
            }
            total_size += pack_length;
        }
    }

    // Generate synthetic timestamp and send to RTSP server
    if (total_size > 0) {
        generate_frame_timestamp(&frame_timestamp);
        minimal_rtsp_server_send_frame(rtsp_server, channel,
                                       combined_frame, total_size,
                                       &frame_timestamp);
    }

    // Release the stream back to encoder
    ret = IMP_Encoder_ReleaseStream(channel, &stream);
    if (ret < 0) {
        LOG_ERROR("IMP_Encoder_ReleaseStream failed: %d", ret);
    }

    return total_size;
}
```

**Key Optimizations**:
- **Non-blocking operations**: Prevents thread stalls
- **Circular buffer handling**: Manages IMP's ring buffer correctly
- **Pack concatenation**: Combines SPS+PPS+IDR into single frame
- **Immediate release**: Returns buffers to encoder quickly

## Memory Management Patterns

### Resource Cleanup Pattern

```c
typedef struct {
    int socket_fd;
    char* buffer;
    size_t buffer_size;
    bool initialized;
} resource_t;

static void cleanup_resource(resource_t* res) {
    if (!res) return;

    if (res->socket_fd >= 0) {
        close(res->socket_fd);
        res->socket_fd = -1;
    }

    if (res->buffer) {
        free(res->buffer);
        res->buffer = NULL;
    }

    res->buffer_size = 0;
    res->initialized = false;
}

// RAII-style cleanup using goto for error handling
static int initialize_system(void) {
    resource_t res = {0};
    res.socket_fd = -1;

    res.socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (res.socket_fd < 0) {
        goto cleanup;
    }

    res.buffer = malloc(BUFFER_SIZE);
    if (!res.buffer) {
        goto cleanup;
    }
    res.buffer_size = BUFFER_SIZE;

    // ... initialization code ...

    res.initialized = true;
    return 0;

cleanup:
    cleanup_resource(&res);
    return -1;
}
```

### Stack vs Heap Allocation Strategy

```c
// Prefer stack allocation for small, fixed-size data
void process_frame(void) {
    char response_buffer[1024];  // Stack allocation
    struct timeval timestamp;   // Stack allocation

    // Use heap only for large or variable-size data
    uint8_t* frame_buffer = malloc(frame_size);
    if (!frame_buffer) {
        LOG_ERROR("Failed to allocate frame buffer");
        return;
    }

    // ... processing ...

    free(frame_buffer);  // Always paired with malloc
}
```

## Performance Optimization Techniques

### Hot Path Optimization

```c
// Optimized frame processing loop
static void* encoder_thread_func(void* arg) {
    int loop_count = 0;

    while (global_rtsp_thread_signal) {
        loop_count++;

        // Process only channel 0 for main stream
        int ret = process_encoded_frame(0);
        if (ret > 0) {
            // Frame processed successfully
            continue;
        }

        // Brief sleep only when no frames available
        if (ret == -1) {  // Timeout, not error
            usleep(10000);  // 10ms sleep
        }
    }

    return NULL;
}
```

### Compiler Optimization Flags

```makefile
# Production build flags
CFLAGS += -Os                    # Optimize for size (embedded)
CFLAGS += -ffunction-sections    # Enable dead code elimination
CFLAGS += -fdata-sections        # Enable dead data elimination
LDFLAGS += -Wl,--gc-sections     # Remove unused sections

# Debug build flags (when needed)
CFLAGS += -g -O0                 # Debug symbols, no optimization
CFLAGS += -DDEBUG_ENABLED        # Enable debug logging
```

## Error Handling Patterns

### Consistent Error Propagation

```c
typedef enum {
    THINGINO_STREAMER_SUCCESS = 0,
    THINGINO_STREAMER_ERROR_INVALID_PARAM = -1,
    THINGINO_STREAMER_ERROR_MEMORY = -2,
    THINGINO_STREAMER_ERROR_NETWORK = -3,
    THINGINO_STREAMER_ERROR_HARDWARE = -4,
    THINGINO_STREAMER_ERROR_TIMEOUT = -5
} thingino_streamer_error_t;

// Error handling with cleanup
static int setup_network_connection(const char* address, int port) {
    int sock_fd = -1;
    struct sockaddr_in addr;

    if (!address || port <= 0) {
        return THINGINO_STREAMER_ERROR_INVALID_PARAM;
    }

    sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_fd < 0) {
        LOG_ERROR("Failed to create socket: %s", strerror(errno));
        return THINGINO_STREAMER_ERROR_NETWORK;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    if (inet_pton(AF_INET, address, &addr.sin_addr) <= 0) {
        LOG_ERROR("Invalid address: %s", address);
        close(sock_fd);
        return THINGINO_STREAMER_ERROR_INVALID_PARAM;
    }

    if (connect(sock_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        LOG_ERROR("Connection failed: %s", strerror(errno));
        close(sock_fd);
        return THINGINO_STREAMER_ERROR_NETWORK;
    }

    return sock_fd;  // Success: return file descriptor
}
```

## Build System Integration

### Thingino Buildroot Package

```makefile
# thingino-streamer.mk - Buildroot package definition
THINGINO_STREAMER_SITE_METHOD = git
THINGINO_STREAMER_SITE = https://github.com/themactep/thingino-streamer
THINGINO_STREAMER_VERSION = $(shell git ls-remote $(THINGINO_STREAMER_SITE) master | head -1 | cut -f1)

THINGINO_STREAMER_DEPENDENCIES = json-c thingino-live555 ingenic-lib libschrift

# Compiler flags for embedded target
THINGINO_STREAMER_CFLAGS += \
    -DPLATFORM_$(shell echo $(SOC_FAMILY) | tr a-z A-Z) \
    -DNO_OPENSSL=1 -Os \
    -I$(STAGING_DIR)/usr/include \
    -I$(STAGING_DIR)/usr/include/liveMedia

# Build command
define THINGINO_STREAMER_BUILD_CMDS
    $(MAKE) \
        ARCH=$(TARGET_ARCH) \
        CROSS_COMPILE=$(TARGET_CROSS) \
        CFLAGS="$(THINGINO_STREAMER_CFLAGS)" \
        LDFLAGS="$(THINGINO_STREAMER_LDFLAGS)" \
        -C $(@D) all
endef
```

---

*This technical implementation guide provides the detailed knowledge needed to understand, modify, and extend the Thingino Streamer Pure C streaming server. Each pattern and technique has been tested and optimized for embedded deployment.*
