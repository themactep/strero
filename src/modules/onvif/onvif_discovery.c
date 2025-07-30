/*
 * onvif_discovery.c - ONVIF WS-Discovery Service
 * Implements WS-Discovery for ONVIF device discovery
 * Copyright (C) 2025 Paul Philippov, Thingino Project
 */

#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include "../../config.h"
#include "../../common.h"
#include "onvif_module.h"

#define TAG "ONVIF_DISCOVERY"
#define WS_DISCOVERY_PORT 3702
#define WS_DISCOVERY_ADDR "239.255.255.250"

static pthread_t discovery_thread;
static volatile int discovery_running = 0;

/*
 * Deduplication cache based on MessageID.
 * This is more reliable than IP-based checks, as a single probe packet
 * can be received multiple times on hosts with multiple network interfaces.
 */
#define MAX_RECENT_MESSAGES 20
#define MAX_MESSAGE_ID_LEN 128 // A generous length for UUIDs in format "urn:uuid:..."
static struct {
    char message_ids[MAX_RECENT_MESSAGES][MAX_MESSAGE_ID_LEN];
    time_t timestamps[MAX_RECENT_MESSAGES];
    int next_index;
} recent_messages = { .next_index = 0 };

/*
 * IP-based response cool-down cache.
 * This prevents sending multiple responses to the same client IP
 * in a very short time frame, which can happen if the client
 * sends multiple different probe messages in quick succession.
 */
#define MAX_RECENT_CLIENTS 20
#define RESPONSE_COOLDOWN_SECONDS 1 // Cooldown period in seconds
static struct {
    uint32_t client_ips[MAX_RECENT_CLIENTS];
    time_t last_response_times[MAX_RECENT_CLIENTS];
    int next_index;
} client_cooldown_cache = { .next_index = 0 };

/* WS-Discovery response template */
static const char* discovery_response_template =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
    "<SOAP-ENV:Envelope xmlns:SOAP-ENV=\"http://www.w3.org/2003/05/soap-envelope\""
    " xmlns:wsa=\"http://www.w3.org/2005/08/addressing\""
    " xmlns:d=\"http://schemas.xmlsoap.org/ws/2005/04/discovery\""
    " xmlns:dn=\"http://www.onvif.org/ver10/network/wsdl\">"
     "<SOAP-ENV:Header>"
      "<wsa:MessageID>uuid:%s</wsa:MessageID>"
      "<wsa:RelatesTo>%s</wsa:RelatesTo>"
      "<wsa:To>http://schemas.xmlsoap.org/ws/2004/08/addressing/role/anonymous</wsa:To>"
      "<wsa:Action>http://schemas.xmlsoap.org/ws/2005/04/discovery/ProbeMatches</wsa:Action>"
     "</SOAP-ENV:Header>"
     "<SOAP-ENV:Body>"
      "<d:ProbeMatches>"
       "<d:ProbeMatch>"
        "<wsa:EndpointReference>"
         "<wsa:Address>urn:uuid:%s</wsa:Address>"
        "</wsa:EndpointReference>"
        "<d:Types>dn:NetworkVideoTransmitter</d:Types>"
        "<d:Scopes>onvif://www.onvif.org/type/video_encoder onvif://www.onvif.org/hardware/%s onvif://www.onvif.org/location/country/unknown</d:Scopes>"
        "<d:XAddrs>http://%s:%d/onvif/device_service</d:XAddrs>"
        "<d:MetadataVersion>1</d:MetadataVersion>"
       "</d:ProbeMatch>"
      "</d:ProbeMatches>"
     "</SOAP-ENV:Body>"
    "</SOAP-ENV:Envelope>";

/* Extract message ID from WS-Discovery probe */
static char* extract_message_id(const char* request)
{
    // Look for the standard MessageID tag
    const char* start = strstr(request, "<wsa:MessageID>");
    if (!start) {
        // Fallback for older or different namespace prefixes
        start = strstr(request, "<a:MessageID>");
    }
    if (!start) {
        // Final fallback for discovery namespace
        start = strstr(request, "<d:MessageID>");
    }

    if (!start) {
        return NULL;
    }

    start = strchr(start, '>');
    if (!start) {
        return NULL;
    }
    start++; // Move past '>'

    const char* end = strstr(start, "</");
    if (!end) {
        return NULL;
    }

    size_t len = end - start;
    if (len == 0) {
        return NULL;
    }

    char* message_id = malloc(len + 1);
    if (!message_id) {
        return NULL;
    }

    strncpy(message_id, start, len);
    message_id[len] = '\0';

    return message_id;
}

