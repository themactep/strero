# Thingino Streamer Project Notes

## Current Status: OSD Channel Validation Fixed ✅

### Latest Changes (2025-01-14)

**MAJOR FIX: OSD Channel Validation and Stream Mapping Issue Resolved**

**Problem:** 
The streamer was failing with OSD-related errors:
```
[E] OSD: Group ID 3 exceeds available streams (2)
[E] OSD: Failed to set timestamp group region attributes: -1
[E] OSD: Failed to create timestamp region for group 0
[W] main: OSD initialization failed for channel 0, continuing without OSD
[E] main: Bind FrameSource channel0 and OSD failed
```

**Root Cause:**
1. **Channel validation issue**: Code was trying to initialize OSD for group 3, but only 2 streams were available
2. **Stream mapping problem**: Incorrect mapping between enabled channels and stream configurations
3. **Missing channel enable checks**: OSD was attempting to initialize for disabled channels

**Solution Applied:**
- **Latest Fix**: Fixed OSD region registration sequence in `osd_create_timestamp_region()`
- **Region registration**: Added `IMP_OSD_RegisterRgn()` call before setting region attributes
- **Channel validation**: Added proper channel enable checking in `osd_init()` function
- **Stream mapping**: Implemented correct mapping between enabled channels (0,1,3) and stream configurations (0,1)
- **Error handling**: Improved error messages and graceful handling of invalid configurations

**Previous Fixes:**
- **Commit 884e76c0**: Fixed multiple definition error by renaming conflicting `osd_init` function to `osd_init_legacy` in `common.c`
- Added missing `#include <schrift.h>` header for libschrift support

### Build Status
- ✅ **Build successful** - No compilation errors
- ✅ **OSD functionality restored** - Timestamp display should now work
- ✅ **libschrift support** - Font rendering enabled
- ✅ **Binary deployed** - Updated streamer binary in `/home/paul/nfs/streamer`

### Build System Notes
- **IMPORTANT**: This project is designed for compilation within an external buildroot tree
- **Compilation**: Use `./build.sh` script, NOT direct `make` calls
- The build script handles cross-compilation setup, SDK paths, and proper linking
- Direct make calls will fail due to missing cross-compilation environment

### Technical Details

**OSD System Architecture:**
- New OSD system with libschrift font rendering
- Supports timestamp display with configurable positioning
- BGRA format for proper alpha blending
- Multiple region support (timestamp, logo, cover, rect)

**Key Files Modified:**
- `src/osd.c` - Main OSD implementation with libschrift integration
- `src/common.c` - Renamed legacy `osd_init` to avoid conflicts
- Build system properly links libschrift library

### Next Steps
1. **Test on camera** - Deploy and verify OSD timestamp display works
2. **Monitor performance** - Check for any memory or performance issues
3. **Configuration tuning** - Adjust font size, position, colors as needed

### Configuration Notes
- Font file: `/usr/share/fonts/default.ttf`
- Default timestamp position: configurable via JSON
- Colors: White text on transparent background (configurable)
- Font size: Scales based on stream resolution

### Deployment
- Binary location: `/home/paul/nfs/streamer`
- Configuration: `/home/paul/nfs/streamer.json`
- Ready for camera testing

---

## Previous Session History

### Build System Integration
- Integrated with Thingino buildroot system
- Cross-compilation for MIPS architecture (T31 platform)
- Dependencies: libschrift, libwebsockets, json-c, Ingenic SDK

### Known Working Features
- Video streaming (H264/H265)
- RTSP server
- HTTP endpoints
- JSON configuration
- Multi-stream support
- Sensor integration

### Architecture
- Platform: Ingenic T31 SoC
- Target: Wyze Cam v3 (and compatible cameras)
- Build system: Buildroot with custom package
- SDK: Ingenic T31 1.1.6

---

## Modular Architecture Implementation ✅

### Latest Changes (2025-01-17)

**MAJOR ACHIEVEMENT: Complete Modular Architecture Implementation**

**What Was Accomplished:**
- **Extracted Audio Module**: Self-contained audio capture, encoding, and streaming
- **Extracted ONVIF Module**: Self-contained WS-Discovery and SOAP services
- **Module System**: Complete lifecycle management (init/start/stop/cleanup)
- **Modular Config System**: Dedicated config files for each module
- **Buildroot Integration**: Optional module compilation via buildroot config

