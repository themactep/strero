/*
 * onvif_services.c - ONVIF SOAP Services (Modular)
 * Copyright (C) 2025 Paul Philippov, Thingino Project
 */

#include <ifaddrs.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <imp/imp_encoder.h>
#include <sys/socket.h>
#include <sysutils/su_base.h>

#include "../../config.h"
#include "../../common.h"
#include "onvif_module.h"

#ifdef ENABLE_HTTP
#include "../http/http_module.h"
#endif

#ifdef ENABLE_RTSP
#include "../rtsp/rtsp_module.h"
#include "../rtsp/rtsp_server.h"
#endif

#define TAG "ONVIF_SERVICES"

/* Forward declarations */
static void send_404_response(int client_socket, const char* message);
static void send_file_response(int client_socket, const char* content_type, FILE* file, long file_size);
static void send_soap_response(int client_socket, const char* action, const char* uuid, const char* body, int* sent);
// static void serve_wsdl_file(int client_socket, const char* filename);

static char* extract_soap_action_from_body(const char* request);

/* ONVIF SOAP response templates */
static const char* soap_envelope_header =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
    "<SOAP-ENV:Envelope xmlns:SOAP-ENV=\"http://www.w3.org/2003/05/soap-envelope\" xmlns:wsa=\"http://www.w3.org/2005/08/addressing\" xmlns:wsa5=\"http://www.w3.org/2005/08/addressing\" xmlns:tds=\"http://www.onvif.org/ver10/device/wsdl\" xmlns:trt=\"http://www.onvif.org/ver10/media/wsdl\" xmlns:tt=\"http://www.onvif.org/ver10/schema\" xmlns:tptz=\"http://www.onvif.org/ver20/ptz/wsdl\" xmlns:timg=\"http://www.onvif.org/ver20/imaging/wsdl\" xmlns:ter=\"http://www.onvif.org/ver10/error\" xmlns:tev=\"http://www.onvif.org/ver10/events/wsdl\" xmlns:wsnt=\"http://docs.oasis-open.org/wsn/b-2\">"
    "<SOAP-ENV:Header>"
    "<wsa:Action>%s</wsa:Action>"
    "<wsa:MessageID>urn:uuid:%s</wsa:MessageID>"
    "</SOAP-ENV:Header>"
    "<SOAP-ENV:Body>";

static const char* soap_envelope_footer =
    "</SOAP-ENV:Body>"
    "</SOAP-ENV:Envelope>";

/* Extract SOAP action from HTTP headers */
static char* extract_soap_action(const char* request)
{
    static char action[256];
    const char* action_header = strstr(request, "SOAPAction:");

    if (!action_header) {
        action_header = strstr(request, "soapaction:");
    }

    if (!action_header) {
        return NULL;
    }

    action_header += 11; /* Skip "SOAPAction:" */
    while (*action_header == ' ') action_header++; /* Skip spaces */

    /* Handle quoted values */
    if (*action_header == '"') {
        action_header++; /* Skip opening quote */

        int i = 0;
        while (i < 255 && *action_header && *action_header != '"') {
            action[i++] = *action_header++;
        }
        action[i] = '\0';
    } else {
        /* Handle unquoted values */
        int i = 0;
        while (i < 255 && *action_header && *action_header != '\r' && *action_header != '\n') {
            action[i++] = *action_header++;
        }
        action[i] = '\0';
    }

    return action;
}

/* Extract SOAP action from request body */
static char* extract_soap_action_from_body(const char* request)
{
    /* Look for SOAP action in the XML body */
    const char* body_start = strstr(request, "\r\n\r\n");
    if (!body_start) {
        IMP_LOG_DBG(TAG, "No body separator found in request");
        return NULL;
    }
    body_start += 4;

    IMP_LOG_DBG(TAG, "Extracting SOAP action from body");

    /* Find the SOAP Body section first */
    const char* body_section = strstr(body_start, "<SOAP-ENV:Body>");
    if (!body_section) {
        body_section = strstr(body_start, "<soap:Body>");
        if (!body_section) {
            IMP_LOG_DBG(TAG, "No SOAP Body section found");
            return NULL;
        }
    }

    /* Find the first namespaced element inside the SOAP Body (wildcard approach) */
    const char* action_start = NULL;
    const char* search_pos = body_section;

    while ((search_pos = strchr(search_pos, '<')) != NULL) {
        search_pos++; /* Skip the '<' */

        /* Skip whitespace */
        while (*search_pos == ' ' || *search_pos == '\t' || *search_pos == '\n' || *search_pos == '\r') {
            search_pos++;
        }

        /* Skip SOAP-ENV and soap elements */
        if (strncmp(search_pos, "SOAP-ENV:", 9) == 0 || strncmp(search_pos, "soap:", 5) == 0) {
            continue;
        }

        /* Check if this is a namespaced element (contains ':' before '>' or space) */
        const char* colon_pos = strchr(search_pos, ':');
        const char* end_pos = strpbrk(search_pos, "> \t\n\r");

        if (colon_pos && end_pos && colon_pos < end_pos) {
            /* Found a namespaced action element */
            action_start = search_pos - 1; /* Include the '<' */
            break;
        }
    }

    if (!action_start) {
        IMP_LOG_DBG(TAG, "No namespaced action element found in SOAP body");
        return NULL;
    }

    IMP_LOG_DBG(TAG, "Found ONVIF action element at: %.50s", action_start);

    /* Extract the action name */
    const char* name_start = strchr(action_start, ':');
    if (!name_start) {
        return NULL;
    }
    name_start++;

    /* Find the end of the action name - could be '>', ' ', or '/' */
    const char* name_end = name_start;
    while (*name_end && *name_end != '>' && *name_end != ' ' && *name_end != '/') {
        name_end++;
    }

    if (name_end == name_start) {
        return NULL;
    }

    size_t len = name_end - name_start;
    char* action = malloc(len + 1);
    if (!action) {
        return NULL;
    }

    strncpy(action, name_start, len);
    action[len] = '\0';

    return action;
}

/* Function declaration for the renamed safe_send */
extern int safe_send(int socket, const void* data, size_t len);

/* Function declarations */
static void handle_get_capabilities(int client_socket);
static void handle_get_services(int client_socket);
static void handle_get_device_information(int client_socket);
static void handle_get_device_service_capabilities(int client_socket);
static void handle_get_media_service_capabilities(int client_socket);
static void handle_get_profiles(int client_socket);
static void handle_get_service_capabilities(int client_socket);
static void handle_get_snapshot_uri(int client_socket);
static void handle_get_stream_uri(int client_socket, const char* request);
static void handle_get_system_date_and_time(int client_socket);
static void handle_get_video_encoder_configuration(int client_socket, const char* request);
static void handle_get_video_sources_alt(int client_socket);
static void handle_system_reboot(int client_socket);
static void handle_imaging_get_options(int client_socket);
static void handle_ptz_get_service_capabilities(int client_socket);
static void handle_event_get_service_capabilities(int client_socket);
static void handle_event_subscribe(int client_socket);
static void handle_event_unsubscribe(int client_socket);

/* Handle GetSystemDateAndTime request */
static void handle_get_system_date_and_time(int client_socket)
{
    char uuid[37];
    char body[1024];
    time_t now;
    struct tm *tm_info;

    generate_uuid(uuid, sizeof(uuid));
    IMP_LOG_INFO(TAG, "Handling GetSystemDateAndTime request");

    /* Get current time */
    time(&now);
    tm_info = localtime(&now);

    /* Get timezone information */
    char tz_str[64] = "UTC"; /* Default to UTC if we can't determine */
    FILE *tz_file = fopen("/etc/timezone", "r");
    if (tz_file) {
        if (fgets(tz_str, sizeof(tz_str), tz_file)) {
            /* Remove newline if present */
            size_t len = strlen(tz_str);
            if (len > 0 && tz_str[len-1] == '\n') {
                tz_str[len-1] = '\0';
            }
        }
        fclose(tz_file);
    }

    /* Determine if daylight savings is in effect */
    int daylight_savings = tm_info->tm_isdst > 0 ? 1 : 0;

    /* Create response body with current system time */
    snprintf(body, sizeof(body),
        "<tds:GetSystemDateAndTimeResponse>"
         "<tds:SystemDateAndTime>"
          "<tt:DateTimeType>NTP</tt:DateTimeType>"
          "<tt:DaylightSavings>%s</tt:DaylightSavings>"
          "<tt:TimeZone><tt:TZ>%s</tt:TZ></tt:TimeZone>"
          "<tt:UTCDateTime>"
           "<tt:Time><tt:Hour>%d</tt:Hour><tt:Minute>%d</tt:Minute><tt:Second>%d</tt:Second></tt:Time>"
           "<tt:Date><tt:Year>%d</tt:Year><tt:Month>%d</tt:Month><tt:Day>%d</tt:Day></tt:Date>"
          "</tt:UTCDateTime>"
         "</tds:SystemDateAndTime>"
        "</tds:GetSystemDateAndTimeResponse>",
        daylight_savings ? "true" : "false",
        tz_str,
        tm_info->tm_hour, tm_info->tm_min, tm_info->tm_sec,
        tm_info->tm_year + 1900, tm_info->tm_mon + 1, tm_info->tm_mday);

    /* Use the helper function to send the SOAP response */
    int sent = 0;
    send_soap_response(client_socket,
                      "http://www.onvif.org/ver10/device/wsdl/GetSystemDateAndTimeResponse",
                      uuid,
                      body,
                      &sent);

    /* Log the response */
    IMP_LOG_INFO(TAG, "Sent GetSystemDateAndTime response: %d bytes", sent);
}

