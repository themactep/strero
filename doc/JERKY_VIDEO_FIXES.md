# Jerky Video Output Fixes

## Problem Description
The video output was experiencing jerky playback with freezes and fast-forward motions, indicating timing and buffering issues in the video pipeline.

## Root Causes Identified

1. **Insufficient Buffering**: Both channels had `buffer_count: 1` providing no frame buffering
2. **Polling Timeout Mismatch**: Encoder used 30ms timeout but config had 500ms `imp_polling_timeout`
3. **No Frame Smoothing**: Direct frame-by-frame processing without timing smoothing
4. **Lack of Frame Drop Detection**: No monitoring for dropped frames that cause jerky playback
5. **Aggressive Loop Timing**: 1ms sleep in RTSP server loop could contribute to timing issues

## Fixes Implemented

### 1. Increased Buffer Depths
**Files Modified**: `res/streamer.json`, `src/common.c`

- Increased `buffer_count` from 1 to 3 for both channels in framesource configuration
- Increased `buffers` from 1 to 3 for both streams in RTSP configuration
- Updated `nrVBs` from 1 to 3 in common.c for better frame buffering

**Rationale**: Provides proper frame buffering to smooth out timing variations and prevent frame starvation.

### 2. Optimized Polling Timeouts
**Files Modified**: `src/frame_manager.c`, `src/modules/rtsp/rtsp_module.c`

- Removed hardcoded `imp_polling_timeout` from configuration
- Polling timeout now calculated automatically based on configured frame rate (1000/fps ms)
- RTSP module polling timeout updated to use frame-rate appropriate timing

**Rationale**: Aligns polling timeouts with actual frame delivery timing for smoother operation.

### 3. Removed Redundant Frame Rate Limiting
**Files Modified**: `src/modules/rtsp/rtsp_module.c`

- Removed hardcoded frame rate limiting logic that conflicted with encoder timing
- Eliminated double frame rate control that caused irregular frame drops
- Let encoder handle frame rate control natively for smoother timing

**Rationale**: Prevents timing conflicts between encoder and RTSP module frame rate control.

### 4. Added Frame Timing Smoothing
**File Modified**: `src/encoder.c`

- Implemented per-channel frame timing tracking with running averages
- Added frame interval monitoring to detect timing variations
- Log warnings for significant timing deviations (>100ms or <10ms)
- Reset averages every 100 frames to adapt to changes

**Rationale**: Provides visibility into frame timing issues and helps identify problematic patterns.

### 5. Implemented Frame Drop Detection
**File Modified**: `src/main.c`

- Added sequence number tracking for both channels
- Detect and log dropped frames by checking sequence number jumps
- Track consecutive timeout counts per channel
- Log warnings when excessive timeouts occur (>10 consecutive)

**Rationale**: Identifies when frames are being dropped, which is a primary cause of jerky playback.

### 6. Optimized RTSP Server Loop Timing
**File Modified**: `src/minimal_rtsp_server.c`

- Reduced sleep time from 1ms to 0.5ms in RTP thread loop
- Provides smoother frame delivery timing

**Rationale**: Reduces potential timing jitter in frame delivery pipeline.

### 6. Enhanced Timeout Handling
**File Modified**: `src/main.c`

- Added per-channel consecutive timeout tracking
- Reset timeout counters on successful frame retrieval
- Log warnings for excessive timeouts that could indicate hardware issues

**Rationale**: Provides early warning of hardware or timing issues that could cause jerky playback.

## Expected Improvements

1. **Smoother Playback**: Increased buffering should eliminate most freeze/fast-forward cycles
2. **Better Timing Consistency**: Aligned timeouts should provide more consistent frame delivery
3. **Improved Diagnostics**: Frame drop detection and timing monitoring provide visibility into issues
4. **Reduced Jitter**: Optimized loop timing should reduce micro-stutters
5. **Better Recovery**: Enhanced timeout handling should improve recovery from temporary issues

## Monitoring and Debugging

The fixes include extensive logging to help monitor video smoothness:

- Frame timing variations are logged when they exceed normal ranges
- Dropped frames are detected and logged with sequence number details
- Consecutive timeouts are tracked and logged when excessive
- Channel synchronization deltas are monitored

## Testing Recommendations

1. Monitor logs for frame drop warnings during playback
2. Check for timing variation warnings that might indicate hardware issues
3. Verify that consecutive timeout warnings are minimal
4. Test with both single and dual-channel streaming scenarios
5. Monitor for extended periods to ensure long-term stability

## Configuration Notes

The buffer increases will use slightly more memory but should still be well within T31 hardware limits. The 3-buffer configuration provides a good balance between smoothness and memory usage.

If memory constraints become an issue, the buffer counts can be reduced to 2, but 1 should be avoided as it provides no buffering margin.
