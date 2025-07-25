RTSP (Real-Time Streaming Protocol)
===================================

RTSP operates with a control plane (for commands) and a data plane (for the actual video/audio). The control commands are exchanged over TCP, while the media data is typically sent over UDP.

### 1. Session Initiation & Setup

This phase is a back-and-forth negotiation to prepare for streaming.

* **`OPTIONS`**: The client first sends an `OPTIONS` request to the server to learn what RTSP methods (e.g., `DESCRIBE`, `PLAY`) are available. The server replies with a list of supported methods.

* **`DESCRIBE`**: The client sends a `DESCRIBE` request, asking for details about the media stream. The server responds with a **Session Description Protocol (SDP)** file. This text-based file contains crucial metadata like the video/audio codecs (e.g., H.264, AAC) and timing information.

* **`SETUP`**: The client reads the SDP file and sends a `SETUP` request for each media track it wants to receive (e.g., one for video, one for audio). In this request, the client tells the server which ports it will use to receive the **RTP** (media data) and **RTCP** (control protocol) packets.

* **`PLAY`**: Once the setup is complete, the client sends the `PLAY` command to tell the server to begin sending the media data.

### 2. Media Streaming

* **RTP (Real-time Transport Protocol) Packets**: The server streams the video and audio data to the client using the RTP ports agreed upon during the `SETUP` phase.

* **RTCP (RTP Control Protocol) Packets**: Simultaneously, the server and client exchange periodic RTCP packets. These packets are used to provide feedback on stream quality, packet loss, and to keep the session synchronized and alive.

### 3. Session Termination

* **`TEARDOWN`**: When the client is finished, it sends a `TEARDOWN` request. The server stops sending media and frees up the resources for that session.

### 4. Package order

The correct order of packets is critical for a video player to start decoding a stream. The general rule is that the decoder must receive the configuration parameters (SPS and PPS) before it receives any video data (IDR and subsequent frames).

In an RTSP session, the SPS (Sequence Parameter Set) and PPS (Picture Parameter Set) are sent right at the beginning of the video stream, followed immediately by an IDR (Instantaneous Decoder Refresh) frame.

#### Correct Order

1. **SPS NALU** (Network Abstraction Layer Unit)
2. **PPS NALU**
3. **IDR Frame Slices** (The first keyframe)
4. **P/B-Frames** (Subsequent video frames)

These three initial components (SPS, PPS, and IDR) are often sent together within the same timestamp in one or more RTP packets. This ensures that a client joining the stream has all the necessary information to start decoding immediately. The SPS/PPS only need to be sent again if the encoding parameters change, which is rare.

Failure to send the SPS/PPS before the IDR frame is the most common reason a video player will show a black screen or fail to start the stream.