/* Handle GetDeviceInformation request */
static void handle_get_device_information(int client_socket)
{
    char uuid[37];
    char body[1024];

    /* Variables to store OS release information */
    char manufacturer[64] = "Thingino";
    char model[64] = "Streamer";
    char firmware_version[32] = "1.0.0";
    char serial_number[64] = "123456789";
    char hardware_id[32] = "T23";

    /* Try to read information from /etc/os-release */
    FILE *os_file = fopen("/etc/os-release", "r");
    if (os_file) {
        char line[256];
        while (fgets(line, sizeof(line), os_file)) {
            char *key = line;
            char *value = strchr(line, '=');

            if (value) {
                *value = '\0'; /* Split the line at '=' */
                value++;

                /* Remove quotes and newline from value */
                size_t len = strlen(value);
                if (len > 0 && value[len-1] == '\n') {
                    value[len-1] = '\0';
                    len--;
                }
                if (len > 1 && value[0] == '"' && value[len-1] == '"') {
                    value[len-1] = '\0';
                    value++;
                }

                /* Extract relevant information */
                if (strcmp(key, "NAME") == 0) {
                    strncpy(manufacturer, value, sizeof(manufacturer) - 1);
                } else if (strcmp(key, "IMAGE_ID") == 0) {
                    strncpy(model, value, sizeof(model) - 1);
                } else if (strcmp(key, "VERSION") == 0 || strcmp(key, "VERSION_ID") == 0) {
                    strncpy(firmware_version, value, sizeof(firmware_version) - 1);
                } else if (strcmp(key, "BUILD_ID") == 0) {
                    /* Extract just the commit hash part for serial number */
                    char *hash = strstr(value, "+");
                    if (hash) {
                        strncpy(serial_number, hash + 1, sizeof(serial_number) - 1);
                        /* Remove trailing comma if present */
                        char *comma = strchr(serial_number, ',');
                        if (comma) *comma = '\0';
                    } else {
                        strncpy(serial_number, value, sizeof(serial_number) - 1);
                    }
                } else if (strcmp(key, "SOC") == 0) {
                    /* Prefix with 'T' if it's just a number */
                    if (value[0] >= '0' && value[0] <= '9') {
                        snprintf(hardware_id, sizeof(hardware_id), "T%s", value);
                    } else {
                        strncpy(hardware_id, value, sizeof(hardware_id) - 1);
                    }
                }
            }
        }
        fclose(os_file);
    }

    generate_uuid(uuid, sizeof(uuid));
    IMP_LOG_INFO(TAG, "Handling GetDeviceInformation request");

    /* Create response body using system information - without newlines between tags */
    snprintf(body, sizeof(body),
        "<tds:GetDeviceInformationResponse>"
         "<tds:Manufacturer>%s</tds:Manufacturer>"
         "<tds:Model>%s</tds:Model>"
         "<tds:FirmwareVersion>%s</tds:FirmwareVersion>"
         "<tds:SerialNumber>%s</tds:SerialNumber>"
         "<tds:HardwareId>%s</tds:HardwareId>"
        "</tds:GetDeviceInformationResponse>",
        manufacturer,
        model,
        firmware_version,
        serial_number,
        hardware_id);

    /* Log the device information */
    IMP_LOG_INFO(TAG, "Device info: Manufacturer=%s, Model=%s, FW=%s, SN=%s, HW=%s",
        manufacturer,
        model,
        firmware_version,
        serial_number,
        hardware_id);

    /* Use the helper function to send the SOAP response */
    int sent = 0;
    send_soap_response(client_socket,
                      "http://www.onvif.org/ver10/device/wsdl/GetDeviceInformationResponse",
                      uuid,
                      body,
                      &sent);

    /* Log the response */
    IMP_LOG_INFO(TAG, "Sent GetDeviceInformation response: %d bytes", sent);
}

/* Handle GetSnapshotUri request */
static void handle_get_snapshot_uri(int client_socket)
{
    char uuid[37];
    char body[512];

    generate_uuid(uuid, sizeof(uuid));
    IMP_LOG_INFO(TAG, "Handling GetSnapshotUri request");

    /* Create response body with snapshot URI */
    snprintf(body, sizeof(body),
        "<trt:GetSnapshotUriResponse>"
         "<trt:MediaUri>"
          "<tt:Uri>http://%s:%d/snap0.jpg</tt:Uri>"
          "<tt:InvalidAfterConnect>false</tt:InvalidAfterConnect>"
          "<tt:InvalidAfterReboot>false</tt:InvalidAfterReboot>"
          "<tt:Timeout>PT60S</tt:Timeout>"
         "</trt:MediaUri>"
        "</trt:GetSnapshotUriResponse>",
        g_config->general.server_ip,
        g_config->general.http_port);

    /* Use the helper function to send the SOAP response */
    int sent = 0;
    send_soap_response(client_socket,
                      "http://www.onvif.org/ver10/media/wsdl/GetSnapshotUriResponse",
                      uuid,
                      body,
                      &sent);

    /* Log the response */
    IMP_LOG_INFO(TAG, "Sent GetSnapshotUri response: %d bytes", sent);
}

/* Handle SystemReboot request */
static void handle_system_reboot(int client_socket)
{
    char uuid[37];
    char body[512];

    generate_uuid(uuid, sizeof(uuid));
    IMP_LOG_INFO(TAG, "Handling SystemReboot request");

    /* Create response body - SystemReboot returns a simple message */
    snprintf(body, sizeof(body),
        "<tds:SystemRebootResponse>"
         "<tt:Message>Rebooting in 5 seconds.</tt:Message>"
        "</tds:SystemRebootResponse>");

    /* Use the helper function to send the SOAP response */
    int sent = 0;
    send_soap_response(client_socket,
                      "http://www.onvif.org/ver10/device/wsdl/SystemRebootResponse",
                      uuid,
                      body,
                      &sent);

    /* Log the response */
    IMP_LOG_INFO(TAG, "Sent SystemReboot response: %d bytes", sent);

    /* Perform the actual reboot after sending response */
    IMP_LOG_WARN(TAG, "System reboot requested via ONVIF - rebooting device");

    /* Give some time for the response to be sent */
    usleep(500000); /* 500ms delay */

    /* Call the Ingenic SDK reboot function */
    int ret = SU_Base_Reboot();
    if (ret != 0) {
        IMP_LOG_ERR(TAG, "SU_Base_Reboot() failed with code: %d", ret);
        /* Fallback to system reboot command */
        system("reboot");
    }
}

/* Handle Imaging GetOptions request */
static void handle_imaging_get_options(int client_socket)
{
    char uuid[37];
    char body[2048];

    generate_uuid(uuid, sizeof(uuid));
    IMP_LOG_INFO(TAG, "Handling Imaging GetOptions request");

    /* Create response body with basic imaging options */
    snprintf(body, sizeof(body),
        "<timg:GetOptionsResponse xmlns:timg=\"http://www.onvif.org/ver20/imaging/wsdl\" xmlns:tt=\"http://www.onvif.org/ver10/schema\">"
         "<timg:ImagingOptions>"
          "<tt:Brightness>"
           "<tt:Min>0</tt:Min>"
           "<tt:Max>100</tt:Max>"
          "</tt:Brightness>"
          "<tt:ColorSaturation>"
           "<tt:Min>0</tt:Min>"
           "<tt:Max>100</tt:Max>"
          "</tt:ColorSaturation>"
          "<tt:Contrast>"
           "<tt:Min>0</tt:Min>"
           "<tt:Max>100</tt:Max>"
          "</tt:Contrast>"
          "<tt:Sharpness>"
           "<tt:Min>0</tt:Min>"
           "<tt:Max>100</tt:Max>"
          "</tt:Sharpness>"
         "</timg:ImagingOptions>"
        "</timg:GetOptionsResponse>");

    /* Use the helper function to send the SOAP response */
    int sent = 0;
    send_soap_response(client_socket,
                      "http://www.onvif.org/ver20/imaging/wsdl/GetOptionsResponse",
                      uuid,
                      body,
                      &sent);

    /* Log the response */
    IMP_LOG_INFO(TAG, "Sent Imaging GetOptions response: %d bytes", sent);
}

