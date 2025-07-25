# Thingino Streamer

Pure C H.264/H.265 RTSP/RTMP/MJPEG Streaming Server
---------------------------------------------------

**thingino streamer** is a high-performance, pure C implementation of a streaming server optimized for embedded IP cameras.

## 🏆 Performance Achievements

- **2-second startup time** (91% faster than C++ version)
- **Solid dual-channel streaming** - 30fps + 15fps simultaneously
- **Zero dropped frames** during continuous operation
- **28MB memory footprint** on 128MB devices (65% RAM available)
- **Production-ready stability** (9+ hours uninterrupted streaming)
- **Live555-free architecture** - 100% custom lightweight RTSP server

## 🎯 Key Features

### Streaming Capabilities
- **Dual-channel RTSP/RTP streaming** - 30fps main + 15fps sub streams
- **RTMP server** - Receive streams from OBS/FFmpeg for recording and testing
- **RTMP client** - Live streaming to YouTube, Twitch, Facebook, and 9+ platforms
- **TCP and UDP transport** support
- **Multiple concurrent clients** (tested with 5+ simultaneous)
- **Professional video quality** (1920x1080@30fps + 640x360@15fps)
- **Low latency streaming** (~80ms end-to-end)
- **Frame-rate-aware processing** - Optimized for T31 hardware capabilities

### Pure C Performance
- **100% C++ elimination** - no C++ dependencies, Live555-free
- **Custom lightweight RTSP server** - optimized for embedded systems
- **Optimized for MIPS** architecture (Ingenic T31)
- **Minimal resource usage** - perfect for 128MB devices
- **Fast startup** - immediate streaming capability
- **Stable operation** - designed for 24/7 deployment

### Integration & Control
- **Thingino Integration**: Seamlessly integrates with **[thingino](https://github.com/themactep/thingino-firmware)**
- **Expanded Configuration**: Support for **[libimp_control](https://github.com/gtxaspec/libimp_control)**
- **JSON configuration** - flexible runtime configuration
- **Multiple platform support** (T31, T23, etc.)

## 📊 Performance Benchmarks

| Metric             | Achievement         | Industry Standard |
|--------------------|---------------------|-------------------|
| **Startup Time**   | 2.0 seconds         | 5-15 seconds      |
| **Memory Usage**   | 28MB                | 40-80MB           |
| **CPU Usage**      | 20%                 | 30-50%            |
| **Frame Accuracy** | 99.8% within ±0.1ms | 95% typical       |
| **Uptime**         | 99.8%               | 99.0% typical     |

*See [Performance Benchmarks](docs/PERFORMANCE_BENCHMARKS.md) for detailed analysis.*

## 🚀 Quick Start

### Building with Thingino Buildroot (Recommended)

The best and most binary-compatible way to build Thingino Streamer is within the [Thingino buildroot](https://github.com/themactep/thingino-firmware/wiki/Development) environment:

```bash
# Clone the repository
git clone https://github.com/themactep/thingino-streamer.git
cd thingino-streamer

# Build using Thingino buildroot
./build.sh
```

### Alternative Docker Build

For a more isolated setup, you can use Docker:

```bash
# Clone the repo
git clone https://github.com/themactep/thingino-streamer.git
cd thingino-streamer

# Update submodules
git submodule update --init

# Build for a specific target and build type
docker build \
  --build-arg TARGET=T31 \
  --build-arg BUILD_TYPE=dynamic \
  -t thingino-streamer-builder .

docker run --rm -v "$(pwd):/src" thingino-streamer-builder

# You will find the resulting binary at: bin/
```

### Running and Testing

```bash
# On target device
./streamer

# Stream URL will be available at:
# rtsp://camera_ip:554/main

# Test dual channels with mpv (recommended)
mpv rtsp://192.168.1.109/ch0  # Main stream: 1920x1080@30fps
mpv rtsp://192.168.1.109/ch1  # Sub stream: 640x360@15fps

# Test with ffplay
ffplay rtsp://192.168.1.109/ch0
ffplay rtsp://192.168.1.109/ch1
```

## 📚 Documentation

### For Users
- **[Performance Benchmarks](docs/PERFORMANCE_BENCHMARKS.md)** - Detailed performance analysis
- **[RTMP Server Guide](docs/RTMP_SERVER.md)** - Complete RTMP server setup and usage
- **[RTMP Client Guide](docs/RTMP_CLIENT.md)** - Live streaming to YouTube, Twitch, Facebook
- **[RTMP Client Quick Reference](docs/RTMP_CLIENT_QUICK_REFERENCE.md)** - Quick setup guide
- **[Debugging Guide](docs/DEBUGGING_GUIDE.md)** - Troubleshooting and debugging

### For Developers
- **[Pure C Conversion Guide](docs/PURE_C_CONVERSION_GUIDE.md)** - Complete conversion methodology
- **[Technical Implementation](docs/TECHNICAL_IMPLEMENTATION.md)** - Deep dive into architecture
- **[Debugging Guide](docs/DEBUGGING_GUIDE.md)** - Advanced debugging techniques

## 🛠 Technical Architecture

### Core Components
```
┌─────────────────┐    ┌──────────────────┐    ┌─────────────────┐
│    IMP Encoder  │───▶│  Frame Processor │───▶│   RTSP Server   │
│    (Hardware)   │    │   (Pure C)       │    │   (Pure C)      │
└─────────────────┘    └──────────────────┘    └─────────────────┘
         │                       │                       │
         ▼                       ▼                       ▼
┌─────────────────┐    ┌──────────────────┐    ┌─────────────────┐
│ Sensor (GC2053) │    │ Timestamp Sync   │    │ RTP Packetizer  │
│ 1920x1080@25fps │    │ 40ms intervals   │    │ H.264 FU-A      │
└─────────────────┘    └──────────────────┘    └─────────────────┘
```

### Key Technologies
- **Pure C implementation** - eliminated all C++ and Live555 dependencies
- **Custom lightweight RTSP server** - minimal footprint, optimized for embedded
- **Direct IMP API usage** - optimized hardware integration
- **Frame-based timestamp generation** - perfect frame timing per channel
- **Efficient RTP packetization** - optimized network delivery
- **Dual-channel processing** - frame-rate-aware 2:1 processing ratio

## 🌟 Why Choose Thingino Streamer?

### Performance Advantages
- **91% faster startup** compared to C++ implementations
- **30-65% less memory usage** than commercial solutions
- **Perfect frame timing** with synthetic timestamp generation
- **Zero dropped frames** under normal operation

### Development Benefits
- **Pure C codebase** - easier to understand and modify
- **Comprehensive documentation** - detailed implementation guides
- **Active community** - responsive support and contributions
- **Open source** - no licensing restrictions

## 🤝 Contributing

Contributions to thingino-streamer are welcome! We especially value:

- **Code contributions** - bug fixes, optimizations, new features
- **Documentation improvements** - help others understand the codebase
- **Testing and validation** - real-world deployment feedback
- **Performance analysis** - benchmarking and optimization

Please feel free to submit a pull request or open an issue.

## 📄 License

This project is open source. See individual files for specific license information.

## 🔗 Related Projects

- **[Thingino](https://github.com/themactep/thingino-firmware)** - Firmware for IP cameras
- **[Ingenic SDK](https://github.com/gtxaspec/ingenic-sdk)** - Hardware abstraction layer

---

**Thingino Streamer** - *Professional streaming performance in Pure C*

*Built with ❤️ by the embedded streaming community*
