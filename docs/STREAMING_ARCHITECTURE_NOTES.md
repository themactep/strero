# Streaming Architecture Notes

## IMP Buffer Management Discoveries

### Native IMP Encoder Buffering
The T31 IMP encoder has sophisticated **native buffer management** that handles multi-channel streaming efficiently:

```c
// IMP provides native stream buffering per channel
IMP_Encoder_SetMaxStreamCnt(channel, 6);        // 6 stream buffers per channel
IMP_Encoder_SetStreamBufSize(channel, 256KB);   // 256KB per buffer
```

**Key Findings:**
- **Multi-channel support**: IMP handles concurrent channel processing natively
- **Flow control**: Built-in backpressure and buffer management
- **Memory efficiency**: Optimized for embedded T31X constraints
- **No artificial staggering needed**: SDK designed for simultaneous channel operation

### Frame Source Buffers (nrVBs)
```c
.nrVBs = 3;  // Frame source buffers - separate from stream buffers
```
- **Purpose**: Input frame buffering before encoding
- **Separate from**: Stream output buffers
- **Memory impact**: ~5-8MB total for both channels

### Memory Layout (T31X: 128MB total)
- **OS Memory**: 84MB
- **RMEM (Video)**: 44MB
  - IMP stream buffers: ~3MB (6×256KB×2 channels)
  - Frame source buffers: ~8MB
  - Available headroom: ~33MB

## Async Queue Experiment Results

### Problem Identified
Original jerky playback was caused by:
1. **TCP fragmentation inefficiency**: Multiple send() calls per fragment
2. **Excessive logging**: Blocking I/O during frame processing
3. **Synchronous RTSP processing**: Blocking main polling loop

### Async Queue Implementation
We implemented custom async frame queues:
```c
static frame_queue_t rtsp_frame_queue[FS_CHN_NUM];  // 10×512KB per channel
static pthread_t rtsp_sender_thread[FS_CHN_NUM];    // Dedicated threads
```

### Results
- **Single channel**: Worked perfectly, eliminated jerky playback
- **Dual channel**: Created **double buffering problem**
  - Channel 0: Queue constantly full, dropping frames
  - Channel 1: Balanced operation
  - Root cause: **Over-buffering on top of native IMP buffers**

### Key Insight: Double Buffering Problem
```
Frame → IMP Native Buffers → Our Async Queue → RTSP Sender → Network
         (6×256KB, efficient)   (10×512KB, bottleneck)
```

The async queue became the **limiting factor**, not the solution.

## Correct Architecture

### Use Native IMP Buffering
```c
// Rely on IMP's native multi-channel buffer management
IMP_Encoder_PollingStream(channel, timeout);
IMP_Encoder_GetStream(channel, &stream, blocking);
rtsp_server_send_frame(server, channel, data, size, timestamp);
IMP_Encoder_ReleaseStream(channel, &stream);
```

### Optimizations Applied
1. **TCP fragmentation**: Single send() calls instead of header+packet
2. **Reduced logging**: Changed INFO to DBG for high-frequency logs
3. **IMP buffer tuning**: 6 stream buffers × 256KB per channel
4. **Memory optimization**: Conservative settings for T31X constraints

## Performance Results

### Before Optimizations
- **Jerky playback**: 200-600ms polling gaps
- **Burst-freeze pattern**: Rapid motion → freeze → repeat
- **TCP inefficiency**: Multiple send() calls per fragment

### After TCP + Logging Optimizations (No Async Queue)
- **Smooth single channel**: Consistent ~25ms intervals
- **Efficient fragmentation**: Single TCP send() per fragment
- **Clean logs**: Reduced I/O blocking

### With Async Queue (Incorrect Approach)
- **Single channel**: Perfect performance
- **Dual channel**: Buffer competition and frame drops
- **Memory waste**: Duplicate buffering

## Lessons Learned

1. **Trust the SDK**: IMP encoder designed for multi-channel streaming
2. **Identify root cause**: TCP inefficiency, not buffer management
3. **Avoid over-engineering**: Native buffering is sufficient
4. **Memory constraints**: T31X requires conservative buffer sizing
5. **Test multi-channel**: Single channel success ≠ multi-channel success

## Recommended Architecture

Use **synchronous processing** with **native IMP buffers**:
- Optimized TCP fragmentation (single send() calls)
- Reduced logging overhead
- Conservative IMP buffer configuration
- Direct encoder → RTSP → network flow

This provides smooth multi-channel streaming within T31X memory constraints.