/* Handle PTZ GetServiceCapabilities request */
static void handle_ptz_get_service_capabilities(int client_socket)
{
    char uuid[37];
    char body[1024];

    generate_uuid(uuid, sizeof(uuid));
    IMP_LOG_INFO(TAG, "Handling PTZ GetServiceCapabilities request");

    /* Create response body - indicate no PTZ support */
    snprintf(body, sizeof(body),
        "<tptz:GetServiceCapabilitiesResponse xmlns:tptz=\"http://www.onvif.org/ver20/ptz/wsdl\" xmlns:tt=\"http://www.onvif.org/ver10/schema\">"
         "<tptz:Capabilities>"
          "<tt:EFlip>false</tt:EFlip>"
          "<tt:Reverse>false</tt:Reverse>"
          "<tt:GetCompatibleConfigurations>false</tt:GetCompatibleConfigurations>"
         "</tptz:Capabilities>"
        "</tptz:GetServiceCapabilitiesResponse>");

    /* Use the helper function to send the SOAP response */
    int sent = 0;
    send_soap_response(client_socket,
                      "http://www.onvif.org/ver20/ptz/wsdl/GetServiceCapabilitiesResponse",
                      uuid,
                      body,
                      &sent);

    /* Log the response */
    IMP_LOG_INFO(TAG, "Sent PTZ GetServiceCapabilities response: %d bytes", sent);
}

/* Handle Event GetServiceCapabilities request */
static void handle_event_get_service_capabilities(int client_socket)
{
    char uuid[37];
    char body[1024];

    generate_uuid(uuid, sizeof(uuid));
    IMP_LOG_INFO(TAG, "Handling Event GetServiceCapabilities request");

    /* Create response body with event service capabilities */
    snprintf(body, sizeof(body),
        "<tev:GetServiceCapabilitiesResponse>"
         "<tev:Capabilities>"
          "<tev:WSSubscriptionPolicySupport>false</tev:WSSubscriptionPolicySupport>"
          "<tev:WSPullPointSupport>false</tev:WSPullPointSupport>"
          "<tev:WSPausableSubscriptionManagerInterfaceSupport>false</tev:WSPausableSubscriptionManagerInterfaceSupport>"
          "<tev:MaxNotificationProducers>0</tev:MaxNotificationProducers>"
          "<tev:MaxPullPoints>0</tev:MaxPullPoints>"
          "<tev:PersistentNotificationStorage>false</tev:PersistentNotificationStorage>"
         "</tev:Capabilities>"
        "</tev:GetServiceCapabilitiesResponse>");

    /* Use the helper function to send the SOAP response */
    int sent = 0;
    send_soap_response(client_socket,
                      "http://www.onvif.org/ver10/events/wsdl/GetServiceCapabilitiesResponse",
                      uuid,
                      body,
                      &sent);

    /* Log the response */
    IMP_LOG_INFO(TAG, "Sent Event GetServiceCapabilities response: %d bytes", sent);
}

/* Handle Event Subscribe request */
static void handle_event_subscribe(int client_socket)
{
    char uuid[37];
    char subscription_uuid[37];
    char body[1024];

    generate_uuid(uuid, sizeof(uuid));
    generate_uuid(subscription_uuid, sizeof(subscription_uuid));
    IMP_LOG_INFO(TAG, "Handling Event Subscribe request");

    /* Get dynamic server information */
    extern streamer_config_t* g_config;
    char device_ip[64];
    char server_address[256];

    /* Get actual device IP address dynamically */
    if (get_device_ip_address(device_ip, sizeof(device_ip)) != 0) {
        /* Fallback to configured IP if detection fails */
        if (g_config && strlen(g_config->general.server_ip) > 0) {
            strncpy(device_ip, g_config->general.server_ip, sizeof(device_ip) - 1);
            device_ip[sizeof(device_ip) - 1] = '\0';
        } else {
            strcpy(device_ip, "127.0.0.1"); /* Ultimate fallback */
        }
    }

    int http_port = (g_config && g_config->general.http_port > 0) ? g_config->general.http_port : 8080;
    snprintf(server_address, sizeof(server_address), "http://%s:%d/onvif/event_service",
             device_ip, http_port);

    /* Get current time and calculate termination time (2 minutes from now) */
    time_t current_time = time(NULL);
    time_t termination_time = current_time + 120; /* 2 minutes */

    struct tm *current_tm = gmtime(&current_time);
    struct tm *termination_tm = gmtime(&termination_time);

    char current_time_str[32];
    char termination_time_str[32];
    strftime(current_time_str, sizeof(current_time_str), "%Y-%m-%dT%H:%M:%SZ", current_tm);
    strftime(termination_time_str, sizeof(termination_time_str), "%Y-%m-%dT%H:%M:%SZ", termination_tm);

    /* Create response body with subscription reference (ONVIF compliant format) */
    snprintf(body, sizeof(body),
        "<wsnt:SubscribeResponse>"
         "<wsnt:SubscriptionReference>"
          "<wsa5:Address>%s</wsa5:Address>"
          "<wsa5:ReferenceParameters>"
           "<SubscriptionId xmlns=\"http://www.onvif.org/ver10/events/wsdl\">%s</SubscriptionId>"
          "</wsa5:ReferenceParameters>"
         "</wsnt:SubscriptionReference>"
         "<wsnt:CurrentTime>%s</wsnt:CurrentTime>"
         "<wsnt:TerminationTime>%s</wsnt:TerminationTime>"
        "</wsnt:SubscribeResponse>",
        server_address, subscription_uuid, current_time_str, termination_time_str);

    /* Use the helper function to send the SOAP response */
    int sent = 0;
    send_soap_response(client_socket,
                      "http://docs.oasis-open.org/wsn/bw-2/NotificationProducer/SubscribeResponse",
                      uuid,
                      body,
                      &sent);

    /* Log the response */
    IMP_LOG_INFO(TAG, "Sent Event Subscribe response: %d bytes (subscription: %s)", sent, subscription_uuid);
}

/* Handle Event Unsubscribe request */
static void handle_event_unsubscribe(int client_socket)
{
    char uuid[37];
    char body[512];

    generate_uuid(uuid, sizeof(uuid));

    /* Log timing information to detect patterns */
    static uint32_t last_unsubscribe_time = 0;
    uint32_t current_time = get_monotonic_time_us() / 1000000; /* Convert to seconds */
    uint32_t time_since_last = current_time - last_unsubscribe_time;

    IMP_LOG_INFO(TAG, "Handling Event Unsubscribe request (time since last: %u seconds)", time_since_last);
    last_unsubscribe_time = current_time;

    /* Create response body for unsubscribe (minimal ONVIF compliant format) */
    snprintf(body, sizeof(body),
        "<wsnt:UnsubscribeResponse/>");

    /* Use the helper function to send the SOAP response */
    int sent = 0;
    send_soap_response(client_socket,
                      "http://docs.oasis-open.org/wsn/bw-2/SubscriptionManager/UnsubscribeResponse",
                      uuid,
                      body,
                      &sent);

    /* Log the response */
    IMP_LOG_INFO(TAG, "Sent Event Unsubscribe response: %d bytes", sent);
}

/* Handle GetServiceCapabilities request */
static void handle_get_service_capabilities(int client_socket)
{
    char uuid[37];
    char body[2048];

    generate_uuid(uuid, sizeof(uuid));
    IMP_LOG_INFO(TAG, "Handling GetServiceCapabilities request");

    /* Create response body with capabilities */
    snprintf(body, sizeof(body),
        "<tds:GetServiceCapabilitiesResponse>"
        "<tds:Capabilities>"
        "<tds:Network IPFilter=\"false\" ZeroConfiguration=\"false\" IPVersion6=\"false\" DynDNS=\"false\" Dot11Configuration=\"false\" HostnameFromDHCP=\"false\" NTP=\"1\" />"
        "<tds:Security TLS1.0=\"false\" TLS1.1=\"false\" TLS1.2=\"false\" OnboardKeyGeneration=\"false\" AccessPolicyConfig=\"false\" DefaultAccessPolicy=\"false\" Dot1X=\"false\" RemoteUserHandling=\"false\" X.509Token=\"false\" SAMLToken=\"false\" KerberosToken=\"false\" UsernameToken=\"false\" HttpDigest=\"false\" RELToken=\"false\" />"
        "<tds:System  DiscoveryResolve=\"true\" DiscoveryBye=\"true\" RemoteDiscovery=\"false\" SystemBackup=\"false\" SystemLogging=\"false\" FirmwareUpgrade=\"false\" HttpFirmwareUpgrade=\"false\" HttpSystemBackup=\"false\" HttpSystemLogging=\"false\" HttpSupportInformation=\"false\" />"
        "</tds:Capabilities>"
        "</tds:GetServiceCapabilitiesResponse>");

    /* Use the helper function to send the SOAP response */
    int sent = 0;
    send_soap_response(client_socket,
                      "http://www.onvif.org/ver10/device/wsdl/GetServiceCapabilitiesResponse",
                      uuid,
                      body,
                      &sent);

    /* Log the response */
    IMP_LOG_INFO(TAG, "Sent GetServiceCapabilities response: %d bytes", sent);
}

