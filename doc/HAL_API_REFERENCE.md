# IMPPlatformHAL API Reference

## Overview

The IMPPlatformHAL class provides a unified interface for platform-specific operations across different Ingenic SoC platforms. This document serves as a complete API reference.

## Platform Detection API

### Basic Platform Detection

#### `static bool isT10()`
- **Purpose**: Detect T10 platform
- **Returns**: `true` if running on T10, `false` otherwise
- **Usage**: Legacy platform detection

#### `static bool isT20()`
- **Purpose**: Detect T20 platform  
- **Returns**: `true` if running on T20, `false` otherwise
- **Usage**: Legacy platform detection

#### `static bool isT21()`
- **Purpose**: Detect T21 platform
- **Returns**: `true` if running on T21, `false` otherwise
- **Usage**: Platform with FPS verification requirements

#### `static bool isT23()`
- **Purpose**: Detect T23 platform
- **Returns**: `true` if running on T23, `false` otherwise
- **Usage**: First platform with H265 support

#### `static bool isT30()`
- **Purpose**: Detect T30 platform
- **Returns**: `true` if running on T30, `false` otherwise
- **Usage**: Legacy platform detection

#### `static bool isT31()`
- **Purpose**: Detect T31 platform
- **Returns**: `true` if running on T31, `false` otherwise
- **Usage**: Most common platform, supports many features

#### `static bool isT40()`
- **Purpose**: Detect T40 platform
- **Returns**: `true` if running on T40, `false` otherwise
- **Usage**: Advanced platform with extended sensor info

#### `static bool isT41()`
- **Purpose**: Detect T41 platform
- **Returns**: `true` if running on T41, `false` otherwise
- **Usage**: Advanced platform with extended sensor info

#### `static bool isC100()`
- **Purpose**: Detect C100 platform
- **Returns**: `true` if running on C100, `false` otherwise
- **Usage**: Newer platform variant

### Architecture Detection

#### `static bool isXBurst1()`
- **Purpose**: Detect XBurst1 architecture (T10-T31)
- **Returns**: `true` if XBurst1 architecture, `false` otherwise
- **Usage**: Architecture-level feature detection

#### `static bool isXBurst2()`
- **Purpose**: Detect XBurst2 architecture (T40+)
- **Returns**: `true` if XBurst2 architecture, `false` otherwise
- **Usage**: Architecture-level feature detection

## Feature Capability API

### Video Encoding Capabilities

#### `static bool supportsH265()`
- **Purpose**: Check H265 encoding support
- **Returns**: `true` if H265 encoding available
- **Platforms**: T30, T31, T40, T41, C100 (T23 does NOT support H265)
- **Usage**: Before enabling H265 encoding

#### `static bool supportsJPEGQuality()`
- **Purpose**: Check JPEG quality control support
- **Returns**: `true` if JPEG quality settings available
- **Platforms**: T40, T41, C100
- **Usage**: Before setting JPEG quality parameters

#### `static bool supportsBufferSharing()`
- **Purpose**: Check encoder buffer sharing support
- **Returns**: `true` if buffer sharing available
- **Platforms**: T31, T40, T41, C100
- **Usage**: Before enabling shared JPEG channel

#### `static bool supportsAdvancedJPEGQuality()`
- **Purpose**: Check advanced JPEG quality control support
- **Returns**: `true` if advanced JPEG quality control available
- **Platforms**: T41 only (has IMPEncoderJpegeQl, IMP_Encoder_SetJpegeQl)
- **Usage**: Before using advanced JPEG quantization table controls

### Audio Processing Capabilities

#### `static bool supportsAGC()`
- **Purpose**: Check Automatic Gain Control support
- **Returns**: `true` if AGC available
- **Platforms**: All platforms (T10-T41, C100) - verified in SDK headers
- **Usage**: Before enabling AGC in audio processing

#### `static bool supportsALCGain()`
- **Purpose**: Check ALC gain control support
- **Returns**: `true` if ALC gain control available
- **Platforms**: T21, T31, T40, T41, C100
- **Usage**: Before setting ALC gain parameters

### ISP Tuning Capabilities

#### `static bool supportsSinterStrength()`
- **Purpose**: Check sinter strength tuning support
- **Returns**: `true` if sinter strength control available
- **Platforms**: All platforms (T10-T41, C100) - verified in SDK headers
- **Usage**: Before setting sinter strength values

#### `static bool supportsAECompensation()`
- **Purpose**: Check auto-exposure compensation support
- **Returns**: `true` if AE compensation available
- **Platforms**: All platforms except T21
- **Usage**: Before setting AE compensation values

#### `static bool supportsAdvancedISP()`
- **Purpose**: Check advanced ISP features support
- **Returns**: `true` if advanced ISP features available
- **Platforms**: T23, T31, T40, T41, C100
- **Usage**: Before using hue, defog, DPC controls

#### `static bool supportsDRCStrength()`
- **Purpose**: Check DRC strength control support
- **Returns**: `true` if DRC strength control available
- **Platforms**: T21, T23, T31, T40, T41, C100
- **Usage**: Before setting DRC strength values

#### `static bool supportsBacklightCompensation()`
- **Purpose**: Check backlight compensation support
- **Returns**: `true` if backlight compensation available
- **Platforms**: T23, T31, T40, T41, C100
- **Usage**: Before enabling backlight compensation

### System Capabilities

#### `static bool supportsFrameRotation()`
- **Purpose**: Check frame rotation support in framesource
- **Returns**: `true` if frame rotation available
- **Platforms**: T31 (but not C100)
- **Usage**: Before setting frame rotation

#### `static bool supportsZeroCopy()`
- **Purpose**: Check zero-copy buffer operations support
- **Returns**: `true` if zero-copy available
- **Platforms**: T31, T40, T41, C100
- **Usage**: Before enabling zero-copy streaming

