RAW IMAGES
==========

YUYV422 Format
--------------

To view a raw YUYV422 (also known as YUY2) file on Linux, you need a tool that supports raw YUV formats, specifically the YUYV422 pixel format, and you must know the file’s resolution (width and height). YUYV422 is a packed YUV 4:2:2 format where each pixel pair is stored as Y0, U, Y1, V (4 bytes per 2 pixels). The process is similar to viewing an NV12 file, but you’ll specify `yuyv422` as the pixel format instead. Below are the steps to view a YUYV422 raw file using various tools, including `ffplay`, YUView, GStreamer, and conversion to a viewable format.

### Prerequisites
- **Know the Resolution**: You must know the width and height of the YUYV422 file (e.g., 1920x1080). Unlike formatted video files, raw YUV files lack metadata, so you need to provide this information.
- **File Size Check**: For a single YUYV422 frame, the file size should be `width * height * 2` bytes (since each pixel uses 2 bytes in YUYV422). For example, a 1920x1080 frame should be `1920 * 1080 * 2 = 4,147,200` bytes. For a video, multiply by the number of frames.
- **Tools**: Ensure you have one of the following tools installed: FFmpeg (`ffplay`), YUView, or GStreamer.

### Method 1: Using `ffplay` (FFmpeg)
`ffplay`, part of FFmpeg suite, supports YUYV422 raw files.

1. **Install FFmpeg**:
   Ensure FFmpeg is installed on your Linux system:
   ```bash
   sudo apt install ffmpeg  # For Ubuntu/Debian
   sudo dnf install ffmpeg  # For Fedora
   sudo pacman -S ffmpeg    # For Arch Linux
   ```

2. **Play the YUYV422 File**:
   Use the following command, replacing `WIDTHxHEIGHT` with the file’s resolution (e.g., `1920x1080`) and `input.yuyv` with your file’s name:
   ```bash
   ffplay -f rawvideo -pixel_format yuyv422 -video_size WIDTHxHEIGHT -i input.yuyv
   ```
   Example for a 1920x1080 file:
   ```bash
   ffplay -f rawvideo -pixel_format yuyv422 -video_size 1920x1080 -i image0.yuyv
   ```
   - `-f rawvideo`: Specifies the input is a raw video file.
   - `-pixel_format yuyv422`: Sets the pixel format to YUYV422.
   - `-video_size WIDTHxHEIGHT`: Defines the resolution (e.g., `1920x1080`).
   - `-i input.yuyv`: Specifies the input file.

   If the file is a video (multiple frames), you may need to specify the frame rate with `-framerate` (e.g., `-framerate 30` for 30 fps):
   ```bash
   ffplay -f rawvideo -pixel_format yuyv422 -video_size 1920x1080 -framerate 30 -i image0.yuyv
   ```

3. **Troubleshooting**:
   - If you get an error like “Option not found” for `-video_size`, try an alternative syntax:
     ```bash
     ffplay -f rawvideo -pixel_format yuyv422 -i image0.yuyv -vf "setdar=1920/1080"
     ```
   - Verify `yuyv422` support:
     ```bash
     ffmpeg -pix_fmts | grep yuyv422
     ```
     You should see:
     ```
     IO.... yuyv422              YUV 4:2:2, 16 bpp, 1 plane, packed
     ```
   - If the output looks garbled, double-check the resolution or frame rate. For a single frame, omit `-framerate`.

### Method 2: Using YUView
YUView is an open-source YUV viewer that supports YUYV422 and is great for analyzing raw video files.

1. **Install YUView**:
   ```bash
   sudo apt install yuview  # For Ubuntu 22.04 or newer
   ```
   Alternatively, use Flatpak:
   ```bash
   flatpak install flathub org.ient.YUView
   ```
   Or download binaries from https://github.com/IENT/YUView.

2. **Open the YUYV422 File**:
   - Launch YUView.
   - Go to `File > Open` and select your YUYV422 file (e.g., `image0.yuyv`).
   - In the dialog, set:
     - **Format**: `YUV`
     - **Pixel Format**: `YUYV422`
     - **Resolution**: Enter the width and height (e.g., `1920x1080`).
     - **Frame Rate** (optional): Set if it’s a video (e.g., `30` for 30 fps).
   - Click `OK` to view the file.

   YUView allows you to navigate frames, zoom, and analyze pixel values, making it ideal for detailed inspection.

### Method 3: Using GStreamer
GStreamer is another option for viewing YUYV422 files, especially if `ffplay` fails or you need support for specific formats.