/* Handle GetStreamUri request */
static void handle_get_stream_uri(int client_socket, const char* request)
{
    char uuid[37];
    char body[1024];
    char profile_token[32] = "Profile_1"; // Default to main stream

    /* Try to extract profile token from request */
    const char* request_body = strstr(request, "\r\n\r\n");
    if (request_body) {
        request_body += 4; // Skip the empty line

        /* Try multiple patterns for ProfileToken */
        const char* token_start = NULL;

        /* Pattern 1: <ProfileToken>value</ProfileToken> */
        token_start = strstr(request_body, "ProfileToken>");
        if (token_start) {
            token_start += 13; // Skip "ProfileToken>"
        } else {
            /* Pattern 2: <trt:ProfileToken>value</trt:ProfileToken> */
            token_start = strstr(request_body, "trt:ProfileToken>");
            if (token_start) {
                token_start += 17; // Skip "trt:ProfileToken>"
            } else {
                /* Pattern 3: ProfileToken="value" */
                token_start = strstr(request_body, "ProfileToken=\"");
                if (token_start) {
                    token_start += 14; // Skip "ProfileToken=\""
                }
            }
        }

        if (token_start) {
            const char* token_end = NULL;
            if (strstr(request_body, "ProfileToken=\"")) {
                /* For attribute format, look for closing quote */
                token_end = strstr(token_start, "\"");
            } else {
                /* For element format, look for closing tag */
                token_end = strstr(token_start, "</");
            }

            if (token_end && (token_end - token_start) < sizeof(profile_token)) {
                size_t len = token_end - token_start;
                strncpy(profile_token, token_start, len);
                profile_token[len] = '\0';
                IMP_LOG_INFO(TAG, "Found ProfileToken in request: %s", profile_token);
            }
        } else {
            IMP_LOG_WARN(TAG, "ProfileToken not found in request body");
        }
    }

    generate_uuid(uuid, sizeof(uuid));
    IMP_LOG_INFO(TAG, "Handling GetStreamUri request for profile: %s", profile_token);

    /* Determine which stream to use based on profile token */
    const char* stream_name = "ch0"; // Default to channel 0
    if (strcmp(profile_token, "Profile_2") == 0) {
        stream_name = "ch1"; // Use channel 1 for Profile_2
    }

    /* Create RTSP URI */
    char rtsp_uri[128];
    int rtsp_port = 554; /* Default RTSP port */
    snprintf(rtsp_uri, sizeof(rtsp_uri), "rtsp://%s:%d/%s", g_config->general.server_ip, rtsp_port, stream_name);

    IMP_LOG_INFO(TAG, "Generated stream URI: %s", rtsp_uri);

    /* Create response body with stream URI */
    snprintf(body, sizeof(body),
        "<trt:GetStreamUriResponse>"
        "<trt:MediaUri>"
        "<tt:Uri>%s</tt:Uri>"
        "<tt:InvalidAfterConnect>false</tt:InvalidAfterConnect>"
        "<tt:InvalidAfterReboot>false</tt:InvalidAfterReboot>"
        "<tt:Timeout>PT60S</tt:Timeout>"
        "</trt:MediaUri>"
        "</trt:GetStreamUriResponse>",
        rtsp_uri);

    /* Use the helper function to send the SOAP response */
    int sent = 0;
    send_soap_response(client_socket,
                      "http://www.onvif.org/ver10/media/wsdl/GetStreamUriResponse",
                      uuid,
                      body,
                      &sent);

    /* Log the response */
    IMP_LOG_INFO(TAG, "Sent GetStreamUri response: %d bytes", sent);
}

