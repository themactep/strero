/*
 * http_router.c - HTTP Dynamic Routing Implementation
 * Implements dynamic HTTP routing for modular architecture
 * Supports registering routes with handlers and dispatching requests
 * Also includes API documentation generation and introspection
 * Copyright (C) 2025 Paul Philippov, Thingino Project
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "http_router.h"
#include "../../common.h"

#define TAG "HTTP_ROUTER"

/* Maximum number of routes */
#define MAX_ROUTES 128

/* Route storage */
static http_route_t routes[MAX_ROUTES];
static size_t route_count = 0;
static int router_initialized = 0;

/* Method string mapping */
static const char* method_strings[] = {
    [HTTP_METHOD_GET] = "GET",
    [HTTP_METHOD_POST] = "POST",
    [HTTP_METHOD_PUT] = "PUT",
    [HTTP_METHOD_DELETE] = "DELETE"
};

int http_router_init(void)
{
    if (router_initialized) {
        return 0;
    }

    route_count = 0;
    memset(routes, 0, sizeof(routes));
    router_initialized = 1;

    IMP_LOG_INFO(TAG, "HTTP router initialized");
    return 0;
}

void http_router_cleanup(void)
{
    route_count = 0;
    router_initialized = 0;
    IMP_LOG_INFO(TAG, "HTTP router cleaned up");
}

int http_router_register_route(const char* path, http_method_t method,
                              http_handler_t handler, const char* module_name,
                              const char* description)
{
    if (!router_initialized) {
        IMP_LOG_ERR(TAG, "Router not initialized");
        return -1;
    }

    if (!path || !handler || !module_name) {
        IMP_LOG_ERR(TAG, "Invalid route parameters");
        return -1;
    }

    if (route_count >= MAX_ROUTES) {
        IMP_LOG_ERR(TAG, "Maximum routes exceeded (%d)", MAX_ROUTES);
        return -1;
    }

    if (method >= HTTP_METHOD_COUNT) {
        IMP_LOG_ERR(TAG, "Invalid HTTP method %d", method);
        return -1;
    }

    /* Check for duplicate routes */
    for (size_t i = 0; i < route_count; i++) {
        if (routes[i].method == method && strcmp(routes[i].path, path) == 0) {
            IMP_LOG_WARN(TAG, "Route %s %s already registered by %s, overriding with %s",
                        method_strings[method], path, routes[i].module_name, module_name);
            routes[i].handler = handler;
            routes[i].module_name = module_name;
            routes[i].description = description;
            return 0;
        }
    }

    /* Add new route */
    routes[route_count].path = path;
    routes[route_count].method = method;
    routes[route_count].handler = handler;
    routes[route_count].module_name = module_name;
    routes[route_count].description = description;
    route_count++;

    IMP_LOG_DBG(TAG, "Registered route: %s %s (%s) - %s",
                method_strings[method], path, module_name,
                description ? description : "No description");

    return 0;
}

int http_router_register_routes(const http_route_t* route_list, size_t count)
{
    if (!route_list) {
        return -1;
    }

    for (size_t i = 0; i < count; i++) {
        int ret = http_router_register_route(
            route_list[i].path,
            route_list[i].method,
            route_list[i].handler,
            route_list[i].module_name,
            route_list[i].description
        );
        if (ret < 0) {
            IMP_LOG_ERR(TAG, "Failed to register route %zu: %s %s",
                       i, method_strings[route_list[i].method], route_list[i].path);
            return ret;
        }
    }

    IMP_LOG_INFO(TAG, "Registered %zu routes", count);
    return 0;
}

/* Extract HTTP method from request */
static http_method_t extract_method(const char* request)
{
    if (strncmp(request, "GET ", 4) == 0) return HTTP_METHOD_GET;
    if (strncmp(request, "POST ", 5) == 0) return HTTP_METHOD_POST;
    if (strncmp(request, "PUT ", 4) == 0) return HTTP_METHOD_PUT;
    if (strncmp(request, "DELETE ", 7) == 0) return HTTP_METHOD_DELETE;
    return HTTP_METHOD_COUNT; /* Invalid */
}

/* Extract path from request */
static int extract_path(const char* request, char* path_buffer, size_t buffer_size)
{
    const char* start = strchr(request, ' ');
    if (!start) return -1;
    start++; /* Skip space */

    const char* end = strchr(start, ' ');
    if (!end) return -1;

    size_t path_len = end - start;
    if (path_len >= buffer_size) return -1;

    /* Extract path without query parameters */
    const char* query = strchr(start, '?');
    if (query && query < end) {
        path_len = query - start;
    }

    strncpy(path_buffer, start, path_len);
    path_buffer[path_len] = '\0';

    return 0;
}

int http_router_dispatch(const char* request, int client_socket)
{
    if (!router_initialized) {
        IMP_LOG_ERR(TAG, "Router not initialized");
        return -1;
    }

    if (!request) {
        IMP_LOG_ERR(TAG, "Invalid request");
        return -1;
    }

    /* Extract method and path */
    http_method_t method = extract_method(request);
    if (method == HTTP_METHOD_COUNT) {
        IMP_LOG_ERR(TAG, "Invalid HTTP method in request");
        return -1;
    }

    char path[256];
    if (extract_path(request, path, sizeof(path)) < 0) {
        IMP_LOG_ERR(TAG, "Failed to extract path from request");
        return -1;
    }

    /* Find matching route */
    for (size_t i = 0; i < route_count; i++) {
        if (routes[i].method == method && strcmp(routes[i].path, path) == 0) {
            IMP_LOG_DBG(TAG, "Dispatching %s %s to %s",
                       method_strings[method], path, routes[i].module_name);
            routes[i].handler(client_socket, request);
            return 0;
        }
    }

    /* No route found */
    IMP_LOG_DBG(TAG, "No route found for %s %s", method_strings[method], path);
    return -1;
}

const char* http_method_to_string(http_method_t method)
{
    if (method < HTTP_METHOD_COUNT) {
        return method_strings[method];
    }
    return "UNKNOWN";
}

http_method_t http_method_from_string(const char* method_str)
{
    if (!method_str) return HTTP_METHOD_COUNT;

    for (int i = 0; i < HTTP_METHOD_COUNT; i++) {
        if (strcmp(method_str, method_strings[i]) == 0) {
            return (http_method_t)i;
        }
    }
    return HTTP_METHOD_COUNT;
}

size_t http_router_get_route_count(void)
{
    return route_count;
}

/* Simple route listing - for API documentation generation */
const http_route_t* http_router_get_route(size_t index)
{
    if (index >= route_count) {
        return NULL;
    }
    return &routes[index];
}

/* Generate JSON endpoint list for API documentation */
int http_router_generate_endpoint_list(char* buffer, size_t buffer_size)
{
    if (!buffer || buffer_size == 0) {
        return -1;
    }

    size_t len = 0;

    for (size_t i = 0; i < route_count; i++) {
        if (len >= buffer_size - 20) { /* Reserve space for closing */
            break;
        }

        /* Add comma separator except for first item */
        if (i > 0) {
            len += snprintf(buffer + len, buffer_size - len, ",");
        }

        /* Add endpoint path */
        len += snprintf(buffer + len, buffer_size - len, "\"%s\"", routes[i].path);
    }

    return (int)len;
}
