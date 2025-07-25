/*
 * auth_utils.h - Authentication Utilities
 * Authentication and authorization utilities for Thingino Streamer
 * Provides authentication and authorization functions for Thingino Streamer
 * Supports Basic Authentication for HTTP, RTSP, and ONVIF
 * Also provides utility functions for encoding/decoding and header parsing
 * Copyright (C) 2025 Paul Philippov, Thingino Project
 */

#ifndef __AUTH_UTILS_H__
#define __AUTH_UTILS_H__

#include <netinet/in.h>
#include <stdbool.h>
#include <sys/socket.h>

/* Authentication result codes */
typedef enum {
    AUTH_RESULT_SUCCESS = 0,        /* Authentication successful or not required */
    AUTH_RESULT_REQUIRED,           /* Authentication required but not provided */
    AUTH_RESULT_INVALID,            /* Invalid credentials provided */
    AUTH_RESULT_ERROR               /* Authentication system error */
} auth_result_t;

/* Authentication configuration */
typedef struct {
    bool enabled;                   /* Enable authentication */
    bool localhost_bypass;          /* Allow localhost to bypass authentication */
    char username[64];              /* Username for authentication */
    char password[64];              /* Password for authentication */
} auth_config_t;

/* Client connection information */
typedef struct {
    int socket_fd;                  /* Client socket file descriptor */
    struct sockaddr_in addr;        /* Client address */
    bool is_localhost;              /* True if client is localhost */
    char ip_string[INET_ADDRSTRLEN]; /* Client IP as string */
} client_info_t;

/* Core authentication functions */

/**
 * Get client information from socket
 * @param socket_fd Client socket file descriptor
 * @param client_info Output client information structure
 * @return 0 on success, -1 on error
 */
int auth_get_client_info(int socket_fd, client_info_t* client_info);

/**
 * Check if client is localhost
 * @param addr Client address structure
 * @return true if localhost, false otherwise
 */
bool auth_is_localhost(const struct sockaddr_in* addr);

/**
 * Check if authentication is required for this client
 * @param config Authentication configuration
 * @param client_info Client information
 * @return true if authentication required, false if bypass allowed
 */
bool auth_is_required(const auth_config_t* config, const client_info_t* client_info);

/**
 * Validate Basic Authentication credentials
 * @param auth_header Authorization header value (e.g., "Basic dXNlcjpwYXNz")
 * @param config Authentication configuration
 * @return AUTH_RESULT_SUCCESS if valid, AUTH_RESULT_INVALID if invalid
 */
auth_result_t auth_validate_basic(const char* auth_header, const auth_config_t* config);

/**
 * Parse Basic Authentication header
 * @param auth_header Authorization header value
 * @param username Output buffer for username (min 64 bytes)
 * @param password Output buffer for password (min 64 bytes)
 * @return 0 on success, -1 on error
 */
int auth_parse_basic_header(const char* auth_header, char* username, char* password);

/**
 * Encode credentials for Basic Authentication
 * @param username Username
 * @param password Password
 * @param output Output buffer for base64 encoded credentials (min 128 bytes)
 * @return 0 on success, -1 on error
 */
int auth_encode_basic_credentials(const char* username, const char* password, char* output);

/**
 * Generate WWW-Authenticate header for Basic auth
 * @param realm Authentication realm
 * @param output Output buffer for header (min 256 bytes)
 * @return 0 on success, -1 on error
 */
int auth_generate_www_authenticate_header(const char* realm, char* output);

/* Protocol-specific authentication helpers */

/**
 * Check HTTP request authentication
 * @param request HTTP request string
 * @param config Authentication configuration
 * @param client_info Client information
 * @return Authentication result
 */
auth_result_t auth_check_http_request(const char* request, const auth_config_t* config,
                                     const client_info_t* client_info);

/**
 * Check RTSP request authentication
 * @param request RTSP request string
 * @param config Authentication configuration
 * @param client_info Client information
 * @return Authentication result
 */
auth_result_t auth_check_rtsp_request(const char* request, const auth_config_t* config,
                                     const client_info_t* client_info);

/**
 * Check ONVIF request authentication
 * @param request ONVIF/SOAP request string
 * @param config Authentication configuration
 * @param client_info Client information
 * @return Authentication result
 */
auth_result_t auth_check_onvif_request(const char* request, const auth_config_t* config,
                                      const client_info_t* client_info);

/* Utility functions */

/**
 * Base64 encode function
 * @param input Input data
 * @param input_len Input data length
 * @param output Output buffer (must be large enough)
 * @return Length of encoded data, -1 on error
 */
int auth_base64_encode(const unsigned char* input, int input_len, char* output);

/**
 * Base64 decode function
 * @param input Input base64 string
 * @param output Output buffer (must be large enough)
 * @return Length of decoded data, -1 on error
 */
int auth_base64_decode(const char* input, unsigned char* output);

#endif /* __AUTH_UTILS_H__ */