/* Main ONVIF request handler - called from HTTP server */
void onvif_module_handle_request(int client_socket, const char* request, void* streamer_config)
{
    struct streamer_config* config = (struct streamer_config*)streamer_config;

    /* Check if ONVIF module is enabled */
    if (!onvif_module_is_enabled()) {
        IMP_LOG_WARN(TAG, "ONVIF request received but module is disabled");
        http_send_error(client_socket, HTTP_STATUS_SERVICE_UNAVAILABLE, "ONVIF service unavailable");
        return;
    }

    /* Get client information for authentication */
    client_info_t client_info;
    if (auth_get_client_info(client_socket, &client_info) < 0) {
        IMP_LOG_ERR(TAG, "Failed to get client information");
        http_send_error(client_socket, HTTP_STATUS_INTERNAL_SERVER_ERROR, "Internal server error");
        return;
    }

    /* Get ONVIF module configuration for authentication */
    const onvif_module_config_t* onvif_config = onvif_module_get_config();
    if (!onvif_config) {
        IMP_LOG_ERR(TAG, "Failed to get ONVIF configuration");
        http_send_error(client_socket, HTTP_STATUS_INTERNAL_SERVER_ERROR, "Internal server error");
        return;
    }

    /* Check if this is GetSystemDateAndTime - which should not require authentication per ONVIF spec */
    bool is_get_system_date_time = strstr(request, "<tds:GetSystemDateAndTime") != NULL;

    /* Check authentication (skip for GetSystemDateAndTime) */
    auth_result_t auth_result = AUTH_RESULT_SUCCESS;
    if (!is_get_system_date_time) {
        auth_result = auth_check_onvif_request(request, &onvif_config->auth, &client_info);
    } else {
        IMP_LOG_DBG(TAG, "GetSystemDateAndTime request - bypassing authentication per ONVIF spec");
    }

    if (auth_result == AUTH_RESULT_REQUIRED) {
        /* Send 401 Unauthorized with WWW-Authenticate header */
        IMP_LOG_INFO(TAG, "401 - Authentication required for ONVIF client %s", client_info.ip_string);

        const char* response = "HTTP/1.1 401 Unauthorized\r\n"
                              "WWW-Authenticate: Basic realm=\"Thingino ONVIF Server\"\r\n"
                              "Content-Type: text/plain\r\n"
                              "Content-Length: 12\r\n"
                              "Connection: close\r\n"
                              "\r\n"
                              "Unauthorized";
        send(client_socket, response, strlen(response), 0);
        return;
    } else if (auth_result == AUTH_RESULT_INVALID) {
        /* Send 401 Unauthorized for invalid credentials */
        IMP_LOG_WARN(TAG, "401 - Invalid credentials from ONVIF client %s", client_info.ip_string);

        const char* response = "HTTP/1.1 401 Unauthorized\r\n"
                              "WWW-Authenticate: Basic realm=\"Thingino ONVIF Server\"\r\n"
                              "Content-Type: text/plain\r\n"
                              "Content-Length: 12\r\n"
                              "Connection: close\r\n"
                              "\r\n"
                              "Unauthorized";
        send(client_socket, response, strlen(response), 0);
        return;
    } else if (auth_result == AUTH_RESULT_ERROR) {
        /* Authentication system error */
        IMP_LOG_ERR(TAG, "Authentication system error for ONVIF client %s", client_info.ip_string);
        http_send_error(client_socket, HTTP_STATUS_INTERNAL_SERVER_ERROR, "Authentication system error");
        return;
    }

    /* Authentication successful or not required */
    if (is_get_system_date_time) {
        IMP_LOG_INFO(TAG, "GetSystemDateAndTime request from %s (no auth required)", client_info.ip_string);
    } else if (auth_is_required(&onvif_config->auth, &client_info)) {
        IMP_LOG_INFO(TAG, "Authenticated ONVIF request from %s", client_info.ip_string);
    } else {
        IMP_LOG_DBG(TAG, "Localhost bypass for ONVIF client %s", client_info.ip_string);
    }

    /* Log the first line of the request */
    const char* end_of_first_line = strstr(request, "\r\n");
    if (end_of_first_line) {
        char first_line[256] = {0};
        size_t line_len = end_of_first_line - request;
        if (line_len > sizeof(first_line) - 1) {
            line_len = sizeof(first_line) - 1;
        }
        strncpy(first_line, request, line_len);
        first_line[line_len] = '\0';
        IMP_LOG_INFO(TAG, "ONVIF request received: %s", first_line);
    } else {
        IMP_LOG_INFO(TAG, "ONVIF request received (truncated): %.100s", request);
    }

    /* Log request headers */
    const char* headers_end = strstr(request, "\r\n\r\n");
    if (headers_end) {
        char headers[1024] = {0};
        size_t headers_len = headers_end - request;
        if (headers_len > sizeof(headers) - 1) {
            headers_len = sizeof(headers) - 1;
        }
        strncpy(headers, request, headers_len);
        headers[headers_len] = '\0';
        IMP_LOG_DBG(TAG, "ONVIF request headers:\n%s", headers);

        /* Log request body (truncated if too long) */
        const char* body = headers_end + 4; // Skip "\r\n\r\n"
        if (strlen(body) > 0) {
            char body_excerpt[5120] = {0};
            strncpy(body_excerpt, body, sizeof(body_excerpt) - 1);
            IMP_LOG_DBG(TAG, "ONVIF request body (excerpt):\n%s", body_excerpt);
        }
    }

    /* Extract SOAP action from headers */
    char* soap_action = NULL;

    /* First try SOAPAction header */
    const char* action_header = strstr(request, "SOAPAction:");
    if (action_header) {
        action_header += 11; /* Skip "SOAPAction:" */
        while (*action_header == ' ') action_header++; /* Skip spaces */

        if (*action_header == '"') {
            action_header++; /* Skip opening quote */
            const char* end_quote = strchr(action_header, '"');
            if (end_quote) {
                size_t len = end_quote - action_header;
                soap_action = malloc(len + 1);
                if (soap_action) {
                    strncpy(soap_action, action_header, len);
                    soap_action[len] = '\0';
                    IMP_LOG_INFO(TAG, "ONVIF SOAP Action (from SOAPAction header): %s", soap_action);
                }
            }
        }
    }

    /* If not found, try Content-Type header with action parameter */
    if (!soap_action) {
        const char* content_type = strstr(request, "Content-Type:");
        if (content_type) {
            const char* action_param = strstr(content_type, "action=\"");
            if (action_param) {
                action_param += 8; /* Skip "action=\"" */
                const char* end_quote = strchr(action_param, '"');
                if (end_quote) {
                    size_t len = end_quote - action_param;
                    soap_action = malloc(len + 1);
                    if (soap_action) {
                        strncpy(soap_action, action_param, len);
                        soap_action[len] = '\0';
                        IMP_LOG_INFO(TAG, "ONVIF SOAP Action (from Content-Type): %s", soap_action);
                    }
                }
            }
        }
    }

    /* If we couldn't extract from headers, try to extract from XML body */
    if (!soap_action) {
        soap_action = extract_soap_action_from_body(request);
        if (soap_action) {
            IMP_LOG_INFO(TAG, "ONVIF SOAP Action (from body): %s", soap_action);
        }
    }

    /* Handle ONVIF snapshot request */
    if (strstr(request, "GET /onvif/snapshot") != NULL) {
        int channel = 0;
        /* Extract channel parameter if present */
        const char* channel_param = strstr(request, "channel=");
        if (channel_param) {
            sscanf(channel_param, "channel=%d", &channel);
        }
        IMP_LOG_INFO(TAG, "ONVIF snapshot request for channel %d", channel);
        handle_snapshot_request(client_socket, channel);
        if (soap_action) free(soap_action);
        return;
    }

    /* Handle SOAP requests */
    if (strstr(request, "POST /onvif/device_service") != NULL ||
        strstr(request, "POST /onvif/media_service") != NULL ||
        strstr(request, "POST /onvif/event_service") != NULL ||
        strstr(request, "POST /onvif") != NULL) {

        char* action = extract_soap_action_from_body(request);
        if (!action && soap_action) {
            action = soap_action;
        }

        if (!action) {
            IMP_LOG_WARN(TAG, "Could not determine SOAP action");
            http_send_error(client_socket, HTTP_STATUS_BAD_REQUEST, "Could not determine SOAP action");
            return;
        }

        /* Log the service endpoint being accessed */
        if (strstr(request, "POST /onvif/device_service") != NULL) {
            IMP_LOG_INFO(TAG, "ONVIF device service request: %s", action);
        } else if (strstr(request, "POST /onvif/media_service") != NULL) {
            IMP_LOG_INFO(TAG, "ONVIF media service request: %s", action);
        } else if (strstr(request, "POST /onvif/event_service") != NULL) {
            IMP_LOG_INFO(TAG, "ONVIF event service request: %s", action);
        } else {
            IMP_LOG_INFO(TAG, "ONVIF generic service request: %s", action);
        }

        /* Handle different ONVIF actions */
        if (strstr(action, "GetDeviceInformation") != NULL) {
            handle_get_device_information(client_socket);
        } else if (strstr(action, "GetSnapshotUri") != NULL) {
            handle_get_snapshot_uri(client_socket);
        } else if (strstr(action, "GetCapabilities") != NULL) {
            handle_get_capabilities(client_socket);
        } else if (strstr(action, "GetServices") != NULL) {
            handle_get_services(client_socket);
        } else if (strstr(action, "GetSystemDateAndTime") != NULL) {
            handle_get_system_date_and_time(client_socket);
        } else if (strstr(action, "GetVideoSources") != NULL ||
                   strstr(action, "wsdlGetVideoSources") != NULL) {
            /* Use the alternative implementation */
            handle_get_video_sources_alt(client_socket);
        } else if (strstr(action, "GetProfiles") != NULL) {
            handle_get_profiles(client_socket);
        } else if (strstr(action, "GetStreamUri") != NULL) {
            handle_get_stream_uri(client_socket, request);
        } else if (strstr(action, "GetVideoEncoderConfiguration") != NULL) {
            handle_get_video_encoder_configuration(client_socket, request);
        } else if (strstr(action, "GetProfile") != NULL) {
            /* Single profile request - reuse GetProfiles handler */
            handle_get_profiles(client_socket);
        } else if (strstr(action, "GetServiceCapabilities") != NULL) {
            /* Handle based on service type */
            if (strstr(request, "POST /onvif/media_service") != NULL) {
                handle_get_media_service_capabilities(client_socket);
            } else if (strstr(request, "POST /onvif/device_service") != NULL) {
                handle_get_device_service_capabilities(client_socket);
            } else if (strstr(request, "POST /onvif/ptz_service") != NULL) {
                handle_ptz_get_service_capabilities(client_socket);
            } else if (strstr(request, "POST /onvif/event_service") != NULL) {
                handle_event_get_service_capabilities(client_socket);
            } else {
                /* Generic service capabilities */
                handle_get_service_capabilities(client_socket);
            }
        } else if (strstr(action, "SystemReboot") != NULL) {
            handle_system_reboot(client_socket);
        } else if (strstr(action, "GetOptions") != NULL && strstr(request, "/onvif/imaging_service") != NULL) {
            handle_imaging_get_options(client_socket);
        } else if (strstr(action, "Subscribe") != NULL) {
            handle_event_subscribe(client_socket);
        } else if (strstr(action, "Unsubscribe") != NULL) {
            handle_event_unsubscribe(client_socket);
        } else {
            /* Unsupported action */
            IMP_LOG_WARN(TAG, "Unsupported ONVIF action: %s", action);
            const char* error = "HTTP/1.1 501 Not Implemented\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            send(client_socket, error, strlen(error), 0);
        }

        if (action != soap_action) {
            free(action);
        }
    } else {
        IMP_LOG_WARN(TAG, "Unknown ONVIF request type");
        const char* error = "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        send(client_socket, error, strlen(error), 0);
    }

    if (soap_action) {
        free(soap_action);
    }
}

/* Send a 404 Not Found response */
static void send_404_response(int client_socket, const char* message)
{
    const char* response_message = message ? message : "Not Found";
    http_send_error(client_socket, HTTP_STATUS_NOT_FOUND, response_message);
    IMP_LOG_WARN(TAG, "Sent 404 response: %s", response_message);
}