/*
 * Checks if a message ID has been seen recently.
 * Returns true if it's a duplicate, false otherwise.
 */
static bool is_duplicate_message(const char* message_id)
{
    if (!message_id) {
        return false;
    }

    time_t now = time(NULL);

    for (int i = 0; i < MAX_RECENT_MESSAGES; i++) {
        // Check if the timestamp is recent (e.g., within 5 seconds) and the IDs match.
        // A 5-second window is generous enough to catch duplicates from network latency.
        if ((now - recent_messages.timestamps[i]) < 5 &&
            strcmp(recent_messages.message_ids[i], message_id) == 0) {
            return true; /* Duplicate found */
        }
    }

    /* Add this new message_id to the cache */
    strncpy(recent_messages.message_ids[recent_messages.next_index], message_id, MAX_MESSAGE_ID_LEN - 1);
    recent_messages.message_ids[recent_messages.next_index][MAX_MESSAGE_ID_LEN - 1] = '\0'; // Ensure null-termination
    recent_messages.timestamps[recent_messages.next_index] = now;
    recent_messages.next_index = (recent_messages.next_index + 1) % MAX_RECENT_MESSAGES;

    return false; /* Not a duplicate */
}

/*
 * Checks if a client is in a cool-down period.
 * Returns true if we should ignore the request, false otherwise.
 * If returning false, it updates the client's last response time.
 */
static bool is_client_in_cooldown(uint32_t client_ip) {
    time_t now = time(NULL);
    int client_index = -1;

    // Find if this client is already in the cache
    for (int i = 0; i < MAX_RECENT_CLIENTS; i++) {
        if (client_cooldown_cache.client_ips[i] == client_ip) {
            client_index = i;
            break;
        }
    }

    if (client_index != -1) {
        // Client found, check the cooldown period
        if ((now - client_cooldown_cache.last_response_times[client_index]) < RESPONSE_COOLDOWN_SECONDS) {
            return true; // In cooldown, so we should ignore the request
        }
    }

    // Client is not in cooldown, so we will respond. Update the timestamp.
    if (client_index == -1) {
        // New client, add to the cache using the next available slot
        client_index = client_cooldown_cache.next_index;
        client_cooldown_cache.client_ips[client_index] = client_ip;
        client_cooldown_cache.next_index = (client_cooldown_cache.next_index + 1) % MAX_RECENT_CLIENTS;
    }

    // Update the timestamp for this client
    client_cooldown_cache.last_response_times[client_index] = now;
    return false; // Not in cooldown
}

