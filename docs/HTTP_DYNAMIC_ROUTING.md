# HTTP Dynamic Routing System

## Overview

The HTTP Dynamic Routing System replaces the previous hardcoded string matching approach with a flexible, modular routing system where modules can register their own HTTP endpoints dynamically.

## Benefits

### **Before (Hardcoded Routing)**
```c
if (strstr(request, "GET /image0.jpg") != NULL) {
    extern void handle_image0_jpg(int client_socket, const char* query_string);
    handle_image0_jpg(client_socket, request);
} else if (strstr(request, "GET /image1.jpg") != NULL) {
    extern void handle_image1_jpg(int client_socket, const char* query_string);
    handle_image1_jpg(client_socket, request);
}
// ... many more hardcoded routes
```

### **After (Dynamic Routing)**
```c
// Modules register routes during initialization
image_grab_register_routes();

// HTTP module dispatches dynamically
if (http_router_dispatch(request, client_socket) == 0) {
    /* Route handled by dynamic router */
}
```

## Architecture

### **1. Router Core** (`http_router.h/c`)
- Route registration and storage
- Request parsing and dispatching
- Route introspection for API documentation

### **2. Module Integration**
- Modules register routes during initialization
- Clean separation of concerns
- Automatic API documentation generation

### **3. HTTP Module Integration**
- Router initialization during HTTP module start
- Authentication checking before route dispatching
- Dynamic route dispatching
- Automatic endpoint listing in API overview
- Snapshot fallback system integration (activates when HTTP unavailable)

## Module Route Registration

### **Step 1: Define Routes**
```c
static const http_route_t image_grab_routes[] = {
    {"/image0.jpg", HTTP_METHOD_GET, handle_image0_jpg, "image_grab", "Channel 0 JPEG capture"},
    {"/image1.jpg", HTTP_METHOD_GET, handle_image1_jpg, "image_grab", "Channel 1 JPEG capture"},
    {"/image0.nv12", HTTP_METHOD_GET, handle_image0_nv12, "image_grab", "Channel 0 NV12 raw capture"},
    // ... more routes
};
```

### **Step 2: Registration Function**
```c
int image_grab_register_routes(void)
{
    return http_router_register_routes(image_grab_routes,
                                     sizeof(image_grab_routes) / sizeof(image_grab_routes[0]));
}
```

### **Step 3: Module Header**
```c
/* Route registration function */
int image_grab_register_routes(void);
```

### **Step 4: HTTP Module Integration**
```c
#ifdef ENABLE_IMAGE_GRAB
    extern int image_grab_register_routes(void);
    if (image_grab_register_routes() < 0) {
        IMP_LOG_ERR(TAG, "Failed to register image grab routes");
        return -1;
    }
#endif
```

## API Features

### **Route Structure**
```c
typedef struct {
    const char* path;           /* URL path (e.g., "/image0.jpg") */
    http_method_t method;       /* HTTP method */
    http_handler_t handler;     /* Handler function */
    const char* module_name;    /* Module that registered this route */
    const char* description;    /* Route description for API docs */
} http_route_t;
```

### **Supported HTTP Methods**
- `HTTP_METHOD_GET`
- `HTTP_METHOD_POST`
- `HTTP_METHOD_PUT`
- `HTTP_METHOD_DELETE`

### **Handler Function Signature**
```c
typedef void (*http_handler_t)(int client_socket, const char* request);
```

## Dynamic API Documentation

### **Automatic Endpoint Listing**
The router automatically generates:

1. **API Overview Page** - Dynamic HTML with all registered routes
2. **JSON Endpoint List** - Machine-readable endpoint list in `/config.json`
3. **Route Introspection** - Access to route metadata for documentation

### **Example API Overview Output**
```html
<h2>Dynamic Module Endpoints</h2>
<ul>
<li><a href="/image0.jpg">GET /image0.jpg</a> - Channel 0 JPEG capture (image_grab)</li>
<li><a href="/image1.jpg">GET /image1.jpg</a> - Channel 1 JPEG capture (image_grab)</li>
<li><a href="/image0.nv12">GET /image0.nv12</a> - Channel 0 NV12 raw capture (image_grab)</li>
</ul>
```

## Performance Benefits

### **Reduced Code Duplication**
- Single routing logic instead of repeated string matching
- Centralized request parsing
- Consistent error handling

### **Better Maintainability**
- Modules manage their own routes
- No need to modify HTTP module for new endpoints
- Clear separation of concerns

### **Scalability**
- Easy to add new modules with HTTP endpoints
- No hardcoded limits on number of routes
- Efficient route lookup

## Future Enhancements

### **Pattern Matching** (Planned)
```c
// Future: Support for parameterized routes
{"/image/{channel}.jpg", HTTP_METHOD_GET, handle_image_dynamic, ...}
```

### **Middleware Support** (Planned)
```c
// Future: Authentication, logging, rate limiting
http_router_add_middleware(auth_middleware);
```

### **Route Groups** (Planned)
```c
// Future: Group related routes
http_router_group("/api/v1", api_v1_routes);
```

## Migration Guide

### **For New Modules**
1. Define route array with `http_route_t` structures
2. Create registration function
3. Add registration call to HTTP module initialization
4. Add conditional compilation guards

### **For Existing Modules**
1. Replace hardcoded string matching with route registration
2. Move handler functions to module files
3. Update build system to include new dependencies
4. Test route registration and dispatching

## Conclusion

The Dynamic HTTP Routing System provides a clean, scalable foundation for HTTP endpoint management in the thingino-streamer. It eliminates hardcoded routing, improves maintainability, and enables automatic API documentation generation while maintaining high performance and low resource usage.
