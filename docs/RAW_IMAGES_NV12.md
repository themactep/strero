RAW IMAGES
==========

NV12 Format
-----------

To view an NV12 raw file on Linux, you need a tool that supports raw YUV formats, specifically NV12, and allows you to specify parameters like resolution and pixel format. Below are several methods to achieve this, using popular tools available on Linux. Since NV12 is a raw YUV 4:2:0 semi-planar format, you must know the file’s resolution (width and height) and frame rate (if it’s a video) to view it correctly.

### Prerequisites
- **Know the Resolution**: You must know the width and height of the NV12 file (e.g., 1920x1080). Unlike formatted video files, raw YUV files lack metadata, so you need to provide this information.
- **File Size Check**: For a single NV12 frame, the file size should be `width * height * 1.5` bytes (since each pixel uses 1.5 bytes in NV12). For example, a 1920x1080 frame should be `1920 * 1080 * 1.5 = 3,110,400` bytes. For a video, multiply by the number of frames.
- **Tools**: Ensure you have one of the following tools installed: FFmpeg (`ffplay`), YUView, or GStreamer.

### Method 1: Using `ffplay` (FFmpeg)
`ffplay`, part of the FFmpeg suite, is a versatile tool for playing raw video files, including NV12. Here’s how to use it:

1. **Install FFmpeg**:
   Ensure FFmpeg is installed on your Linux system:
   ```bash
   sudo apt install ffmpeg  # For Ubuntu/Debian
   sudo dnf install ffmpeg  # For Fedora
   sudo pacman -S ffmpeg    # For Arch Linux
   ```