/* WS-Discovery thread function */
static void* discovery_thread_func(void* arg)
{
    int sock;
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_addr_len = sizeof(client_addr);
    char buffer[4096];
    char response[8192];
    char device_uuid[40];
    char message_uuid[40];

    /* Get ONVIF module configuration */
    const onvif_module_config_t* config = onvif_module_get_config();
    if (!config) {
        IMP_LOG_ERR(TAG, "Failed to get ONVIF module configuration");
        return NULL;
    }

    char model[64] = "Thingino";
    FILE *os_file = fopen("/etc/os-release", "r");
    if (os_file) {
        char line[256];
        while (fgets(line, sizeof(line), os_file)) {
            char *key = line;
            char *value = strchr(line, '=');
            if (value) {
                *value = '\0';
                value++;
                size_t len = strlen(value);
                if (len > 0 && value[len-1] == '\n') {
                    value[len-1] = '\0';
                }
                if (len > 1 && value[0] == '"' && value[len-1] == '"') {
                    memmove(value, value + 1, len - 2);
                    value[len - 2] = '\0';
                }
                if (strcmp(key, "IMAGE_ID") == 0) {
                    strncpy(model, value, sizeof(model) - 1);
                }
            }
        }
        fclose(os_file);
    }

    generate_uuid(device_uuid, sizeof(device_uuid));

    sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        IMP_LOG_ERR(TAG, "Failed to create discovery socket");
        return NULL;
    }

    /*
     * Set SO_REUSEADDR. This is crucial for multicast servers.
     * It allows the application to bind to the port even if it's in a
     * TIME_WAIT state after a restart, or if other applications need to
     * listen to the same multicast group.
     */
    int reuse = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        IMP_LOG_ERR(TAG, "Failed to set SO_REUSEADDR on discovery socket");
        close(sock);
        return NULL;
    }

    /*
     * Bind to the port BEFORE joining the multicast group.
     * This is the standard, portable sequence for setting up a multicast listener.
     */
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY); // Listen on all interfaces
    server_addr.sin_port = htons(WS_DISCOVERY_PORT);

    if (bind(sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        IMP_LOG_ERR(TAG, "Failed to bind discovery socket");
        close(sock);
        return NULL;
    }

    /* Join the multicast group */
    struct ip_mreq mreq;
    mreq.imr_multiaddr.s_addr = inet_addr(WS_DISCOVERY_ADDR);
    mreq.imr_interface.s_addr = htonl(INADDR_ANY);

    if (setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
        IMP_LOG_ERR(TAG, "Failed to join multicast group");
        close(sock);
        return NULL;
    }

    // IMP_LOG_INFO(TAG, "WS-Discovery service started on port %d", WS_DISCOVERY_PORT);

    while (discovery_running) {
        int recv_len = recvfrom(sock, buffer, sizeof(buffer) - 1, 0,
                               (struct sockaddr*)&client_addr, &client_addr_len);
        if (recv_len <= 0) {
            continue;
        }

        buffer[recv_len] = '\0';

        if (strstr(buffer, "Probe") && strstr(buffer, "NetworkVideoTransmitter")) {
            char* message_id = extract_message_id(buffer);
            if (!message_id) {
                // IMP_LOG_DBG(TAG, "Probe received without a valid MessageID. Ignoring.");
                continue;
            }

            /*
             * Check for duplicate MessageID to prevent double responses.
             * This function now handles both checking and adding to the cache.
             */
            if (is_duplicate_message(message_id)) {
                // IMP_LOG_DBG(TAG, "Ignoring duplicate WS-Discovery probe with MessageID: %s", message_id);
                free(message_id);
                continue;
            }

            /*
             * Check for client cooldown to handle clients sending multiple distinct probes.
             */
            uint32_t client_ip = client_addr.sin_addr.s_addr;
            if (is_client_in_cooldown(client_ip)) {
                //  IMP_LOG_DBG(TAG, "Ignoring probe from %s due to 1-second cooldown.", inet_ntoa(client_addr.sin_addr));
                 free(message_id);
                 continue;
            }

            generate_uuid(message_uuid, sizeof(message_uuid));

            extern streamer_config_t* g_config;
            if (!g_config || !g_config->general.server_ip || !g_config->general.http_port) {
                IMP_LOG_ERR(TAG, "Missing required configuration: server_ip or http_port not set");
                free(message_id);
                continue;
            }

            snprintf(response, sizeof(response), discovery_response_template,
                message_uuid, message_id, device_uuid, model,
                g_config->general.server_ip,
                g_config->general.http_port);

            sendto(sock, response, strlen(response), 0,
                  (struct sockaddr*)&client_addr, client_addr_len);

            // IMP_LOG_DBG(TAG, "Sent WS-Discovery response to %s:%d for MessageID: %s",
            //            inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port),
            //            message_id);

            free(message_id);
        }
    }

    close(sock);
    return NULL;
}

/* Start WS-Discovery service */
int onvif_start_discovery_service(void)
{
    if (discovery_running) {
        return 0; /* Already running */
    }

    discovery_running = 1;

    if (pthread_create(&discovery_thread, NULL, discovery_thread_func, NULL) != 0) {
        IMP_LOG_ERR(TAG, "Failed to create discovery thread");
        discovery_running = 0;
        return -1;
    }

    return 0;
}

/* Stop WS-Discovery service */
int onvif_stop_discovery_service(void)
{
    if (!discovery_running) {
        return 0;
    }

    discovery_running = 0;

    // A simple way to unblock the recvfrom call is to close the socket
    // from another thread, but a more graceful shutdown would use select() or poll()
    // with a timeout. For now, we'll rely on pthread_join.
    // To ensure the thread exits, you might need to send a final packet to the socket
    // or use other signaling mechanisms.
    if (discovery_thread) {
        pthread_join(discovery_thread, NULL);
    }

    // IMP_LOG_INFO(TAG, "WS-Discovery service stopped");
    return 0;
}