/* Handle GetCapabilities request */
static void handle_get_capabilities(int client_socket)
{
    char uuid[37];
    char body[2048];

    generate_uuid(uuid, sizeof(uuid));
    IMP_LOG_INFO(TAG, "Handling GetCapabilities request");

    /* Get server IP and port from global config */
    const char* server_ip = g_config->general.server_ip;
    int http_port = g_config->general.http_port;

    /* Create response body with capabilities */
    snprintf(body, sizeof(body),
        "<tds:GetCapabilitiesResponse>"
         "<tds:Capabilities>"
          "<tt:Analytics><tt:XAddr>http://%s:%d/onvif/analytics_service</tt:XAddr></tt:Analytics>"
          "<tt:Device><tt:XAddr>http://%s:%d/onvif/device_service</tt:XAddr></tt:Device>"
          "<tt:Events><tt:XAddr>http://%s:%d/onvif/event_service</tt:XAddr></tt:Events>"
          "<tt:Imaging><tt:XAddr>http://%s:%d/onvif/imaging_service</tt:XAddr></tt:Imaging>"
          "<tt:Media><tt:XAddr>http://%s:%d/onvif/media_service</tt:XAddr></tt:Media>"
          "<tt:PTZ><tt:XAddr>http://%s:%d/onvif/ptz_service</tt:XAddr></tt:PTZ>"
         "</tds:Capabilities>"
        "</tds:GetCapabilitiesResponse>",
        server_ip, http_port,
        server_ip, http_port,
        server_ip, http_port,
        server_ip, http_port,
        server_ip, http_port,
        server_ip, http_port);

    /* Use the helper function to send the SOAP response */
    int sent = 0;
    send_soap_response(client_socket,
                      "http://www.onvif.org/ver10/device/wsdl/GetCapabilitiesResponse",
                      uuid,
                      body,
                      &sent);

    IMP_LOG_INFO(TAG, "Sent GetCapabilities response: %d bytes", sent);
}

/* Handle GetServices request */
static void handle_get_services(int client_socket)
{
    char uuid[37];
    char body[2048];

    generate_uuid(uuid, sizeof(uuid));
    IMP_LOG_INFO(TAG, "Handling GetServices request");

    /* Get server IP and port from global config */
    const char* server_ip = g_config->general.server_ip;
    int http_port = g_config->general.http_port;

    /* Create response body with available services */
    snprintf(body, sizeof(body),
        "<tds:GetServicesResponse>"
         "<tds:Service>"
          "<tds:Namespace>http://www.onvif.org/ver10/device/wsdl</tds:Namespace>"
          "<tds:XAddr>http://%s:%d/onvif/device_service</tds:XAddr>"
          "<tds:Version>"
           "<tt:Major>2</tt:Major>"
           "<tt:Minor>0</tt:Minor>"
          "</tds:Version>"
         "</tds:Service>"
         "<tds:Service>"
          "<tds:Namespace>http://www.onvif.org/ver10/media/wsdl</tds:Namespace>"
          "<tds:XAddr>http://%s:%d/onvif/media_service</tds:XAddr>"
          "<tds:Version>"
           "<tt:Major>2</tt:Major>"
           "<tt:Minor>0</tt:Minor>"
          "</tds:Version>"
         "</tds:Service>"
         "<tds:Service>"
          "<tds:Namespace>http://www.onvif.org/ver10/events/wsdl</tds:Namespace>"
          "<tds:XAddr>http://%s:%d/onvif/event_service</tds:XAddr>"
          "<tds:Version>"
           "<tt:Major>2</tt:Major>"
           "<tt:Minor>0</tt:Minor>"
          "</tds:Version>"
         "</tds:Service>"
         "<tds:Service>"
          "<tds:Namespace>http://www.onvif.org/ver20/imaging/wsdl</tds:Namespace>"
          "<tds:XAddr>http://%s:%d/onvif/imaging_service</tds:XAddr>"
          "<tds:Version>"
           "<tt:Major>2</tt:Major>"
           "<tt:Minor>0</tt:Minor>"
          "</tds:Version>"
         "</tds:Service>"
        "</tds:GetServicesResponse>",
        server_ip, http_port, server_ip, http_port, server_ip, http_port, server_ip, http_port);

    /* Use the helper function to send the SOAP response */
    int sent = 0;
    send_soap_response(client_socket,
                      "http://www.onvif.org/ver10/device/wsdl/GetServicesResponse",
                      uuid,
                      body,
                      &sent);

    /* Log the response */
    IMP_LOG_INFO(TAG, "Sent GetServices response: %d bytes", sent);
}

/* Handle GetVideoSources request (alternative implementation) */
static void handle_get_video_sources_alt(int client_socket) {
    char uuid[37];
    char body[1024];

    generate_uuid(uuid, sizeof(uuid));
    IMP_LOG_INFO(TAG, "Handling GetVideoSources request");

    /* Get resolution and fps from config */
    int width;
    int height;
    int fps;

    /* Use values from first stream if available */
    if (g_config && g_config->streams && g_config->stream_count > 0) {
        width = g_config->streams[0].width;
        height = g_config->streams[0].height;
        fps = g_config->sensor.fps;
    }

    /* Create response body with video source information */
    // FIXME: replace hardcoded values with real ones
    snprintf(body, sizeof(body),
        "<trt:GetVideoSourcesResponse>"
         "<trt:VideoSources token=\"VideoSource_1\">"
          "<tt:Framerate>%d</tt:Framerate>"
          "<tt:Resolution>"
           "<tt:Width>%d</tt:Width>"
           "<tt:Height>%d</tt:Height>"
          "</tt:Resolution>"
          "<tt:Imaging>"
           "<tt:Brightness>50</tt:Brightness>"
           "<tt:ColorSaturation>50</tt:ColorSaturation>"
           "<tt:Contrast>50</tt:Contrast>"
           "<tt:Sharpness>50</tt:Sharpness>"
          "</tt:Imaging>"
         "</trt:VideoSources>"
        "</trt:GetVideoSourcesResponse>",
        fps, width, height);

    /* Use the helper function to send the SOAP response */
    int sent = 0;
    send_soap_response(client_socket,
                      "http://www.onvif.org/ver10/media/wsdl/GetVideoSourcesResponse",
                      uuid,
                      body,
                      &sent);

    /* Log the response */
    IMP_LOG_INFO(TAG, "Sent GetVideoSources response: %d bytes", sent);
}

/* Forward declaration for helper functions */
static const char* get_h264_profile_name(const char* format, int stream_index);
static int get_encoder_gop_length(int stream_index);
static int get_session_timeout_seconds(void);

