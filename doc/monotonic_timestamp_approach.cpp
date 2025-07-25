/*
 * MONOTONIC TIMESTAMP APPROACH - PRESERVED FOR FUTURE USE
 * 
 * This was an attempt to fix RTSP streaming delays by using monotonic time
 * for consistent RTP timestamps. While technically correct, it didn't match
 * the official Ingenic sample pattern and caused compatibility issues.
 * 
 * The official sample uses gettimeofday() for presentation timestamps.
 */

// MONOTONIC TIME CALCULATION (preserved for reference)
void calculateMonotonicTimestamp(struct timeval* timestamp, int encChn) {
    // CRITICAL FIX: Use proper RTP timestamp calculation for Live555 (90kHz clock)
    // Declare all variables at top to avoid goto scope issues
    static bool first_frame = true;
    static struct timespec start_time;
    struct timespec current_time;
    long elapsed_sec, elapsed_nsec, elapsed_usec;
    uint64_t rtp_timestamp;
    
    if (first_frame) {
        clock_gettime(CLOCK_MONOTONIC, &start_time);
        first_frame = false;
    }
    
    clock_gettime(CLOCK_MONOTONIC, &current_time);
    
    // Calculate elapsed time in microseconds
    elapsed_sec = current_time.tv_sec - start_time.tv_sec;
    elapsed_nsec = current_time.tv_nsec - start_time.tv_nsec;
    elapsed_usec = elapsed_sec * 1000000 + elapsed_nsec / 1000;
    
    // Convert to 90kHz RTP timestamp (Live555 standard for H.264)
    rtp_timestamp = (elapsed_usec * 90) / 1000;
    
    // Use elapsed time for presentation timestamp
    timestamp->tv_sec = elapsed_usec / 1000000;
    timestamp->tv_usec = elapsed_usec % 1000000;
    
    LOG_DEBUG("SimpleVideoSource: Using monotonic timestamp %ld.%06ld (elapsed=%ld us, rtp=%llu) for channel %d",
              (long)timestamp->tv_sec, (long)timestamp->tv_usec, elapsed_usec, rtp_timestamp, encChn);
}

/*
 * NOTES ON MONOTONIC APPROACH:
 * 
 * Advantages:
 * - Consistent timing regardless of system clock changes
 * - Proper RTP timestamp calculation (90kHz)
 * - No timestamp jumps from NTP sync or manual time changes
 * 
 * Issues encountered:
 * - Live555 expects system time format for presentation timestamps
 * - Caused packet loss and timing issues with RTSP clients
 * - Didn't match official Ingenic sample behavior
 * 
 * Future considerations:
 * - Could be useful for systems with unreliable system clocks
 * - Might need Live555 modifications to work properly
 * - Consider for embedded systems without RTC
 */
