/*
 * auth_utils.c - Authentication Utilities
 * Authentication and authorization utilities for Thingino Streamer
 * Provides authentication and authorization functions for Thingino Streamer
 * Supports Basic Authentication for HTTP, RTSP, and ONVIF
 * Also provides utility functions for encoding/decoding and header parsing
 * Copyright (C) 2025 Paul Philippov, Thingino Project
 */

#include "auth_utils.h"
#include "common.h"
#include <string.h>
#include <arpa/inet.h>
#include <ctype.h>

#define TAG "AUTH_UTILS"

/* Base64 encoding table */
static const char base64_chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/* Get client information from socket */
int auth_get_client_info(int socket_fd, client_info_t* client_info)
{
    // IMP_LOG_DBG(TAG, "Getting client info for socket %d", socket_fd);
    if (!client_info) {
        IMP_LOG_ERR(TAG, "Invalid client info pointer");
        return -1;
    }

    /* Initialize client info */
    // IMP_LOG_DBG(TAG, "Initializing client info");
    memset(client_info, 0, sizeof(client_info_t));
    client_info->socket_fd = socket_fd;

    /* Get client address */
    // IMP_LOG_DBG(TAG, "Getting client address");
    socklen_t addr_len = sizeof(client_info->addr);
    if (getpeername(socket_fd, (struct sockaddr*)&client_info->addr, &addr_len) < 0) {
        IMP_LOG_ERR(TAG, "Failed to get client address");
        return -1;
    }

    /* Convert IP to string */
    // IMP_LOG_DBG(TAG, "Converting IP to string");
    if (!inet_ntop(AF_INET, &client_info->addr.sin_addr,
                   client_info->ip_string, sizeof(client_info->ip_string))) {
        IMP_LOG_ERR(TAG, "Failed to convert IP to string");
        return -1;
    }

    /* Check if localhost */
    // IMP_LOG_DBG(TAG, "Checking if client is localhost: %s", client_info->ip_string);
    client_info->is_localhost = auth_is_localhost(&client_info->addr);

    // IMP_LOG_DBG(TAG, "Client info: IP=%s, localhost=%s, raw_ip=0x%08x",
    //            client_info->ip_string, client_info->is_localhost ? "yes" : "no",
    //            ntohl(client_info->addr.sin_addr.s_addr));
    return 0;
}

/* Check if client is localhost */
bool auth_is_localhost(const struct sockaddr_in* addr)
{
    if (!addr) {
        IMP_LOG_ERR(TAG, "Invalid address pointer");
        return false;
    }

    uint32_t ip = ntohl(addr->sin_addr.s_addr);

    /* Check for localhost addresses:
     * 127.0.0.0/8 (127.0.0.1 - 127.255.255.255)
     * Also check for 0.0.0.0 (sometimes used for local connections)
     */
    bool is_localhost = (ip >> 24) == 127 || ip == 0;

    // IMP_LOG_DBG(TAG, "Localhost check: IP=0x%08x, first_octet=%d, is_localhost=%s",
    //            ip, (ip >> 24), is_localhost ? "yes" : "no");
    return is_localhost;
}

/* Check if authentication is required for this client */
bool auth_is_required(const auth_config_t* config, const client_info_t* client_info)
{
    if (!config || !client_info) {
        IMP_LOG_ERR(TAG, "Invalid config or client info pointer");
        return true; /* Fail secure */
    }

    /* If authentication is disabled, never require it */
    if (!config->enabled) {
        IMP_LOG_DBG(TAG, "Authentication disabled in configuration");
        return false;
    }

    /* If localhost bypass is enabled and client is localhost, skip auth */
    if (config->localhost_bypass && client_info->is_localhost) {
        IMP_LOG_DBG(TAG, "Localhost bypass enabled for %s", client_info->ip_string);
        return false;
    }

    /* Authentication required for non-localhost or when bypass disabled */
    // IMP_LOG_DBG(TAG, "Authentication required for %s", client_info->ip_string);
    return true;
}

