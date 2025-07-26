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
    if (!client_info) {
        return -1;
    }

    memset(client_info, 0, sizeof(client_info_t));
    client_info->socket_fd = socket_fd;

    socklen_t addr_len = sizeof(client_info->addr);
    if (getpeername(socket_fd, (struct sockaddr*)&client_info->addr, &addr_len) < 0) {
        IMP_LOG_ERR(TAG, "Failed to get client address");
        return -1;
    }

    /* Convert IP to string */
    if (!inet_ntop(AF_INET, &client_info->addr.sin_addr,
                   client_info->ip_string, sizeof(client_info->ip_string))) {
        IMP_LOG_ERR(TAG, "Failed to convert IP to string");
        return -1;
    }

    /* Check if localhost */
    client_info->is_localhost = auth_is_localhost(&client_info->addr);

    IMP_LOG_DBG(TAG, "Client info: IP=%s, localhost=%s",
               client_info->ip_string, client_info->is_localhost ? "yes" : "no");

    return 0;
}

/* Check if client is localhost */
bool auth_is_localhost(const struct sockaddr_in* addr)
{
    if (!addr) {
        return false;
    }

    uint32_t ip = ntohl(addr->sin_addr.s_addr);

    /* Check for localhost addresses:
     * 127.0.0.0/8 (127.0.0.1 - 127.255.255.255)
     * Also check for 0.0.0.0 (sometimes used for local connections)
     */
    bool is_localhost = (ip >> 24) == 127 || ip == 0;
    return is_localhost;
}

/* Check if authentication is required for this client */
bool auth_is_required(const auth_config_t* config, const client_info_t* client_info)
{
    if (!config || !client_info) {
        return true; /* Fail secure */
    }

    /* If authentication is disabled, never require it */
    if (!config->enabled) {
        return false;
    }

    /* If localhost bypass is enabled and client is localhost, skip auth */
    if (config->localhost_bypass && client_info->is_localhost) {
        IMP_LOG_DBG(TAG, "Localhost bypass enabled for %s", client_info->ip_string);
        return false;
    }

    /* Authentication required for non-localhost or when bypass disabled */
    IMP_LOG_DBG(TAG, "Authentication required for %s", client_info->ip_string);
    return true;
}

/* Base64 encode function */
int auth_base64_encode(const unsigned char* input, int input_len, char* output)
{
    if (!input || !output || input_len < 0) {
        return -1;
    }

    int output_len = 0;
    int i;

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
    return output_len;
}

/* Base64 decode function */
int auth_base64_decode(const char* input, unsigned char* output)
{
    if (!input || !output) {
        return -1;
    }

    int input_len = strlen(input);
    if (input_len % 4 != 0) {
        return -1;
    }

    int output_len = 0;
    int i;

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

    return output_len;
}

/* Parse Basic Authentication header */
int auth_parse_basic_header(const char* auth_header, char* username, char* password)
{
    if (!auth_header || !username || !password) {
        return -1;
    }

    /* Check for "Basic " prefix */
    if (strncmp(auth_header, "Basic ", 6) != 0) {
        return -1;
    }

    const char* encoded = auth_header + 6;
    unsigned char decoded[128];
    int decoded_len = auth_base64_decode(encoded, decoded);

    if (decoded_len < 0) {
        return -1;
    }

    decoded[decoded_len] = '\0';

    /* Find the colon separator */
    char* colon = strchr((char*)decoded, ':');
    if (!colon) {
        return -1;
    }

    *colon = '\0';
    strncpy(username, (char*)decoded, 63);
    username[63] = '\0';
    strncpy(password, colon + 1, 63);
    password[63] = '\0';

    return 0;
}

/* Validate Basic Authentication credentials */
auth_result_t auth_validate_basic(const char* auth_header, const auth_config_t* config)
{
    if (!config) {
        return AUTH_RESULT_ERROR;
    }

    if (!auth_header) {
        return AUTH_RESULT_REQUIRED;
    }

    char username[64], password[64];
    if (auth_parse_basic_header(auth_header, username, password) < 0) {
        return AUTH_RESULT_INVALID;
    }

    if (strcmp(username, config->username) == 0 && strcmp(password, config->password) == 0) {
        return AUTH_RESULT_SUCCESS;
    }

    return AUTH_RESULT_INVALID;
}

/* Encode credentials for Basic Authentication */
int auth_encode_basic_credentials(const char* username, const char* password, char* output)
{
    if (!username || !password || !output) {
        return -1;
    }

    char credentials[128];
    snprintf(credentials, sizeof(credentials), "%s:%s", username, password);

    return auth_base64_encode((unsigned char*)credentials, strlen(credentials), output);
}

/* Generate WWW-Authenticate header for Basic auth */
int auth_generate_www_authenticate_header(const char* realm, char* output)
{
    if (!realm || !output) {
        return -1;
    }

    snprintf(output, 256, "WWW-Authenticate: Basic realm=\"%s\"", realm);
    return 0;
}

/* Extract Authorization header from HTTP/RTSP request */
static const char* extract_auth_header(const char* request)
{
    if (!request) {
        return NULL;
    }

    const char* auth_line = strstr(request, "Authorization:");
    if (!auth_line) {
        auth_line = strstr(request, "authorization:");
    }

    if (!auth_line) {
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

    return auth_header;
}

/* Check HTTP request authentication */
auth_result_t auth_check_http_request(const char* request, const auth_config_t* config,
                                     const client_info_t* client_info)
{
    if (!config || !client_info) {
        return AUTH_RESULT_ERROR;
    }

    /* Check if authentication is required */
    if (!auth_is_required(config, client_info)) {
        return AUTH_RESULT_SUCCESS;
    }

    /* Extract and validate authorization header */
    const char* auth_header = extract_auth_header(request);
    return auth_validate_basic(auth_header, config);
}

/* Check RTSP request authentication */
auth_result_t auth_check_rtsp_request(const char* request, const auth_config_t* config,
                                     const client_info_t* client_info)
{
    /* RTSP uses the same Basic Authentication as HTTP */
    return auth_check_http_request(request, config, client_info);
}

/* Check ONVIF request authentication */
auth_result_t auth_check_onvif_request(const char* request, const auth_config_t* config,
                                      const client_info_t* client_info)
{
    /* ONVIF can use HTTP Basic Authentication or WS-Security
     * For now, implement Basic Authentication like HTTP */
    return auth_check_http_request(request, config, client_info);
}