2. **Play the NV12 File**:
   Use the following command, replacing `WIDTHxHEIGHT` with the resolution of your NV12 file (e.g., `1920x1080`) and `input.yuv` with your file’s name:
   ```bash
   ffplay -f rawvideo -pixel_format nv12 -video_size WIDTHxHEIGHT -i input.yuv
   ```
   - `-f rawvideo`: Specifies the input is a raw video file.
   - `-pixel_format nv12`: Sets the pixel format to NV12.
   - `-video_sizes WIDTHxHEIGHT`: Defines the resolution (e.g., `1920x1080`).
   - `-i input.yuv`: Specifies the input file.

   Example:
   ```bash
   ffplay -f rawvideo -pixel_format nv12 -video_size 1920x1080 -i image0.nv12
   ```

   If the file is a video, you may need to specify the frame rate with `-r` (e.g., `-r 30` for 30 fps):
   ```bash
   ffplay -f rawvideo -pixel_format nv12 -video_size 1920x1080 -r 30 -i test_nv12.yuv
   ```

   **Note**: If your NV12 file uses a tiled format (e.g., `nv12-64z32`), `ffplay` may not support it directly. You might need to convert it first (see Method 4) or use a tool like GStreamer that supports tiled formats.[](https://ffmpeg.org/pipermail/ffmpeg-user/2016-April/031925.html)

### Method 2: Using YUView
YUView is an open-source YUV viewer with advanced features for analyzing and viewing raw YUV files, including NV12.

1. **Install YUView**:
   On Ubuntu 22.04 or newer:
   ```bash
   sudo apt install yuview
   ```
   For other Linux distributions, you can install it via Flatpak:
   ```bash
   flatpak install flathub org.ient.YUView
   ```
   Alternatively, download precompiled binaries or build from source: https://github.com/IENT/YUView[](https://github.com/IENT/YUView)

2. **Open the NV12 File**:
   - Launch YUView.
   - Go to `File > Open` and select your NV12 file.
   - In the dialog, specify the resolution (width and height), pixel format (`NV12`), and frame rate (if applicable).
   - Click `OK` to view the file.

   YUView supports zooming, frame navigation, and analysis tools like histograms, making it ideal for detailed inspection.

### Method 3: Using Vooya
Vooya is another YUV player that supports NV12 and other raw video formats. It’s free on Linux and offers features like zooming and diff views.

1. **Install Vooya**:
   Download the Linux version from https://www.offminor.de/ and follow the installation instructions. It’s available as a portable binary or AppImage.

2. **Open the NV12 File**:
   - Run Vooya.
   - Open the file via `File > Open` or drag and drop.
   - Specify the resolution, pixel format (`NV12`), and frame rate in the settings.
   - Play or navigate through the frames.

   Vooya is particularly useful for scientific and film environments, supporting a wide range of YUV formats.[](https://www.offminor.de/)

### Method 4: Convert NV12 to a Viewable Format
If you can’t view the NV12 file directly, convert it to a more common format (e.g., PNG, MP4) using `ffmpeg` and view it with a standard image or video player.

1. **Convert NV12 to PNG** (for a single frame or image):
   ```bash
   ffmpeg -f rawvideo -pixel_format nv12 -video_size WIDTHxHEIGHT -i input.yuv -pixel_format rgb24 output.png
   ```
   Example:
   ```bash
   ffmpeg -f rawvideo -pixel_format nv12 -video_size 1920x1080 -i test_nv12.yuv -pixel_format rgb24 output.png
   ```
   Then open `output.png` with any image viewer (e.g., `eog`, `gimp`).

2. **Convert NV12 to MP4** (for video):
   ```bash
   ffmpeg -f rawvideo -pixel_format nv12 -video_size WIDTHxHEIGHT -r FRAMERATE -i input.yuv -c:v libx264 -pixel_format yuv420p output.mp4
   ```
   Example:
   ```bash
   ffmpeg -f rawvideo -pixel_format nv12 -video_size 1920x1080 -r 30 -i test_nv12.yuv -c:v libx264 -pixel_format yuv420p output.mp4
   ```
   Play `output.mp4` with a video player like VLC or MPV.

   **Note**: Ensure you know the resolution and frame rate. If the output looks incorrect, double-check these parameters.[](https://community.nxp.com/t5/i-MX-Processors-Knowledge-Base/Useful-tools-to-convert-image-format-on-PC/ta-p/1101350)

### Method 5: Using GStreamer
GStreamer can handle NV12 files, including some tiled formats, and is useful if `ffplay` fails.

1. **Install GStreamer**:
   ```bash
   sudo apt install gstreamer1.0-tools gstreamer1.0-plugins-base gstreamer1.0-plugins-good
   ```

2. **Play the NV12 File**:
   Use a pipeline like this, replacing `WIDTH`, `HEIGHT`, and `input.yuv`:
   ```bash
   gst-launchDIYlaunch-1.0 filesrc location=input.yuv ! videoparse width=WIDTH height=HEIGHT framerate=FRAMERATE format=nv12 ! videoconvert ! autovideosink
   ```
   Example:
   ```bash
   gst-launch-1.0 filesrc location=test_nv12.yuv ! videoparse width=1920 height=1080 framerate=30/1 format=nv12 ! videoconvert ! autovideosink
   ```

   For tiled formats like `nv12-64z32`, try:
   ```bash
   gst-launch-1.0 filesrc location=yuvframes_720p.yuv ! videoparse width=1280 height=720 framerate=30/1 format=nv12-64z32 ! autovideoconvert ! autovideosink
   ```
  [](https://ffmpeg.org/pipermail/ffmpeg-user/2016-April/031925.html)

### Troubleshooting Tips
- **Resolution and Frame Rate**: NV12 files lack metadata, so you must know the resolution and frame rate. Incorrect settings result in garbled output.
- **File Size Check**: For a single NV12 frame, the file size should be `width * height * 1.5` bytes (e.g., 1920x1080 = 3,110,400 bytes). This can help verify the resolution.
- **Tiled Formats**: If the NV12 file is in a tiled format (e.g., `nv12-64z32`), `ffplay` may not support it. Use GStreamer or convert it to standard NV12 with `ffmpeg`:
   ```bash
   ffmpeg -f rawvideo -pixel_format nv12-64z32 -s WIDTHxHEIGHT -i input.yuv -pixel_format nv12 output.yuv
   ```

### Recommendations
- **YUView** is recommended for its user-friendly interface and analytical tools, especially for static inspection.
- **ffplay** is great for quick playback of standard NV12 video files.
- **GStreamer** is best for tiled NV12 formats or when `ffplay` fails.
- If none work, convert to a standard format like MP4 for compatibility.

If you encounter issues or need help with specific parameters, provide details like the file’s resolution or error messages, and I can refine the solution!