/* Base64 encode function */
int auth_base64_encode(const unsigned char* input, int input_len, char* output)
{
    if (!input || !output || input_len < 0) {
        IMP_LOG_ERR(TAG, "Invalid input or output pointer");
        return -1;
    }

    int output_len = 0;
    int i;

    // IMP_LOG_DBG(TAG, "Base64 encoding input of length %d", input_len);
    for (i = 0; i < input_len; i += 3) {
        uint32_t octet_a = i < input_len ? input[i] : 0;
        uint32_t octet_b = i + 1 < input_len ? input[i + 1] : 0;
        uint32_t octet_c = i + 2 < input_len ? input[i + 2] : 0;

        uint32_t triple = (octet_a << 16) + (octet_b << 8) + octet_c;

        output[output_len++] = base64_chars[(triple >> 18) & 63];
        output[output_len++] = base64_chars[(triple >> 12) & 63];
        output[output_len++] = i + 1 < input_len ? base64_chars[(triple >> 6) & 63] : '=';
        output[output_len++] = i + 2 < input_len ? base64_chars[triple & 63] : '=';
    }

    output[output_len] = '\0';
    // IMP_LOG_DBG(TAG, "Base64 encoded output: %s", output);
    return output_len;
}

/* Base64 decode function */
int auth_base64_decode(const char* input, unsigned char* output)
{
    if (!input || !output) {
        IMP_LOG_ERR(TAG, "Invalid input or output pointer");
        return -1;
    }

    int input_len = strlen(input);
    if (input_len % 4 != 0) {
        IMP_LOG_ERR(TAG, "Invalid input length");
        return -1;
    }

    int output_len = 0;
    int i;

    // IMP_LOG_DBG(TAG, "Base64 decoding input: %s", input);
    for (i = 0; i < input_len; i += 4) {
        uint32_t sextet_a = input[i] == '=' ? 0 : strchr(base64_chars, input[i]) - base64_chars;
        uint32_t sextet_b = input[i + 1] == '=' ? 0 : strchr(base64_chars, input[i + 1]) - base64_chars;
        uint32_t sextet_c = input[i + 2] == '=' ? 0 : strchr(base64_chars, input[i + 2]) - base64_chars;
        uint32_t sextet_d = input[i + 3] == '=' ? 0 : strchr(base64_chars, input[i + 3]) - base64_chars;

        uint32_t triple = (sextet_a << 18) + (sextet_b << 12) + (sextet_c << 6) + sextet_d;

        output[output_len++] = (triple >> 16) & 255;
        if (input[i + 2] != '=') output[output_len++] = (triple >> 8) & 255;
        if (input[i + 3] != '=') output[output_len++] = triple & 255;
    }

    // IMP_LOG_DBG(TAG, "Base64 decoded output of length %d", output_len);
    return output_len;
}

/* Parse Basic Authentication header */
int auth_parse_basic_header(const char* auth_header, char* username, char* password)
{
    if (!auth_header || !username || !password) {
        IMP_LOG_ERR(TAG, "Invalid input or output pointer");
        return -1;
    }

    /* Check for "Basic " prefix */
    if (strncmp(auth_header, "Basic ", 6) != 0) {
        IMP_LOG_ERR(TAG, "Invalid Basic Authentication header format");
        return -1;
    }

    const char* encoded = auth_header + 6;
    unsigned char decoded[128];
    int decoded_len = auth_base64_decode(encoded, decoded);

    if (decoded_len < 0) {
        IMP_LOG_ERR(TAG, "Failed to decode Base64 in Basic Authentication header");
        return -1;
    }

    decoded[decoded_len] = '\0';
    IMP_LOG_DBG(TAG, "Decoded Basic Authentication credentials: '%s' (length: %d)", decoded, decoded_len);

    /* Find the colon separator */
    char* colon = strchr((char*)decoded, ':');
    if (!colon) {
        IMP_LOG_ERR(TAG, "No colon separator found in Basic Authentication credentials");
        return -1;
    }

    *colon = '\0';
    strncpy(username, (char*)decoded, 63);
    username[63] = '\0';
    strncpy(password, colon + 1, 63);
    password[63] = '\0';
    IMP_LOG_DBG(TAG, "Parsed username: '%s', password: '%s'", username, password);

    return 0;
}