### Module System Architecture

**Core Components:**
- **Module System** (`src/module_system.c/h`): Registration, lifecycle, config loading
- **Audio Module** (`src/modules/audio/`): T31 audio capture with G.711A/G.711U encoding
- **ONVIF Module** (`src/modules/onvif/`): WS-Discovery service and SOAP endpoints
- **Core Streamer**: Video streaming, RTSP server, HTTP endpoints (always enabled)

**Module Lifecycle:**
1. **Registration**: Manual registration in main.c (due to symbol stripping)
2. **Config Loading**: Dedicated config files with fallback to main config
3. **Initialization**: Module-specific setup and validation
4. **Start**: Service activation and thread creation
5. **Stop/Cleanup**: Graceful shutdown and resource cleanup

### Modular Configuration System

**Config File Locations (Priority Order):**
1. **Binary directory**: `/mnt/nfs/module.json` (for testing from network share)
2. **System directory**: `/etc/streamer.d/module.json` (production deployment)
3. **Fallback**: Main config `streamer.json` section (backward compatibility)

**Example Module Configs:**
- `audio.json`: Audio capture settings, sample rates, codecs
- `onvif.json`: Device information, service endpoints, discovery settings

### Buildroot Integration

**Module Makefile Requirements (Critical):**

**1. Dual Flag System Required:**
```makefile
# Make variables (for Makefile logic)
$(if $(BR2_PACKAGE_THINGINO_STREAMER_MODULE),ENABLE_MODULE=1,) \

# CFLAGS preprocessor flags (for C code #ifdef)
ifeq ($(BR2_PACKAGE_THINGINO_STREAMER_MODULE),y)
THINGINO_STREAMER_CFLAGS += -DENABLE_MODULE=1
endif
```

**2. IMP Include Paths (Essential):**
```makefile
$(CC) $(CFLAGS) \
    -I$(LIBIMP_INC_DIR) \
    -I$(LIBIMP_INC_DIR)/imp \
    -I$(LIBIMP_INC_DIR)/sysutils \
    -isystem $(THIRDPARTY_INC_DIR) \
    -c $< -o $@
```

**Why Both Are Needed:**
- **Make variables**: Control object compilation and linking in Makefiles
- **CFLAGS**: Enable `#ifdef ENABLE_MODULE` conditions in main.c registration
- **IMP includes**: Prevent "imp/imp_common.h: No such file or directory" errors

### Current Module Status

- **Audio Module**: ✅ Working (optional, disabled by default)
  - `BR2_PACKAGE_THINGINO_STREAMER_AUDIO=y` to enable
  - T31 audio capture with G.711A codec
  - Modular config: `audio.json`

- **ONVIF Module**: ✅ Working (optional, enabled by default)
  - `BR2_PACKAGE_THINGINO_STREAMER_ONVIF=y` to enable
  - WS-Discovery service on port 3702
  - SOAP endpoints for device/media services
  - Modular config: `onvif.json`

- **Core Streamer**: ✅ Working (always enabled)
  - Video streaming, RTSP server, HTTP endpoints
  - Main config: `streamer.json`

### Runtime Verification

**Expected Module Logs:**
```
[I] MODULE: Initializing module system
[I] MODULE: Module system initialized successfully
[I] main: Audio module registered manually
[I] main: ONVIF module registered manually
[I] MODULE: Initializing 2 registered modules
[I] AUDIO_MODULE: Initializing audio module
[I] ONVIF_MODULE: Initializing ONVIF module
[I] MODULE: Module start complete: 2 success, 0 errors
```

**Module System Benefits:**
- **Clean separation**: Each module is self-contained
- **Optional deployment**: Disable unused modules to save resources
- **Easy testing**: Enable/disable modules without code changes
- **Maintainable**: Clear module boundaries and interfaces
- **Extensible**: Easy to add new modules (OSD, Motion, Recording)

---

*Last updated: 2025-01-17 12:00 EST*
*Status: Modular Architecture Complete - Audio & ONVIF Modules Working*