/* Handle GetProfiles request */
static void handle_get_profiles(int client_socket) {
    char uuid[37];
    char body[3072];

    generate_uuid(uuid, sizeof(uuid));
    IMP_LOG_INFO(TAG, "Handling GetProfiles request");

    char main_format[16];
    char sub_format[16];
    char main_profile_name[64];
    char sub_profile_name[64];

    int main_width;
    int main_height;
    int main_fps;
    int main_bitrate;
    int main_gop_length;

    int sub_width;
    int sub_height;
    int sub_fps;
    int sub_bitrate;
    int sub_gop_length;

    /* Use values from streams */
    if (g_config && g_config->streams && g_config->stream_count > 0) {
        strncpy(main_format, g_config->streams[0].format, sizeof(main_format) - 1);
        main_width = g_config->streams[0].width;
        main_height = g_config->streams[0].height;
        main_fps = g_config->sensor.fps;
        main_bitrate = g_config->streams[0].bitrate;
        main_gop_length = get_encoder_gop_length(0);

        /* Use stream info for profile name */
        snprintf(main_profile_name, sizeof(main_profile_name), "%s",
                 g_config->streams[0].rtsp_info[0] ? g_config->streams[0].rtsp_info : "Main Stream");

        if (g_config->stream_count > 1) {
            strncpy(sub_format, g_config->streams[1].format, sizeof(sub_format) - 1);
            sub_width = g_config->streams[1].width;
            sub_height = g_config->streams[1].height;
            sub_fps = g_config->sensor.fps;
            sub_bitrate = g_config->streams[1].bitrate;
            sub_gop_length = get_encoder_gop_length(1);

            /* Use stream info for profile name */
            snprintf(sub_profile_name, sizeof(sub_profile_name), "%s",
                     g_config->streams[1].rtsp_info[0] ? g_config->streams[1].rtsp_info : "Sub Stream");
        }
    }

    int session_timeout = get_session_timeout_seconds();

    /* Create response body with profiles using real configuration values */
    snprintf(body, sizeof(body),
        "<trt:GetProfilesResponse>"
         "<trt:Profiles fixed=\"true\" token=\"Profile_1\">"
          "<tt:Name>%s</tt:Name>"
          "<tt:VideoSourceConfiguration token=\"VideoSourceConfig_1\">"
           "<tt:Name>VideoSourceConfig_1</tt:Name>"
           "<tt:UseCount>1</tt:UseCount>"
           "<tt:SourceToken>VideoSource_1</tt:SourceToken>"
           "<tt:Bounds height=\"%d\" width=\"%d\" y=\"0\" x=\"0\"/>"
          "</tt:VideoSourceConfiguration>"
          "<tt:VideoEncoderConfiguration token=\"VideoEncoder_1\">"
           "<tt:Name>VideoEncoder_1</tt:Name>"
           "<tt:UseCount>1</tt:UseCount>"
           "<tt:Encoding>%s</tt:Encoding>"
           "<tt:Resolution>"
            "<tt:Width>%d</tt:Width>"
            "<tt:Height>%d</tt:Height>"
           "</tt:Resolution>"
           "<tt:Quality>%d</tt:Quality>"
           "<tt:RateControl>"
            "<tt:FrameRateLimit>%d</tt:FrameRateLimit>"
            "<tt:EncodingInterval>1</tt:EncodingInterval>"
            "<tt:BitrateLimit>%d</tt:BitrateLimit>"
           "</tt:RateControl>"
           "<tt:H264>"
            "<tt:GovLength>%d</tt:GovLength>"
            "<tt:H264Profile>%s</tt:H264Profile>"
           "</tt:H264>"
           "<tt:Multicast>"
            "<tt:Address>"
             "<tt:Type>IPv4</tt:Type>"
             "<tt:IPv4Address>%s</tt:IPv4Address>"
            "</tt:Address>"
            "<tt:Port>0</tt:Port>"
            "<tt:TTL>0</tt:TTL>"
            "<tt:AutoStart>false</tt:AutoStart>"
           "</tt:Multicast>"
           "<tt:SessionTimeout>PT%dS</tt:SessionTimeout>"
          "</tt:VideoEncoderConfiguration>"

         "</trt:Profiles>"
         "<trt:Profiles fixed=\"true\" token=\"Profile_2\">"
          "<tt:Name>%s</tt:Name>"
          "<tt:VideoSourceConfiguration token=\"VideoSourceConfig_1\">"
           "<tt:Name>VideoSourceConfig_1</tt:Name>"
           "<tt:UseCount>1</tt:UseCount>"
           "<tt:SourceToken>VideoSource_1</tt:SourceToken>"
           "<tt:Bounds height=\"%d\" width=\"%d\" y=\"0\" x=\"0\"/>"
          "</tt:VideoSourceConfiguration>"
          "<tt:VideoEncoderConfiguration token=\"VideoEncoder_2\">"
           "<tt:Name>VideoEncoder_2</tt:Name>"
           "<tt:UseCount>1</tt:UseCount>"
           "<tt:Encoding>%s</tt:Encoding>"
           "<tt:Resolution>"
            "<tt:Width>%d</tt:Width>"
            "<tt:Height>%d</tt:Height>"
           "</tt:Resolution>"
           "<tt:Quality>%d</tt:Quality>"
           "<tt:RateControl>"
            "<tt:FrameRateLimit>%d</tt:FrameRateLimit>"
            "<tt:EncodingInterval>1</tt:EncodingInterval>"
            "<tt:BitrateLimit>%d</tt:BitrateLimit>"
           "</tt:RateControl>"
           "<tt:H264>"
            "<tt:GovLength>%d</tt:GovLength>"
            "<tt:H264Profile>%s</tt:H264Profile>"
           "</tt:H264>"
           "<tt:Multicast>"
            "<tt:Address>"
             "<tt:Type>IPv4</tt:Type>"
             "<tt:IPv4Address>%s</tt:IPv4Address>"
            "</tt:Address>"
            "<tt:Port>0</tt:Port>"
            "<tt:TTL>0</tt:TTL>"
            "<tt:AutoStart>false</tt:AutoStart>"
           "</tt:Multicast>"
           "<tt:SessionTimeout>PT%dS</tt:SessionTimeout>"
          "</tt:VideoEncoderConfiguration>"

         "</trt:Profiles>"
        "</trt:GetProfilesResponse>",
        main_profile_name,                                      /* Profile_1 Name */
        main_height, main_width,                                /* Profile_1 VideoSourceConfiguration bounds */
        main_format,                                            /* Profile_1 VideoEncoderConfiguration encoding */
        main_width, main_height,                                /* Profile_1 VideoEncoderConfiguration resolution */
        5,                                                      /* Profile_1 Quality (default 5) */
        main_fps, main_bitrate,                                 /* Profile_1 VideoEncoderConfiguration rate control */
        main_gop_length,                                        /* Profile_1 GOP length */
        get_h264_profile_name(main_format, 0),                  /* Profile_1 H264Profile */
        g_config->general.server_ip,                            /* Profile_1 Multicast IPv4Address */
        session_timeout,                                        /* Profile_1 SessionTimeout */
        sub_profile_name,                                       /* Profile_2 Name */
        sub_height, sub_width,                                  /* Profile_2 VideoSourceConfiguration bounds */
        sub_format,                                             /* Profile_2 VideoEncoderConfiguration encoding */
        sub_width, sub_height,                                  /* Profile_2 VideoEncoderConfiguration resolution */
        5,                                                      /* Profile_2 Quality (default 5) */
        sub_fps, sub_bitrate,                                   /* Profile_2 VideoEncoderConfiguration rate control */
        sub_gop_length,                                         /* Profile_2 GOP length */
        get_h264_profile_name(sub_format, 1),                   /* Profile_2 H264Profile */
        g_config->general.server_ip,                            /* Profile_2 Multicast IPv4Address */
        session_timeout                                         /* Profile_2 SessionTimeout */
        );

    /* Check if response was truncated */
    size_t body_len = strlen(body);
    if (body_len >= sizeof(body) - 1) {
        IMP_LOG_ERR(TAG, "GetProfiles response truncated! Body length: %zu, Buffer size: %zu", body_len, sizeof(body));
    } else {
        IMP_LOG_DBG(TAG, "GetProfiles response body length: %zu bytes", body_len);
    }

    /* Use the helper function to send the SOAP response */
    int sent = 0;
    send_soap_response(client_socket,
                      "http://www.onvif.org/ver10/media/wsdl/GetProfilesResponse",
                      uuid,
                      body,
                      &sent);

    /* Log the response */
    IMP_LOG_INFO(TAG, "Sent GetProfiles response: %d bytes", sent);
}

/* Handle GetVideoEncoderConfiguration request */
static void handle_get_video_encoder_configuration(int client_socket, const char* request) {
    char uuid[37];
    char body[1024];
    char config_token[32] = "VideoEncoder_1"; // Default to main stream

    /* Try to extract configuration token from request */
    const char* request_body = strstr(request, "\r\n\r\n");
    if (request_body) {
        request_body += 4; // Skip the empty line

        const char* token_start = strstr(request_body, "ConfigurationToken>");
        if (token_start) {
            token_start += 19; // Skip "ConfigurationToken>"
            const char* token_end = strstr(token_start, "</");
            if (token_end && (token_end - token_start) < sizeof(config_token)) {
                size_t len = token_end - token_start;
                strncpy(config_token, token_start, len);
                config_token[len] = '\0';
                IMP_LOG_INFO(TAG, "Found ConfigurationToken in request: %s", config_token);
            }
        }
    }

    generate_uuid(uuid, sizeof(uuid));
    IMP_LOG_INFO(TAG, "Handling GetVideoEncoderConfiguration request for token: %s", config_token);

    /* Determine which stream to use based on token */
    int stream_index = 0;
    if (strcmp(config_token, "VideoEncoder_2") == 0) {
        stream_index = 1;
    }

    /* Get stream configuration values */
    int width;
    int height;
    int fps;
    int bitrate;
    int gop_length;
    char format[16];

    /* Use values from streams if available */
    if (g_config && g_config->streams && g_config->stream_count > stream_index) {
        width = g_config->streams[stream_index].width;
        height = g_config->streams[stream_index].height;
        fps = g_config->sensor.fps;
        bitrate = g_config->streams[stream_index].bitrate;
        gop_length = get_encoder_gop_length(stream_index);
        strncpy(format, g_config->streams[stream_index].format, sizeof(format) - 1);
        format[sizeof(format) - 1] = '\0';
    }

    int session_timeout = get_session_timeout_seconds();

    /* Create response body with encoder configuration using real values */
    snprintf(body, sizeof(body),
        "<trt:GetVideoEncoderConfigurationResponse>"
         "<trt:Configuration token=\"%s\">"
          "<tt:Name>%s</tt:Name>"
          "<tt:UseCount>1</tt:UseCount>"
          "<tt:Encoding>%s</tt:Encoding>"
          "<tt:Resolution>"
           "<tt:Width>%d</tt:Width>"
           "<tt:Height>%d</tt:Height>"
          "</tt:Resolution>"
          "<tt:Quality>%d</tt:Quality>"
          "<tt:RateControl>"
           "<tt:FrameRateLimit>%d</tt:FrameRateLimit>"
           "<tt:EncodingInterval>1</tt:EncodingInterval>"
           "<tt:BitrateLimit>%d</tt:BitrateLimit>"
          "</tt:RateControl>"
          "<tt:H264>"
           "<tt:GovLength>%d</tt:GovLength>"
           "<tt:H264Profile>%s</tt:H264Profile>"
          "</tt:H264>"
          "<tt:Multicast>"
           "<tt:Address>"
            "<tt:Type>IPv4</tt:Type>"
            "<tt:IPv4Address>%s</tt:IPv4Address>"
           "</tt:Address>"
           "<tt:Port>0</tt:Port>"
           "<tt:TTL>0</tt:TTL>"
           "<tt:AutoStart>false</tt:AutoStart>"
          "</tt:Multicast>"
          "<tt:SessionTimeout>PT%dS</tt:SessionTimeout>"
         "</trt:Configuration>"
        "</trt:GetVideoEncoderConfigurationResponse>",
        config_token,                                   /* Configuration token */
        config_token,                                   /* Configuration name */
        format,                                         /* Encoding format */
        width, height,                                  /* Resolution */
        5,                                              /* Quality (default 5) */
        fps, bitrate,                                   /* Rate control */
        gop_length,                                     /* GOP length */
        get_h264_profile_name(format, stream_index),    /* H264 profile */
        g_config->general.server_ip,                    /* Multicast IP */
        session_timeout                                 /* Session timeout */
        );

    /* Use the helper function to send the SOAP response */
    int sent = 0;
    send_soap_response(client_socket,
                      "http://www.onvif.org/ver10/media/wsdl/GetVideoEncoderConfigurationResponse",
                      uuid,
                      body,
                      &sent);

    /* Log the response */
    IMP_LOG_INFO(TAG, "Sent GetVideoEncoderConfiguration response: %d bytes", sent);
}