#### `static bool requiresExtendedSensorInfo()`
- **Purpose**: Check if platform requires extended sensor info
- **Returns**: `true` if extended sensor info required
- **Platforms**: T40, T41
- **Usage**: Before setting sensor configuration

#### `static bool requiresFPSVerification()`
- **Purpose**: Check if platform requires FPS verification after setting
- **Returns**: `true` if FPS verification required
- **Platforms**: T21
- **Usage**: After setting sensor FPS

## API Abstraction Functions

### Sensor Management

#### `static int addSensorToISP(IMPSensorInfo* sinfo)`
- **Purpose**: Add sensor to ISP with platform-specific handling
- **Parameters**: `sinfo` - Sensor information structure
- **Returns**: 0 on success, negative on error
- **Usage**: Unified sensor registration across platforms

#### `static int enableSensor(IMPSensorInfo* sinfo)`
- **Purpose**: Enable sensor with platform-specific handling
- **Parameters**: `sinfo` - Sensor information structure  
- **Returns**: 0 on success, negative on error
- **Usage**: Unified sensor enabling across platforms

#### `static int disableSensor()`
- **Purpose**: Disable sensor with platform-specific handling
- **Returns**: 0 on success, negative on error
- **Usage**: Unified sensor disabling across platforms

#### `static int deleteSensorFromISP(IMPSensorInfo* sinfo)`
- **Purpose**: Remove sensor from ISP with platform-specific handling
- **Parameters**: `sinfo` - Sensor information structure
- **Returns**: 0 on success, negative on error
- **Usage**: Unified sensor cleanup across platforms

### Encoder Data Access

#### `static void* getEncoderStreamData(int encChn)`
- **Purpose**: Get encoder stream data with unified interface
- **Parameters**: `encChn` - Encoder channel number
- **Returns**: Pointer to stream data, NULL on error
- **Usage**: Platform-agnostic stream data access

#### `static uint32_t getEncoderDataSize(void* stream_data)`
- **Purpose**: Get size of encoder data
- **Parameters**: `stream_data` - Stream data pointer from getEncoderStreamData
- **Returns**: Data size in bytes
- **Usage**: Get stream data size across platforms

#### `static uint64_t getEncoderTimestamp(void* stream_data)`
- **Purpose**: Get timestamp from encoder data
- **Parameters**: `stream_data` - Stream data pointer from getEncoderStreamData
- **Returns**: Timestamp value
- **Usage**: Unified timestamp extraction

#### `static void releaseEncoderStream(int encChn)`
- **Purpose**: Release encoder stream with platform-specific handling
- **Parameters**: `encChn` - Encoder channel number
- **Usage**: Unified stream cleanup

### Timestamp Management

#### `static uint64_t getMonotonicTimestamp()`
- **Purpose**: Get monotonic timestamp for A/V synchronization
- **Returns**: Monotonic timestamp value
- **Usage**: Unified timestamp source for proper A/V sync

## Usage Examples

### Basic Platform Detection
```cpp
if (IMPPlatformHAL::isT31()) {
    LOG_INFO("Running on T31 platform");
}

if (IMPPlatformHAL::isXBurst2()) {
    LOG_INFO("Running on XBurst2 architecture");
}
```

### Feature Capability Checking
```cpp
// Check before using H265
if (IMPPlatformHAL::supportsH265()) {
    // Enable H265 encoding
    stream->format = "H265";
}

// Check before using AGC
if (IMPPlatformHAL::supportsAGC()) {
    // Enable AGC in audio processing
    enableAGC();
}
```

### Unified API Usage
```cpp
// Unified sensor management
IMPSensorInfo sinfo = createSensorInfo();
int ret = IMPPlatformHAL::addSensorToISP(&sinfo);
if (ret == 0) {
    ret = IMPPlatformHAL::enableSensor(&sinfo);
}

// Unified encoder data access
void* data = IMPPlatformHAL::getEncoderStreamData(channel);
if (data) {
    uint32_t size = IMPPlatformHAL::getEncoderDataSize(data);
    uint64_t timestamp = IMPPlatformHAL::getEncoderTimestamp(data);
    
    // Process data...
    
    IMPPlatformHAL::releaseEncoderStream(channel);
}

// Unified timestamp for A/V sync
uint64_t timestamp = IMPPlatformHAL::getMonotonicTimestamp();
```

### Error Handling
```cpp
// Always check return values for API functions
int ret = IMPPlatformHAL::addSensorToISP(&sinfo);
if (ret != 0) {
    LOG_ERROR("Failed to add sensor to ISP: " << ret);
    return ret;
}

// Check capabilities before using features
if (!IMPPlatformHAL::supportsH265()) {
    LOG_WARNING("H265 not supported on this platform");
    // Fall back to H264
    stream->format = "H264";
}
```

## Implementation Notes

### Thread Safety
- All HAL functions are thread-safe
- Platform detection results are cached
- No global state modification in capability functions

### Performance
- Platform detection uses compile-time macros when possible
- Capability functions are lightweight boolean checks
- API abstraction functions have minimal overhead

### Error Handling
- API functions return standard error codes
- Capability functions never fail (always return boolean)
- Platform detection functions never fail (always return boolean)

## Migration from Direct Platform Checks

### Before (Direct Platform Checks)
```cpp
#ifdef PLATFORM_T31
if (feature_enabled) {
    use_t31_api();
}
#elif defined(PLATFORM_T40)
if (feature_enabled) {
    use_t40_api();
}
#endif
```

### After (HAL Usage)
```cpp
if (IMPPlatformHAL::supportsFeature()) {
    use_unified_api();
}
```

This approach provides better maintainability, cleaner code, and easier platform support.
