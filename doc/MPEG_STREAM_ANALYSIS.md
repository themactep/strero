MPEG/H.264/H.265 Stream Analysis Tools
======================================

Several Linux-compatible software tools can perform comprehensive analysis of MPEG, H.264 (AVC), and H.265 (HEVC) video streams, including detection of encoding issues (e.g., syntax errors, bitstream non-compliance, quality degradation) and stream flow problems (e.g., buffer overflows, timestamp inconsistencies, bitrate irregularities). These range from open-source options for basic probing to commercial tools for in-depth visualization and validation. Below is a curated list based on their capabilities, with a focus on Linux support. Note that some tools are free/open-source, while others are paid but offer trials or Linux versions.

### Open-Source/Free Tools

- **FFmpeg/ffprobe**
  FFmpeg is a powerful, cross-platform command-line tool that includes ffprobe for analyzing video streams. It supports MPEG-1/2, H.264, and H.265, allowing you to inspect stream metadata, detect encoding errors (e.g., via debug logging or compliance checks), calculate quality metrics (e.g., PSNR, SSIM with filters), and identify flow issues like timestamp discontinuities or bitrate spikes. Use commands like `ffprobe -v error input.mp4` for error detection or `ffmpeg -i input.ts -v debug` for detailed decoding logs. It's ideal for scripting automated analysis.
  - Installation: `sudo apt install ffmpeg` (on Ubuntu/Debian).
  - Limitations: More focused on processing than visual analysis; requires command-line expertise.

- **MediaInfo**
  A lightweight, open-source tool for extracting detailed technical information from video files, including codec specifics, bitrate, resolution, and compliance flags for MPEG, H.264, and H.265 streams. It can highlight basic encoding issues (e.g., invalid profiles/levels) and flow problems (e.g., inconsistent frame rates or audio-video sync). Output is available in text, XML, or GUI formats. Use `mediainfo --fullscan input.mkv` for deeper scans.
  - Installation: `sudo apt install mediainfo`.
  - Limitations: Primarily informational; not for visual frame-by-frame analysis or advanced metrics.

- **VLC Media Player**
  An open-source player with built-in analysis features, supporting playback and inspection of MPEG, H.264, and H.265 streams. It allows frame-by-frame viewing, codec information display, and basic error detection during decoding (e.g., via logs or playback artifacts). Tools like the "Codec Information" window show stream details, and it can log issues like packet loss or buffer underruns. Useful for quick spot-checks on flow and encoding compatibility.
  - Installation: `sudo apt install vlc`.
  - Limitations: Better for playback verification than deep encoding diagnostics.

- **libde265**
  An open-source H.265 decoder library with example tools for bitstream analysis. It can parse and decode H.265 streams, detecting syntax errors or decoding failures. Compile and use the included `dec265` binary for command-line inspection, which reports issues like invalid NAL units or reference frame errors. It integrates with other tools like FFmpeg for hybrid analysis.
  - Installation: `sudo apt install libde265-dev` (build examples from source on GitHub).
  - Limitations: Primarily a library; requires custom scripting for comprehensive analysis.

### Commercial Tools with Linux Support

- **Elecard StreamEye**
  A professional video analysis tool that provides visual and metric-based inspection of MPEG-1/2, H.264, and H.265 streams. Features include frame-by-frame visualization of coding elements (e.g., motion vectors, slice boundaries), quality metrics (PSNR, SSIM, VMAF), buffer analysis (DPB/VBV), standard compliance checks, and bit distribution histograms. It excels at detecting encoding issues (e.g., quantization errors, non-compliant syntax) and flow problems (e.g., buffer overflows, reference frame mismatches). Supports automation via CLI and CSV exports.
  - Linux Support: Ubuntu 18.04/20.04/22.04, CentOS 7.6.
  - Pricing: Paid (trial available); download from the Elecard website.

- **Elecard Stream Analyzer**
  Focused on syntax and transport stream validation for MPEG, H.264, and H.265, including ETSI TR 101 290 compliance testing. It analyzes timestamps (PCR/PTS/DTS), bitrate, metadata, and PSI/SI tables, identifying issues like packet errors, multiplexing flaws, or HDR metadata inconsistencies. Great for stream flow diagnostics (e.g., timing errors) and encoding validation. Includes CLI for batch processing and CSV exports.
  - Linux Support: Ubuntu 18.04/20.04/22.04, CentOS 7.6.
  - Pricing: Paid (trial available); integrates with StreamEye.

- **SolveigMM Zond 265**
  A visual bitstream analyzer for H.265, H.264, and MPEG-2, offering frame navigation, bitrate histograms, HRD buffer simulation, and quality metrics (VMAF, PSNR). It detects conformance issues, syntax errors, and flow problems like buffer underflows. Features HDR analysis, CU/PU/TU overlays, and CLI/JSON reports for automation.
  - Linux Support: Ubuntu/debian-based (16.04+ for x86/x64).
  - Pricing: Paid (trial available); limited container support on Linux (e.g., VES formats primary).

### Additional Recommendations
- For network-streamed content (e.g., RTP/UDP), combine with **Wireshark** (open-source, `sudo apt install wireshark`) to inspect packet-level flow issues like jitter or loss, filtering for H.264/H.265 payloads.
Example: Capture RTSP traffic with `tshark -i any -f "tcp port 554" -w capture.pcap`, then analyze in GUI for H.264/H.265 RTP payloads.

- If you need automation or integration, tools like FFmpeg can be scripted with Python (e.g., using subprocess) for custom workflows.
- For purely open-source advanced analysis, consider building on libraries like **h264bitstream** (GitHub) for H.264 parsing, though it requires development effort.

If your streams are in specific containers (e.g., TS, MP4) or you have sample files, provide more details for tailored command examples.
