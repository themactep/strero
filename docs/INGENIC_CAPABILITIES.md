### Ingenic Platform Capability per SoC family

**Status**: ✅ Verified against SDK headers and HAL verification findings
**Source**: Based on systematic verification of actual Ingenic SDK headers

| Feature | T10 | T20 | T21 | T23 | T30 | T31 | T40 | T41 | C100 | Notes |
|---------|-----|-----|-----|-----|-----|-----|-----|-----|------|-------|
| **Video Encoding** |
| H264 | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | Universal support |
| H265 | ❌ | ❌ | ❌ | ❌ | ✅ | ✅ | ✅ | ✅ | ✅ | T30+ only |
| Buffer Sharing | ❌ | ❌ | ❌ | ❌ | ❌ | ✅ | ✅ | ✅ | ✅ | T31+ only |
| Advanced JPEG Quality | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ✅ | ❌ | T41 only |
| **Audio Processing** |
| AGC | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | Universal support |
| ALC Gain | ❌ | ❌ | ✅ | ❌ | ❌ | ✅ | ❌ | ❌ | ✅ | T21, T31, C100 |
| **ISP Features** |
| Advanced ISP | ❌ | ❌ | ❌ | ✅ | ❌ | ✅ | ✅ | ✅ | ✅ | T23+ (excl. T30) |
| Sinter Strength | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | Universal support |
| AE Compensation | ✅ | ✅ | ❌ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | All except T21 |
| DRC Strength | ❌ | ❌ | ✅ | ✅ | ❌ | ✅ | ✅ | ✅ | ✅ | T21+ (excl. T30) |
| Backlight Comp | ❌ | ❌ | ❌ | ✅ | ❌ | ✅ | ✅ | ✅ | ✅ | T23+ (excl. T30) |
| **System Features** |
| Frame Rotation | ❌ | ❌ | ❌ | ❌ | ❌ | ✅ | ❌ | ❌ | ❌ | T31 only |
| Zero Copy | ❌ | ❌ | ❌ | ✅ | ❌ | ✅ | ✅ | ✅ | ✅ | T23+ (excl. T30) |
| Motion Detection | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | Universal support |
| **Cropping Features** |
| Framesource Cropping | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | Universal support |
| Encoder Cropping | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | Universal support |
| ISP Front Cropping | ❌ | ❌ | ❌ | ✅ | ❌ | ✅ | ❌ | ❌ | ✅ | T23, T31, C100 |

## Architecture Groups

### XBurst1 (Old Generation)
- **Platforms**: T10, T20, T21, T23, T30, T31, C100
- **Characteristics**: Mature platform with stable feature set
- **Notable**: T31 has unique frame rotation support

### XBurst2 (New Generation)
- **Platforms**: T40, T41, A1
- **Characteristics**: Enhanced performance, advanced encoding features
- **Notable**: T41 has exclusive advanced JPEG quality control

## Verification Status

✅ **Verified Features** (confirmed against SDK headers):
- H265 Support, AGC, ALC Gain, Buffer Sharing, Advanced ISP
- Sinter Strength, AE Compensation, DRC Strength, Backlight Compensation
- Frame Rotation, Zero Copy, Motion Detection, All Cropping Features

📋 **Verification Sources**:
- SDK headers from `/include/[PLATFORM]/[VERSION]/[LANG]/imp/`
- HAL verification findings documented in `HAL_VERIFICATION_FINDINGS.md`
- Systematic header analysis across all platform versions

## Key Findings

### Universal Features (All Platforms)
- **H264 Encoding**: Standard across all Ingenic platforms
- **AGC Audio**: Universal audio gain control support
- **Sinter Strength**: ISP noise reduction (corrected - T21 also supported)
- **Motion Detection**: IVS motion detection (corrected - T21 also supported)
- **Framesource & Encoder Cropping**: Universal cropping capabilities

### Platform-Specific Features
- **H265**: T30+ only (T23 explicitly marked "Unsupport H.265")
- **Frame Rotation**: T31 exclusive feature
- **Advanced JPEG**: T41 exclusive feature
- **ALC Gain**: Scattered support (T21, T31, C100)

### Architecture Differences
- **XBurst1**: More varied feature support, platform-specific quirks
- **XBurst2**: More consistent feature set, enhanced performance