/* Validate Basic Authentication credentials */
auth_result_t auth_validate_basic(const char* auth_header, const auth_config_t* config)
{
    if (!config) {
        IMP_LOG_ERR(TAG, "Invalid config pointer");
        return AUTH_RESULT_ERROR;
    }

    if (!auth_header) {
        IMP_LOG_WARN(TAG, "No Authorization header provided");
        return AUTH_RESULT_REQUIRED;
    }

    char username[64], password[64];
    if (auth_parse_basic_header(auth_header, username, password) < 0) {
        IMP_LOG_WARN(TAG, "Failed to parse Basic Authentication header");
        return AUTH_RESULT_INVALID;
    }

    // IMP_LOG_DBG(TAG, "Validating credentials: provided='%s:%s', expected='%s:%s'",
    //            username, password, config->username, config->password);

    if (strcmp(username, config->username) == 0 && strcmp(password, config->password) == 0) {
        // IMP_LOG_DBG(TAG, "Credentials match - authentication successful");
        return AUTH_RESULT_SUCCESS;
    }

    // IMP_LOG_WARN(TAG, "Credentials do not match - authentication failed");
    return AUTH_RESULT_INVALID;
}

/* Encode credentials for Basic Authentication */
int auth_encode_basic_credentials(const char* username, const char* password, char* output)
{
    if (!username || !password || !output) {
        IMP_LOG_ERR(TAG, "Invalid input or output pointer");
        return -1;
    }

    char credentials[128];
    snprintf(credentials, sizeof(credentials), "%s:%s", username, password);
    // IMP_LOG_DBG(TAG, "Encoding credentials: %s", credentials);

    return auth_base64_encode((unsigned char*)credentials, strlen(credentials), output);
}

/* Generate WWW-Authenticate header for Basic auth */
int auth_generate_www_authenticate_header(const char* realm, char* output)
{
    if (!realm || !output) {
        IMP_LOG_ERR(TAG, "Invalid input or output pointer");
        return -1;
    }

    snprintf(output, 256, "WWW-Authenticate: Basic realm=\"%s\"", realm);
    // IMP_LOG_DBG(TAG, "Generated WWW-Authenticate header: %s", output);
    return 0;
}

/* Extract Authorization header from HTTP/RTSP request */
static const char* extract_auth_header(const char* request)
{
    if (!request) {
        IMP_LOG_ERR(TAG, "Invalid request pointer");
        return NULL;
    }

    // dump full request to debug
    IMP_LOG_DBG(TAG, "Request: %s", request);

    IMP_LOG_DBG(TAG, "Extracting Authorization header from request");
    const char* auth_line = strstr(request, "Authorization:");
    if (!auth_line) {
        /* Try lowercase */
        auth_line = strstr(request, "authorization:");
    }

    if (!auth_line) {
        IMP_LOG_DBG(TAG, "No Authorization header found in request");
        return NULL;
    }

    /* Skip "Authorization: " */
    auth_line += 14;
    while (*auth_line == ' ') {
        auth_line++;
    }

    /* Find end of line */
    const char* end = strstr(auth_line, "\r\n");
    if (!end) {
        end = strstr(auth_line, "\n");
    }

    if (!end) {
        IMP_LOG_DBG(TAG, "No end of line found in Authorization header");
        return auth_line; /* Single line */
    }

    /* Create a copy of the header value */
    static char auth_header[256];
    int len = end - auth_line;
    if (len >= sizeof(auth_header)) {
        len = sizeof(auth_header) - 1;
    }

    strncpy(auth_header, auth_line, len);
    auth_header[len] = '\0';
    IMP_LOG_DBG(TAG, "Extracted Authorization header: '%s'", auth_header);

    return auth_header;
}