1. **Install GStreamer**:
   ```bash
   sudo apt install gstreamer1.0-tools gstreamer1.0-plugins-base gstreamer1.0-plugins-good
   ```

2. **Play the YUYV422 File**:
   Use the following pipeline, replacing `WIDTH`, `HEIGHT`, and `input.yuyv`:
   ```bash
   gst-launch-1.0 filesrc location=input.yuyv ! videoparse width=WIDTH height=HEIGHT format=yuy2 ! videoconvert ! autovideosink
   ```
   Example for 1920x1080:
   ```bash
   gst-launch-1.0 filesrc location=image0.yuyv ! videoparse width=1920 height=1080 format=yuy2 ! videoconvert ! autovideosink
   ```
   - `videoparse format=yuy2`: Specifies the YUYV422 format (GStreamer uses `yuy2` for YUYV422).
   - For a video, add the frame rate:
     ```bash
     gst-launch-1.0 filesrc location=image0.yuyv ! videoparse width=1920 height=1080 framerate=30/1 format=yuy2 ! videoconvert ! autovideosink
     ```

### Method 4: Convert YUYV422 to a Viewable Format
If you can’t view the file directly, convert it to a standard format like PNG (for a single frame) or MP4 (for a video) using `ffmpeg`.

1. **Convert to PNG** (for a single frame):
   ```bash
   ffmpeg -f rawvideo -pixel_format yuyv422 -video_size WIDTHxHEIGHT -i input.yuyv -pix_fmt rgb24 -frames:v 1 output.png
   ```
   Example:
   ```bash
   ffmpeg -f rawvideo -pixel_format yuyv422 -video_size 1920x1080 -i image0.yuyv -pix_fmt rgb24 -frames:v 1 output.png
   ```
   View the PNG with an image viewer:
   ```bash
   eog output.png  # Or use gimp, feh, etc.
   ```

2. **Convert to MP4** (for a video):
   ```bash
   ffmpeg -f rawvideo -pixel_format yuyv422 -video_size WIDTHxHEIGHT -framerate FRAMERATE -i input.yuyv -c:v libx264 -pix_fmt yuv420p output.mp4
   ```
   Example:
   ```bash
   ffmpeg -f rawvideo -pixel_format yuyv422 -video_size 1920x1080 -framerate 30 -i image0.yuyv -c:v libx264 -pix_fmt yuv420p output.mp4
   ```
   Play the MP4 with a video player:
   ```bash
   vlc output.mp4  # Or use mpv, totem, etc.
   ```

### Troubleshooting Tips
- **Resolution**: Ensure the width and height are correct. For a single frame, the file size should be `width * height * 2` bytes. For example:
  ```bash
  ls -l image0.yuyv
  ```
  A 1920x1080 YUYV422 frame should be 4,147,200 bytes.
- **Pixel Format**: Confirm the file is in YUYV422 (not another YUV format like YUV422P or NV12). If the output looks garbled, try other YUV 4:2:2 formats like `uyvy422`:
  ```bash
  ffplay -f rawvideo -pixel_format uyvy422 -video_size 1920x1080 -i image0.yuyv
  ```
- **Frame Rate**: For videos, an incorrect frame rate can cause playback issues. If unknown, test common values (e.g., 24, 25, 30 fps).
- **FFmpeg Build**: Your FFmpeg 7.1.1-1+b1 supports `yuyv422` (as `nv12` worked and YUYV422 is standard). If `ffplay` fails with “Option not found” for `-video_size`, try reinstalling FFmpeg as described previously:
  ```bash
  sudo apt remove ffmpeg
  ```
  Then download a static build from https://ffmpeg.org/download.html.

### Recommendations
- **ffplay**: Fastest for quick viewing, using `-pixel_format yuyv422 -video_size WIDTHxHEIGHT`.
- **YUView**: Best for detailed analysis of single frames or videos, with a user-friendly interface.
- **GStreamer**: Useful if `ffplay` fails or for specific pipeline needs.
- **Conversion**: Convert to PNG for single frames or MP4 for videos if direct viewing fails.

If you have a specific YUYV422 file (e.g., `image0.yuyv`) and know its resolution, try the `ffplay` command first:
```bash
ffplay -f rawvideo -pixel_format yuyv422 -video_size 1920x1080 -i image0.yuyv
```
If you encounter issues or need help with a specific file (e.g., confirming resolution or handling errors), provide details like the file size or error messages, and I can refine the solution!