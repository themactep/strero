RTMP (Real-Time Messaging Protocol)
===================================

RTMP multiplexes all control commands and media data over a single, persistent TCP connection. It is most commonly used for pushing streams to a media server (publishing).

### 1. Connection Initiation (handshake)

This phase establishes a reliable connection between the client and server.

* **Handshake**: A three-way packet exchange (C0/C1 -> S0/S1/S2 -> C2) occurs to establish a connection and agree on the protocol version.

* **`connect`**: The client sends a `connect` command message to the server, requesting a connection to a specific application on the server.

* **`createStream`**: The client sends a `createStream` command, and the server responds by creating a logical channel for the media to flow through, providing a unique **Stream ID**.

### 2. Media Streaming

* **`publish`**: The client sends a `publish` command, along with a stream name or key, signaling its intent to start sending video. The server acknowledges that the stream is live and ready to receive data.

* **Audio/Video Messages**: The client sends a continuous flow of timestamped audio and video data messages. These are chunked and sent over the single TCP connection established earlier.

### 3. Session Termination

* To end the stream, the client typically sends an `FCUnpublish` and/or `deleteStream` command. The TCP connection is then closed, and the server stops accepting data for that stream.

### 4. Package order

The correct order of packets is critical for a video player to start decoding a stream. The general rule is that the decoder must receive the configuration parameters (SPS and PPS) before it receives any video data (IDR and subsequent frames).

RTMP handles this by sending a single configuration packet at the very beginning of the stream, before any video frames. This packet contains the SPS and PPS information combined, and is often called the AVCDecoderConfigurationRecord.

#### Correct Order

1. **Video Sequence Header**: This is a special RTMP video message that contains the SPS and PPS information combined. It is often called the AVCDecoderConfigurationRecord. This packet is sent only once after the publish command is successful.
2. **IDR Frame**: The first video data packet sent after the sequence header must be a keyframe (IDR).
3. **P/B-Frames**: All subsequent video frames follow.

Failure to send the SPS/PPS before the IDR frame is the most common reason a video player will show a black screen or fail to start the stream.
