#ifndef __HTTP_ROUTER_H__
#define __HTTP_ROUTER_H__

#include <stddef.h>

/* HTTP methods */
typedef enum {
    HTTP_METHOD_GET = 0,
    HTTP_METHOD_POST,
    HTTP_METHOD_PUT,
    HTTP_METHOD_DELETE,
    HTTP_METHOD_COUNT
} http_method_t;

/* Route handler function signature */
typedef void (*http_handler_t)(int client_socket, const char* request);

/* Route definition structure */
typedef struct {
    const char* path;           /* URL path (e.g., "/image0.jpg") */
    http_method_t method;       /* HTTP method */
    http_handler_t handler;     /* Handler function */
    const char* module_name;    /* Module that registered this route */
    const char* description;    /* Route description for API docs */
} http_route_t;

/* Route registration functions */
int http_router_init(void);
void http_router_cleanup(void);

/* Register a single route */
int http_router_register_route(const char* path, http_method_t method,
                              http_handler_t handler, const char* module_name,
                              const char* description);

/* Register multiple routes at once */
int http_router_register_routes(const http_route_t* routes, size_t count);

/* Route matching and dispatch */
int http_router_dispatch(const char* request, int client_socket);

/* Utility functions */
const char* http_method_to_string(http_method_t method);
http_method_t http_method_from_string(const char* method_str);

/* Route introspection for API documentation */
typedef struct {
    const http_route_t* route;
    struct route_list_node* next;
} route_list_node_t;

const route_list_node_t* http_router_get_routes(void);
size_t http_router_get_route_count(void);
const http_route_t* http_router_get_route(size_t index);

/* Generate JSON endpoint list for API documentation */
int http_router_generate_endpoint_list(char* buffer, size_t buffer_size);

/* Route pattern matching (for future expansion) */
typedef struct {
    char* name;     /* Parameter name */
    char* value;    /* Parameter value */
} route_param_t;

/* Advanced route matching with parameters (future) */
int http_router_match_pattern(const char* pattern, const char* path,
                             route_param_t** params, size_t* param_count);

#endif /* __HTTP_ROUTER_H__ */