/* Check HTTP request authentication */
auth_result_t auth_check_http_request(const char* request, const auth_config_t* config,
                                     const client_info_t* client_info)
{
    if (!config || !client_info) {
        IMP_LOG_ERR(TAG, "Invalid config or client info pointer");
        return AUTH_RESULT_ERROR;
    }

    /* Check if authentication is required */
    if (!auth_is_required(config, client_info)) {
        IMP_LOG_DBG(TAG, "Authentication not required for HTTP request from %s", client_info->ip_string);
        return AUTH_RESULT_SUCCESS;
    }

    /* Extract and validate authorization header */
    // IMP_LOG_DBG(TAG, "Checking HTTP request authentication from %s", client_info->ip_string);
    const char* auth_header = extract_auth_header(request);
    return auth_validate_basic(auth_header, config);
}

/* Check RTSP request authentication */
auth_result_t auth_check_rtsp_request(const char* request, const auth_config_t* config,
                                     const client_info_t* client_info)
{
    /* RTSP uses the same Basic Authentication as HTTP */
    // IMP_LOG_DBG(TAG, "Checking RTSP request authentication from %s", client_info->ip_string);
    return auth_check_http_request(request, config, client_info);
}

/* Parse WS-Security UsernameToken from SOAP request */
static int parse_ws_security_token(const char* request, char* username, char* password)
{
    if (!request || !username || !password) {
        return -1;
    }

    /* Look for Username element */
    const char* username_start = strstr(request, "<Username>");
    if (!username_start) {
        return -1;
    }
    username_start += 10; /* Skip "<Username>" */

    const char* username_end = strstr(username_start, "</Username>");
    if (!username_end) {
        return -1;
    }

    int username_len = username_end - username_start;
    if (username_len >= 64) {
        username_len = 63;
    }
    strncpy(username, username_start, username_len);
    username[username_len] = '\0';

    /* For WS-Security with digest, we can't extract the plain password
     * We'll just mark that WS-Security was found and validate the username */
    strcpy(password, ""); /* Empty password for WS-Security */

    IMP_LOG_DBG(TAG, "Found WS-Security UsernameToken: username='%s'", username);
    return 0;
}

/* Check ONVIF request authentication */
auth_result_t auth_check_onvif_request(const char* request, const auth_config_t* config,
                                      const client_info_t* client_info)
{
    if (!config || !client_info) {
        IMP_LOG_ERR(TAG, "Invalid config or client info pointer");
        return AUTH_RESULT_ERROR;
    }

    /* Check if authentication is required */
    if (!auth_is_required(config, client_info)) {
        IMP_LOG_DBG(TAG, "Authentication not required for ONVIF request from %s", client_info->ip_string);
        return AUTH_RESULT_SUCCESS;
    }

    /* First try WS-Security authentication */
    if (strstr(request, "<Security") && strstr(request, "<UsernameToken")) {
        char ws_username[64], ws_password[64];
        if (parse_ws_security_token(request, ws_username, ws_password) == 0) {
            /* For WS-Security, we only validate the username for now
             * Full digest validation would require implementing the WS-Security spec */
            if (strcmp(ws_username, config->username) == 0) {
                IMP_LOG_DBG(TAG, "WS-Security authentication successful for user: %s", ws_username);
                return AUTH_RESULT_SUCCESS;
            } else {
                IMP_LOG_WARN(TAG, "WS-Security authentication failed: username mismatch");
                return AUTH_RESULT_INVALID;
            }
        }
    }

    /* Fall back to HTTP Basic Authentication */
    IMP_LOG_DBG(TAG, "Checking ONVIF Basic Authentication from %s", client_info->ip_string);
    return auth_check_http_request(request, config, client_info);
}
