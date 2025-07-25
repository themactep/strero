# Multi-Stream Configuration Fix

## Problem Analysis
Current implementation has resource conflicts preventing reliable dual-stream operation with JPEG snapshots.

## Required Changes Based on Official SDK Patterns

### 1. FrameSource Channel Configuration (CORRECT)
Current implementation is mostly correct but needs adjustment:

```c
// Channel 0: Main stream (1920x1080) - KEEP AS IS
// Channel 1: Sub stream (640x360) - KEEP AS IS  
```

### 2. Encoder Channel Architecture (MAJOR FIX NEEDED)

**Current (INCORRECT):**
```c
// Both channels in group 0 - CAUSES CONFLICTS
IMP_Encoder_CreateGroup(0);
IMP_Encoder_RegisterChn(0, 0);  // Channel 0 to group 0
IMP_Encoder_RegisterChn(0, 1);  // Channel 1 to group 0 - WRONG!
```

**Correct Official Pattern:**
```c
// Separate groups for each stream
IMP_Encoder_CreateGroup(0);  // Group for channel 0
IMP_Encoder_CreateGroup(1);  // Group for channel 1

IMP_Encoder_RegisterChn(0, 0);  // Channel 0 to group 0
IMP_Encoder_RegisterChn(1, 1);  // Channel 1 to group 1
```

### 3. JPEG Channel Implementation (MISSING)

**Add JPEG channels with offset pattern:**
```c
// JPEG channels use +4 offset to avoid conflicts
int jpeg_channel_0 = 4;  // For main stream snapshots
int jpeg_channel_1 = 5;  // For sub stream snapshots

// Create JPEG encoders
IMPEncoderChnAttr jpeg_attr;
IMP_Encoder_SetDefaultParam(&jpeg_attr, IMP_ENC_PROFILE_JPEG, 
                           IMP_ENC_RC_MODE_FIXQP, width, height, 
                           1, 0, 0, 25, 0);

IMP_Encoder_CreateChn(jpeg_channel_0, &jpeg_attr);
IMP_Encoder_CreateChn(jpeg_channel_1, &jpeg_attr);

// Register JPEG channels to their respective groups
IMP_Encoder_RegisterChn(0, jpeg_channel_0);  // JPEG for main to group 0
IMP_Encoder_RegisterChn(1, jpeg_channel_1);  // JPEG for sub to group 1
```

### 4. System Binding Pattern (FIX NEEDED)

**Current (PARTIALLY INCORRECT):**
```c
// Main stream binding - CORRECT
IMPCell fs_chn0 = {DEV_ID_FS, 0, 0};
IMPCell enc_chn0 = {DEV_ID_ENC, 0, 0};
IMP_System_Bind(&fs_chn0, &enc_chn0);

// Sub stream binding - INCORRECT CELL STRUCTURE
IMPCell fs_chn1 = {DEV_ID_FS, 1, 0};
IMPCell enc_chn1 = {DEV_ID_ENC, 0, 1};  // WRONG: should be {DEV_ID_ENC, 1, 0}
```

**Correct Official Pattern:**
```c
// Main stream binding
IMPCell fs_chn0 = {DEV_ID_FS, 0, 0};
IMPCell enc_chn0 = {DEV_ID_ENC, 0, 0};
IMP_System_Bind(&fs_chn0, &enc_chn0);

// Sub stream binding - CORRECTED
IMPCell fs_chn1 = {DEV_ID_FS, 1, 0};
IMPCell enc_chn1 = {DEV_ID_ENC, 1, 0};  // GROUP 1, not group 0
IMP_System_Bind(&fs_chn1, &enc_chn1);
```

### 5. Streaming Thread Fix (CRITICAL)

**Current (ONLY PROCESSES CHANNEL 0):**
```c
// Only processes channel 0 - channel 1 never used
int ret = encoder_process_frame(0, manager->rtsp_server);
```

**Required Fix:**
```c
// Process both channels in round-robin or parallel
int ret0 = encoder_process_frame(0, manager->rtsp_server);
int ret1 = encoder_process_frame(1, manager->rtsp_server);
```

### 6. Channel Start Sequence (FIX NEEDED)

**Current (ONLY STARTS CHANNEL 0):**
```c
IMP_Encoder_StartRecvPic(0);  // Only channel 0
```

**Required Fix:**
```c
IMP_Encoder_StartRecvPic(0);  // Main stream
IMP_Encoder_StartRecvPic(1);  // Sub stream
```

## Implementation Priority

### Phase 1: Fix Core Architecture (HIGH PRIORITY)
1. Fix encoder group creation (separate groups)
2. Fix system binding cells
3. Fix streaming thread to process both channels
4. Fix channel start sequence

### Phase 2: Add JPEG Support (MEDIUM PRIORITY)
1. Implement JPEG channel creation with +4 offset
2. Add JPEG snapshot functionality
3. Ensure no conflicts with video streams

### Phase 3: Optimization (LOW PRIORITY)
1. Fine-tune buffer sizes
2. Optimize polling intervals
3. Add error recovery

## Expected Results After Fix

✅ **Dual H.264 streams working simultaneously**
- Main stream: 1920x1080@25fps
- Sub stream: 640x360@25fps

✅ **JPEG snapshots without conflicts**
- Independent JPEG channels (4, 5)
- No interference with video streams

✅ **Stable long-term operation**
- No resource conflicts
- Proper resource cleanup
- Following official SDK patterns

## Files to Modify

1. `src/encoder.c` - Fix encoder group and channel creation
2. `src/imp_system.c` - Fix binding pattern  
3. `src/encoder.c` - Fix streaming thread processing
4. Add JPEG functionality to existing encoder module