# RTMP Server Technical Implementation

## Architecture Overview

The RTMP server implementation follows a modular, event-driven architecture that integrates seamlessly with the existing Thingino streamer framework.

## Core Components

### 1. Module System Integration

```c
typedef struct {
    const char* name;
    const char* description;
    int (*init)(void);
    int (*start)(void);
    int (*stop)(void);
    void (*cleanup)(void);
    int (*rtsp_frame_callback)(struct rtsp_server* server, int channel);
} module_info_t;
```

The RTMP module registers with the module system and receives video frames via the RTSP frame callback mechanism.

### 2. RTMP Server Structure

```c
typedef struct {
    int server_socket;
    bool running;
    pthread_t accept_thread;
    rtmp_connection_t* connections;
    pthread_mutex_t connections_mutex;
    rtmp_module_config_t config;
} rtmp_server_t;
```

### 3. Connection Management

```c
typedef struct rtmp_connection {
    int socket_fd;
    rtmp_state_t state;
    pthread_t thread;
    bool thread_running;
    bool publishing;
    char app_name[256];
    char stream_key[256];
    uint32_t chunk_size_in;
    uint32_t chunk_size_out;
    uint32_t window_ack_size;
    uint8_t c1_s1_data[RTMP_HANDSHAKE_SIZE];
    uint8_t c2_s2_data[RTMP_HANDSHAKE_SIZE];
    uint8_t c0_s0_version;
    struct rtmp_connection* next;
} rtmp_connection_t;
```

## Protocol Implementation

### 1. RTMP Handshake

The handshake follows the RTMP specification exactly:

```
Client                    Server
  |                         |
  |-------- C0 (ver) ------>|  1. Version exchange
  |<------- S0 (ver) -------|
  |                         |
  |------- C1 (1536) ------>|  2. Random data exchange
  |<------ S1 (1536) -------|
  |<------ S2 (echo) -------|
  |                         |
  |------- C2 (echo) ------>|  3. Echo verification
  |                         |
  |    HANDSHAKE DONE!      |
```

**Implementation Details:**
- C0/S0: Single byte version (0x03)
- C1/S1: 1536 bytes with timestamp and random data
- C2/S2: Echo of received C1/S1 data
- Timeout protection: 30-second socket timeouts
- Error handling: Graceful connection cleanup on failures

### 2. Chunking Protocol

RTMP uses a chunking protocol to break large messages into smaller chunks:

```c
typedef struct {
    uint8_t fmt;                    // Chunk type (0-3)
    uint32_t chunk_stream_id;       // Chunk stream ID
    uint32_t timestamp;             // Message timestamp
    uint32_t message_length;        // Message length
    uint8_t message_type_id;        // Message type
    uint32_t message_stream_id;     // Message stream ID
} rtmp_chunk_header_t;
```

**Chunk Types:**
- **Type 0** (11 bytes): Full header for new messages
- **Type 1** (7 bytes): No message stream ID (same stream)
- **Type 2** (3 bytes): Only timestamp delta
- **Type 3** (0 bytes): No header (continuation)

**Features:**
- Header compression for efficiency
- Extended timestamp support (>= 0xFFFFFF)
- Variable chunk stream ID encoding (1-3 bytes)
- Partial message assembly across multiple chunks

### 3. AMF Encoding/Decoding

Action Message Format (AMF0) is used for command messages:

```c
typedef struct amf_value {
    uint8_t type;
    union {
        double number;
        uint8_t boolean;
        struct {
            char* data;
            uint16_t length;
        } string;
        struct {
            struct amf_property* properties;
            int count;
        } object;
    } value;
} amf_value_t;
```

**Supported Types:**
- Numbers (IEEE 754 double precision)
- Booleans (true/false)
- Strings (UTF-8 with length prefix)
- Objects (key-value pairs)
- Null values
- Arrays (future enhancement)

## Video Streaming Integration

### 1. Frame Processing Pipeline

```
IMP Encoder → RTSP Callback → RTMP Module → Frame Distribution → RTMP Clients
```

**Process Flow:**
1. IMP encoder produces H.264/H.265 frames
2. RTSP callback polls for available frames (10ms timeout)
3. RTMP module receives frames via callback
4. Frames are distributed to all publishing connections
5. RTMP chunking protocol delivers frames to clients