/* Handle GetMediaServiceCapabilities request */
static void handle_get_media_service_capabilities(int client_socket) {
    char uuid[37];
    char body[2048];

    generate_uuid(uuid, sizeof(uuid));
    IMP_LOG_INFO(TAG, "Handling GetMediaServiceCapabilities request");

    /* Create response body with media service capabilities */
    snprintf(body, sizeof(body),
        "<trt:GetServiceCapabilitiesResponse>"
         "<trt:Capabilities>"
          "<trt:ProfileCapabilities MaximumNumberOfProfiles=\"2\"/>"
          "<trt:StreamingCapabilities RTPMulticast=\"false\" RTP_TCP=\"true\" RTP_RTSP_TCP=\"true\" NonAggregateControl=\"false\"/>"
          "<trt:SnapshotUri>true</trt:SnapshotUri>"
          "<trt:Rotation>false</trt:Rotation>"
          "<trt:VideoSourceMode>false</trt:VideoSourceMode>"
          "<trt:OSD>false</trt:OSD>"
         "</trt:Capabilities>"
        "</trt:GetServiceCapabilitiesResponse>");

    /* Use the helper function to send the SOAP response */
    int sent = 0;
    send_soap_response(client_socket,
                      "http://www.onvif.org/ver10/media/wsdl/GetServiceCapabilitiesResponse",
                      uuid,
                      body,
                      &sent);

    /* Log the response */
    IMP_LOG_INFO(TAG, "Sent GetMediaServiceCapabilities response: %d bytes", sent);
}

/* Handle GetDeviceServiceCapabilities request */
static void handle_get_device_service_capabilities(int client_socket) {
    char uuid[37];
    char body[2048];

    generate_uuid(uuid, sizeof(uuid));
    IMP_LOG_INFO(TAG, "Handling GetDeviceServiceCapabilities request");

    /* Create response body with device service capabilities */
    snprintf(body, sizeof(body),
        "<tds:GetServiceCapabilitiesResponse>"
         "<tds:Capabilities>"
          "<tds:Network IPFilter=\"false\" ZeroConfiguration=\"false\" IPVersion6=\"false\" DynDNS=\"false\" Dot11Configuration=\"false\" HostnameFromDHCP=\"false\" NTP=\"1\" />"
          "<tds:Security TLS1.0=\"false\" TLS1.1=\"false\" TLS1.2=\"false\" OnboardKeyGeneration=\"false\" AccessPolicyConfig=\"false\" DefaultAccessPolicy=\"false\" Dot1X=\"false\" RemoteUserHandling=\"false\" X.509Token=\"false\" SAMLToken=\"false\" KerberosToken=\"false\" UsernameToken=\"false\" HttpDigest=\"false\" RELToken=\"false\" />"
          "<tds:System DiscoveryResolve=\"true\" DiscoveryBye=\"true\" RemoteDiscovery=\"false\" SystemBackup=\"false\" SystemLogging=\"false\" FirmwareUpgrade=\"false\" HttpFirmwareUpgrade=\"false\" HttpSystemBackup=\"false\" HttpSystemLogging=\"false\" HttpSupportInformation=\"false\" />"
         "</tds:Capabilities>"
        "</tds:GetServiceCapabilitiesResponse>");

    /* Use the helper function to send the SOAP response */
    int sent = 0;
    send_soap_response(client_socket,
                      "http://www.onvif.org/ver10/device/wsdl/GetServiceCapabilitiesResponse",
                      uuid,
                      body,
                      &sent);

    /* Log the response */
    IMP_LOG_INFO(TAG, "Sent GetDeviceServiceCapabilities response: %d bytes", sent);
}

/* Send SOAP response */
static void send_soap_response(int client_socket, const char* action, const char* uuid, const char* body, int* sent)
{
    /* Calculate the content length correctly */
    size_t content_length = strlen(soap_envelope_header) + strlen(body) + strlen(soap_envelope_footer);
    /* Add the length of the action and message ID strings that will be formatted into the header */
    content_length += strlen(action) + strlen(uuid);
    /* Subtract 4 for the two %s placeholders in the header */
    content_length -= 4;

    /* Build complete SOAP response */
    char soap_response[8192];
    snprintf(soap_response, sizeof(soap_response), "%s%s%s",
             soap_envelope_header, body, soap_envelope_footer);

    /* Format the header part with action and UUID */
    char formatted_response[8192];
    snprintf(formatted_response, sizeof(formatted_response), soap_response, action, uuid);

    /* Send SOAP response using HTTP utility */
    http_send_response(client_socket, HTTP_STATUS_OK, "application/soap+xml; charset=utf-8", formatted_response);
    *sent = strlen(formatted_response);
}

/* Send a file as HTTP 200 OK response */
static void send_file_response(int client_socket, const char* content_type, FILE* file, long file_size) {
    char headers[512];

    /* Read entire file into memory for HTTP utility */
    char* file_buffer = malloc(file_size);
    if (!file_buffer) {
        http_send_error(client_socket, HTTP_STATUS_INTERNAL_SERVER_ERROR, "Memory allocation failed");
        return;
    }

    /* Reset file position to beginning */
    fseek(file, 0, SEEK_SET);

    size_t bytes_read = fread(file_buffer, 1, file_size, file);
    if (bytes_read != (size_t)file_size) {
        free(file_buffer);
        http_send_error(client_socket, HTTP_STATUS_INTERNAL_SERVER_ERROR, "File read error");
        return;
    }

    /* Send file using HTTP utility */
    http_send_binary(client_socket, content_type, file_buffer, file_size);

    free(file_buffer);
    IMP_LOG_INFO(TAG, "Sent file response: %ld bytes", file_size);
}

/* Helper function to get H264/H265 profile name */
static const char* get_h264_profile_name(const char* format, int stream_index)
{
    if (!format) {
        return "Main";
    }

    /* For H265, return appropriate profile */
    if (strcasecmp(format, "H265") == 0 || strcasecmp(format, "HEVC") == 0) {
        return "Main";
    }

    /* For H264, try to get profile from encoder configuration */
    extern struct chn_conf chn[FS_CHN_NUM];
    if (stream_index < FS_CHN_NUM) {
        /* Extract profile from payloadType if available */
        uint32_t profile = (chn[stream_index].payloadType >> 16) & 0xFF;
        switch (profile) {
            case 0: return "Baseline";
            case 1: return "Main";
            case 2: return "High";
            default: return "Main";
        }
    }

    /* Default to Main profile */
    return "Main";
}

/* Helper function to get encoder GOP length */
static int get_encoder_gop_length(int stream_index)
{
    /* Calculate GOP length based on frame rate (typically 2 seconds worth of frames) */
    extern streamer_config_t* g_config;
    if (g_config && g_config->sensor.fps > 0) {
        return g_config->sensor.fps * 2; /* 2 seconds GOP */
    }

    /* Fallback to reasonable default */
    return 60;
}

/* Helper function to get session timeout from RTSP configuration */
static int get_session_timeout_seconds(void)
{
    /* Try to get session timeout from RTSP module if available */
    extern rtsp_server_t* rtsp_module_get_server(void);
    rtsp_server_t* rtsp_server = rtsp_module_get_server();

    if (rtsp_server) {
        /* RTSP server has session timeout configuration */
        /* For now, use default since we don't have direct access to config */
        return 60; /* Default 60 seconds */
    }

    /* Default session timeout */
    return 60;
}