### 2. Frame Callback Implementation

```c
int rtmp_module_rtsp_frame_callback(struct rtsp_server* server, int channel)
{
    // Check for publishing connections
    if (publishing_connections == 0) return 0;

    // Poll for frames from IMP encoder
    int ret = IMP_Encoder_PollingStream(channel, 10);
    if (ret >= 0) {
        IMPEncoderStream stream;
        ret = IMP_Encoder_GetStream(channel, &stream, 1);
        if (ret >= 0) {
            // Distribute frame to RTMP connections
            rtmp_server_send_frame(g_rtmp_server, channel,
                                 stream.virAddr, frame_size, &timestamp);
            IMP_Encoder_ReleaseStream(channel, &stream);
        }
    }
    return 0;
}
```

### 3. RTMP Video Messages

Video frames are sent as RTMP video messages:

```c
typedef struct {
    rtmp_chunk_header_t header;
    uint8_t* payload;
    uint32_t payload_size;
    uint32_t bytes_read;
} rtmp_message_t;
```

**Message Format:**
- Chunk Stream ID: 4 (video channel)
- Message Type: 9 (video message)
- Timestamp: Converted from IMP timestamp
- Payload: Raw H.264/H.265 frame data

## Command Processing

### 1. Command Structure

```c
typedef struct {
    char* command_name;
    double transaction_id;
    amf_value_t command_object;
    amf_value_t* arguments;
    int argument_count;
} rtmp_command_t;
```

### 2. Supported Commands

**connect**: Establishes RTMP connection
```
Command: "connect"
Transaction ID: 1
Command Object: {app: "live", flashVer: "FMLE/3.0", ...}
```

**createStream**: Creates a new stream
```
Command: "createStream"
Transaction ID: 2
Command Object: null
```

**publish**: Starts publishing a stream
```
Command: "publish"
Transaction ID: 0
Arguments: ["stream_key", "live"]
```

### 3. Response Generation

Responses use AMF encoding:

```c
// _result response for connect
{
    command_name: "_result",
    transaction_id: 1,
    properties: {...},
    information: null
}

// onStatus for publish
{
    command_name: "onStatus",
    transaction_id: 0,
    command_object: null,
    info_object: {
        level: "status",
        code: "NetStream.Publish.Start",
        description: "Publishing stream"
    }
}
```

## Memory Management

### 1. Connection Lifecycle

```c
// Connection creation
rtmp_connection_t* conn = malloc(sizeof(rtmp_connection_t));
memset(conn, 0, sizeof(rtmp_connection_t));

// Add to connection list
pthread_mutex_lock(&server->connections_mutex);
conn->next = server->connections;
server->connections = conn;
pthread_mutex_unlock(&server->connections_mutex);

// Connection cleanup
static void rtmp_connection_cleanup(rtmp_connection_t* conn)
{
    if (conn->socket_fd >= 0) {
        close(conn->socket_fd);
    }
    // Remove from connection list
    // Free allocated memory
    free(conn);
}
```

### 2. Message Buffers

```c
// Frame allocation
msg.payload = malloc(frame_size);
memcpy(msg.payload, frame_data, frame_size);

// Automatic cleanup
if (msg.payload) {
    free(msg.payload);
    msg.payload = NULL;
}
```

### 3. AMF Memory Management

```c
void amf_value_free(amf_value_t* value)
{
    switch (value->type) {
        case AMF0_STRING:
            if (value->value.string.data) {
                free(value->value.string.data);
            }
            break;
        case AMF0_OBJECT:
            amf_object_free(value->value.object.properties,
                          value->value.object.count);
            break;
    }
}
```

## Threading Model

### 1. Thread Architecture

```
Main Thread
├── RTMP Accept Thread (rtmp_accept_thread)
│   └── Accepts new connections
│   └── Creates connection threads
├── Connection Thread 1 (rtmp_connection_thread)
│   └── Handles RTMP protocol for connection 1
├── Connection Thread 2 (rtmp_connection_thread)
│   └── Handles RTMP protocol for connection 2
└── RTSP Frame Thread (existing)
    └── Calls RTMP frame callback
```

### 2. Thread Synchronization

```c
// Connection list protection
pthread_mutex_t connections_mutex;

// Frame distribution
pthread_mutex_lock(&server->connections_mutex);
rtmp_connection_t* conn = server->connections;
while (conn) {
    if (conn->publishing) {
        rtmp_send_video_frame(conn, frame_data, frame_size, timestamp);
    }
    conn = conn->next;
}
pthread_mutex_unlock(&server->connections_mutex);
```

### 3. Thread Safety

- **Connection list**: Protected by mutex
- **Socket operations**: Per-connection (thread-safe)
- **Frame distribution**: Atomic operations where possible
- **Configuration**: Read-only after initialization

## Error Handling

### 1. Network Errors

```c
static int rtmp_read_bytes(int socket_fd, uint8_t* buffer, size_t length)
{
    size_t bytes_read = 0;
    while (bytes_read < length) {
        ssize_t result = recv(socket_fd, buffer + bytes_read,
                            length - bytes_read, 0);
        if (result <= 0) {
            if (result == 0) {
                return 0; // Connection closed
            } else {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    IMP_LOG_ERR(TAG, "Socket timeout during read");
                } else {
                    IMP_LOG_ERR(TAG, "recv() failed: %s", strerror(errno));
                }
                return -1;
            }
        }
        bytes_read += result;
    }
    return bytes_read;
}
```

### 2. Protocol Errors

```c
// Invalid chunk format
if (header->fmt > 3) {
    IMP_LOG_ERR(TAG, "Invalid chunk format: %d", header->fmt);
    return -1;
}

// Buffer overflow protection
if (offset + length > buffer_size) {
    IMP_LOG_ERR(TAG, "AMF buffer underrun");
    return -1;
}
```

### 3. Resource Cleanup

```c
// Graceful connection cleanup
static void rtmp_connection_cleanup(rtmp_connection_t* conn)
{
    if (!conn) return;

    conn->thread_running = false;

    if (conn->socket_fd >= 0) {
        close(conn->socket_fd);
        conn->socket_fd = -1;
    }

    // Remove from connection list
    pthread_mutex_lock(&g_rtmp_server->connections_mutex);
    // ... list removal logic ...
    pthread_mutex_unlock(&g_rtmp_server->connections_mutex);

    free(conn);
}
```

## Performance Optimizations

### 1. Frame Processing

- **Polling optimization**: 10ms timeout for responsive frame processing
- **Connection filtering**: Only process frames when clients are publishing
- **Memory reuse**: Minimize allocations in frame path
- **Zero-copy**: Direct memory access where possible

### 2. Network Optimization

- **Socket timeouts**: Prevent hanging connections
- **Chunk size tuning**: Configurable chunk sizes for efficiency
- **Buffer management**: Optimal buffer sizes for network I/O
- **Connection limits**: Configurable maximum connections

### 3. CPU Optimization

- **AMF caching**: Reuse encoded AMF objects where possible
- **Header compression**: Use appropriate chunk types
- **Minimal copying**: Avoid unnecessary memory copies
- **Efficient parsing**: Optimized AMF parsing routines

## Configuration System

### 1. JSON Configuration

```json
{
  "rtmp": {
    "enabled": true,
    "port": 1935,
    "max_connections": 10,
    "chunk_size": 4096,
    "auth_required": false,
    "stream_key": "",
    "app_name": "live",
    "connection_timeout": 30
  }
}
```

### 2. Runtime Configuration

```c
typedef struct {
    bool enabled;
    int port;
    int max_connections;
    int chunk_size;
    bool auth_required;
    char stream_key[256];
    char app_name[256];
    int connection_timeout;
} rtmp_module_config_t;
```

### 3. Configuration Loading

```c
static int rtmp_module_load_config(rtmp_module_config_t* config)
{
    // Set defaults
    rtmp_module_set_default_config(config);

    // Load from JSON file
    json_object* root = json_object_from_file("/etc/streamer.d/rtmp.json");
    if (root) {
        // Parse JSON configuration
        rtmp_module_parse_json_config(config, root);
        json_object_put(root);
    }

    return 0;
}
```

## Integration Points

### 1. Module System

- **Registration**: `MODULE_REGISTER(rtmp_module_info)`
- **Lifecycle**: init → start → stop → cleanup
- **Callbacks**: RTSP frame callback integration

### 2. Build System

- **Buildroot**: `BR2_PACKAGE_THINGINO_STREAMER_RTMP`
- **Makefile**: Conditional compilation with `ENABLE_RTMP`
- **Dependencies**: IMP libraries, JSON-C, pthread

### 3. Configuration System

- **JSON files**: `/etc/streamer.d/rtmp.json`
- **API integration**: HTTP configuration endpoints
- **Runtime updates**: Dynamic configuration changes

## Future Enhancements

### 1. Protocol Extensions

- **RTMPS**: SSL/TLS encryption support
- **Enhanced AMF**: AMF3 support for advanced features
- **Metadata**: Stream metadata and cue points
- **Authentication**: Advanced user management

### 2. Performance Improvements

- **Hardware acceleration**: GPU-assisted encoding
- **Multi-threading**: Parallel frame processing
- **Memory pools**: Pre-allocated buffer pools
- **Network optimization**: TCP tuning and buffering

### 3. Feature Additions

- **Recording**: Save incoming streams to disk
- **Transcoding**: Multiple output formats
- **Statistics**: Detailed performance metrics
- **Webhooks**: Stream event notifications

## Testing and Validation

### 1. Unit Tests

- AMF encoding/decoding validation
- Chunk protocol correctness
- Memory leak detection
- Error handling verification

### 2. Integration Tests

- OBS Studio compatibility
- FFmpeg compatibility
- Multi-client scenarios
- Performance benchmarks

### 3. Stress Testing

- Connection limits
- Memory usage under load
- CPU usage optimization
- Network bandwidth limits

This technical implementation provides a solid foundation for RTMP streaming while maintaining compatibility with the existing Thingino architecture.

## API Reference

### Core Functions

#### Server Management
```c
int rtmp_module_init(void);
int rtmp_module_start(void);
int rtmp_module_stop(void);
void rtmp_module_cleanup(void);
```

#### Connection Handling
```c
int rtmp_handshake_process(rtmp_connection_t* conn);
int rtmp_chunk_read(rtmp_connection_t* conn, rtmp_message_t* msg);
int rtmp_chunk_write(rtmp_connection_t* conn, rtmp_message_t* msg);
int rtmp_message_parse(rtmp_connection_t* conn, rtmp_message_t* msg);
```

#### AMF Functions
```c
int amf_encode_number(uint8_t** buffer, size_t* buffer_size, double number);
int amf_encode_string(uint8_t** buffer, size_t* buffer_size, const char* string);
int amf_decode_value(const uint8_t* buffer, size_t buffer_size,
                     size_t* offset, amf_value_t* value);
void amf_value_free(amf_value_t* value);
```

#### Video Streaming
```c
int rtmp_send_video_frame(rtmp_connection_t* conn, const uint8_t* frame_data,
                         uint32_t frame_size, uint32_t timestamp);
int rtmp_server_send_frame(rtmp_server_t* server, int channel,
                          const uint8_t* frame_data, uint32_t frame_size,
                          const struct timeval* timestamp);
```

### Constants and Definitions

```c
#define RTMP_VERSION                3
#define RTMP_HANDSHAKE_SIZE        1536
#define RTMP_DEFAULT_CHUNK_SIZE    4096
#define RTMP_DEFAULT_PORT          1935

// Message Types
#define RTMP_MSG_SET_CHUNK_SIZE    1
#define RTMP_MSG_ACKNOWLEDGEMENT   3
#define RTMP_MSG_WINDOW_ACK_SIZE   5
#define RTMP_MSG_AUDIO             8
#define RTMP_MSG_VIDEO             9
#define RTMP_MSG_COMMAND_AMF0      20

// Chunk Types
#define RTMP_CHUNK_TYPE_0          0
#define RTMP_CHUNK_TYPE_1          1
#define RTMP_CHUNK_TYPE_2          2
#define RTMP_CHUNK_TYPE_3          3

// AMF Types
#define AMF0_NUMBER                0x00
#define AMF0_BOOLEAN               0x01
#define AMF0_STRING                0x02
#define AMF0_OBJECT                0x03
#define AMF0_NULL                  0x05
```
