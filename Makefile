# =============================================================================
# Thingino Streamer Makefile
# =============================================================================
#
# Modular Feature Support:
# To enable optional modules, set the following flags:
#   ENABLE_AUDIO=1        - Enable audio capture and streaming module
#   ENABLE_IMP_CONTROL=1  - Enable IMP image quality control module
#   ENABLE_METRICS=1      - Enable system metrics collection and HTTP endpoints module
#   ENABLE_MOTION=1       - Enable motion detection module
#   ENABLE_OSD=1          - Enable OSD overlay module (enabled by default)
#   ENABLE_PHOTOSENSING=1 - Enable automatic day/night mode switching module
#   ENABLE_RTMP_CLIENT=1  - Enable RTMP streaming client module
#   ENABLE_RTMP_SERVER=1  - Enable RTMP streaming server module
#   ENABLE_RTSP=1         - Enable RTSP streaming server module (enabled by default)
#   ENABLE_IMAGE_GRAB=1   - Enable image grabbing module (JPEG/NV12/YUV capture)
#
# Optional Audio Codec Support:
# To enable audio codec libraries, set the following flags:
#   ENABLE_OPUS=1         - Enable Opus audio codec support
#   ENABLE_FAAC=1         - Enable FAAC audio codec support
#   ENABLE_HELIX_AAC=1    - Enable Helix AAC audio codec support
#
# Example: make ENABLE_AUDIO=1 ENABLE_OPUS=1 ENABLE_FAAC=1
# =============================================================================

# Module Configuration (Feature Flags)
# =====================================
ENABLE_AUDIO ?= 0
ENABLE_HTTP ?= 1
ENABLE_IMAGE_GRAB ?= 0
ENABLE_IMP_CONTROL ?= 1
ENABLE_METRICS ?= 1
ENABLE_MOTION ?= 1
ENABLE_OSD ?= 1
ENABLE_PHOTOSENSING ?= 1
ENABLE_RTMP_CLIENT ?= 1
ENABLE_RTMP_SERVER ?= 0
ENABLE_RTSP ?= 1

RTMPS_BACKEND_OPENSSL ?= 0
RTMPS_BACKEND_MBEDTLS ?= 0
RTSPS_BACKEND_OPENSSL ?= 0
RTSPS_BACKEND_MBEDTLS ?= 0


# Compiler Configuration
# ----------------------
CC                      = ${CROSS_COMPILE}gcc

# Compiler Flags
# --------------
CFLAGS                 ?= -Wall -Wextra -Wno-unused-parameter -O2 -DNO_OPENSSL=1 -std=c99
LDFLAGS                += -lrt -lpthread -latomic -Wl,--no-as-needed

# Kernel Version Support
# ----------------------
ifeq ($(KERNEL_VERSION_4),y)
CFLAGS                 += -DKERNEL_VERSION_4
endif

# Binary Type Configuration
# -------------------------
# Always use dynamic linking
override CFLAGS        += -DBINARY_DYNAMIC

# Library Configuration
# =====================

# Audio codec libraries (optional - controlled by flags)
AUDIO_LIBS_STATIC =
AUDIO_LIBS_DYNAMIC =
ifdef ENABLE_OPUS
    AUDIO_LIBS_STATIC += -l:libopus.a
    AUDIO_LIBS_DYNAMIC += -l:libopus.so
    CFLAGS += -DENABLE_OPUS=1
endif
ifdef ENABLE_FAAC
    AUDIO_LIBS_STATIC += -l:libfaac.a
    AUDIO_LIBS_DYNAMIC += -l:libfaac.so
    CFLAGS += -DENABLE_FAAC=1
endif
ifdef ENABLE_HELIX_AAC
    AUDIO_LIBS_STATIC += -l:libhelix-aac.a
    AUDIO_LIBS_DYNAMIC += -l:libhelix-aac.so
    CFLAGS += -DENABLE_HELIX_AAC=1
endif

# Check for libc type from CFLAGS, default to musl if not specified
# We add libmusl shim only when using musl (default if no libc type specified)

ifneq ($(MAKECMDGOALS),clean)

# Dynamic Binary Configuration
# ----------------------------
LIBS                    = -Wl,-Bdynamic \
                          -l:libimp.so \
                          -l:libjson-c.so \
                          -l:libsysutils.so \
                          -l:libwebsockets.so \
                          -latomic \
                          -lpthread -ldl -lm

# LIBS += -l:libalog.so

# Add TLS libraries based on backend selection
ifeq ($(RTMPS_BACKEND_OPENSSL),1)
    LIBS += -l:libssl.so -l:libcrypto.so
endif

ifeq ($(RTMPS_BACKEND_MBEDTLS),1)
    LIBS += -l:libmbedtls.so -l:libmbedx509.so
endif
ifneq (,$(findstring -DLIBC_GLIBC,$(CFLAGS)))
	# GLIBC - no additional libraries needed
else ifneq (,$(findstring -DLIBC_UCLIBC,$(CFLAGS)))
	# uClibc - no additional libraries needed
else
	# Default to musl
LIBS                   += -l:libmuslshim.so -latomic
endif

endif

# Platform-Specific Include and Library Directories
# =================================================
ifneq (,$(findstring -DPLATFORM_C100,$(CFLAGS)))
	LIBIMP_INC_DIR          = ./include/C100/2.1.0/en
	LIBIMP_LIB_DIR          = ./lib/C100/lib/2.1.0/uclibc/5.4.0
else ifneq (,$(findstring -DPLATFORM_T10,$(CFLAGS)))
	LIBIMP_INC_DIR          = ./include/T20/3.12.0/zh
	LIBIMP_LIB_DIR          = ./lib/T20/lib/3.12.0/uclibc/5.4.0
else ifneq (,$(findstring -DPLATFORM_T20,$(CFLAGS)))
	LIBIMP_INC_DIR          = ./include/T20/3.12.0/zh
	LIBIMP_LIB_DIR          = ./lib/T20/lib/3.12.0/uclibc/5.4.0
else ifneq (,$(findstring -DPLATFORM_T21,$(CFLAGS)))
	LIBIMP_INC_DIR          = ./include/T21/1.0.33/zh
	LIBIMP_LIB_DIR          = ./lib/T21/lib/1.0.33/uclibc/5.4.0
else ifneq (,$(findstring -DPLATFORM_T23,$(CFLAGS)))
	LIBIMP_INC_DIR          = ./include/T23/1.1.0/zh
	LIBIMP_LIB_DIR          = ./lib/T23/lib/1.1.0/uclibc/5.4.0
else ifneq (,$(findstring -DPLATFORM_T30,$(CFLAGS)))
	LIBIMP_INC_DIR          = ./include/T30/1.0.5/zh
	LIBIMP_LIB_DIR          = ./lib/T30/lib/1.0.5/uclibc/5.4.0
else ifneq (,$(findstring -DPLATFORM_T31,$(CFLAGS)))
	LIBIMP_INC_DIR          = ./include/T31/1.1.6/en
	LIBIMP_LIB_DIR          = ./lib/T31/lib/1.1.6/uclibc/5.4.0
else ifneq (,$(findstring -DPLATFORM_T32,$(CFLAGS)))
	LIBIMP_INC_DIR          = ./include/T32/1.0.4/en
	LIBIMP_LIB_DIR          = ./lib/T32/lib/1.0.4/uclibc/5.4.0
else ifneq (,$(findstring -DPLATFORM_T40,$(CFLAGS)))
	LIBIMP_INC_DIR          = ./include/T40/1.2.0/zh
	LIBIMP_LIB_DIR          = ./lib/T40/lib/1.2.0/uclibc/5.4.0
else ifneq (,$(findstring -DPLATFORM_T41,$(CFLAGS)))
	LIBIMP_INC_DIR          = ./include/T41/1.2.0/zh
	LIBIMP_LIB_DIR          = ./lib/T41/lib/1.2.0/uclibc/5.4.0
endif

# Directory Structure
# ===================
SRC_DIR                 = ./src
OBJ_DIR                 = ./obj
BIN_DIR                 = ./bin
THIRDPARTY_INC_DIR      = ./3rdparty/install/include

# Source and Object Files
# =======================
# Core source files (always included)
C_SOURCES_CORE          = $(SRC_DIR)/main.c \
                          $(SRC_DIR)/common.c \
                          $(SRC_DIR)/config.c \
                          $(SRC_DIR)/sensor.c \
                          $(SRC_DIR)/module_system.c \
                          $(SRC_DIR)/frame_manager.c \
                          $(SRC_DIR)/auth_utils.c \
                          $(SRC_DIR)/snapshot_fallback.c

# Module source files (conditionally included based on feature flags)
C_SOURCES_MODULES       =

# Include audio module if enabled
ifeq ($(ENABLE_AUDIO),1)
    CFLAGS += -DENABLE_AUDIO
    LIBS += -l:libaudioProcess.so $(AUDIO_LIBS_DYNAMIC)
    include src/modules/audio/Makefile.audio
    # Add audio objects to build
    EXTRA_OBJECTS += $(AUDIO_OBJECTS)
endif

# Include HTTP module if enabled (enabled by default)
ifeq ($(ENABLE_HTTP),1)
    CFLAGS += -DENABLE_HTTP
    include src/modules/http/Makefile.http
    # Add HTTP objects to build
    EXTRA_OBJECTS += $(HTTP_OBJECTS)
endif

# Include IMP control module if enabled (enabled by default)
ifeq ($(ENABLE_IMP_CONTROL),1)
    CFLAGS += -DENABLE_IMP_CONTROL
    include src/modules/imp_control/Makefile.imp_control
    # Add IMP control objects to build
    EXTRA_OBJECTS += $(IMP_CONTROL_OBJECTS)
endif

# Include metrics module if enabled
ifeq ($(ENABLE_METRICS),1)
    CFLAGS += -DENABLE_METRICS
    include src/modules/metrics/Makefile.metrics
    # Add metrics objects to build
    EXTRA_OBJECTS += $(METRICS_OBJECTS)
endif

# Include motion module if enabled
ifeq ($(ENABLE_MOTION),1)
    CFLAGS += -DENABLE_MOTION_MODULE
    include src/modules/motion/Makefile.motion
    # Add motion objects to build
    EXTRA_OBJECTS += $(MOTION_OBJECTS)
endif

# Include ONVIF module if enabled
ifeq ($(ENABLE_ONVIF),1)
    CFLAGS += -DENABLE_ONVIF
    include src/modules/onvif/Makefile.onvif
    # Add ONVIF objects to build
    EXTRA_OBJECTS += $(ONVIF_OBJECTS)
endif

# Include OSD module if enabled (enabled by default)
ifeq ($(ENABLE_OSD),1)
    CFLAGS += -DENABLE_OSD
    LIBS += -l:libschrift.so
    include src/modules/osd/Makefile.osd
    # Add OSD objects to build
    EXTRA_OBJECTS += $(OSD_OBJECTS)
endif

# Include photosensing module if enabled
ifeq ($(ENABLE_PHOTOSENSING),1)
    CFLAGS += -DENABLE_PHOTOSENSING
    include src/modules/photosensing/Makefile.photosensing
    # Add photosensing objects to build
    EXTRA_OBJECTS += $(PHOTOSENSING_OBJECTS)
endif

# Include RTMP server module if enabled
ifeq ($(ENABLE_RTMP_SERVER),1)
    CFLAGS += -DENABLE_RTMP_SERVER
    include src/modules/rtmp_server/Makefile.rtmp_server
    # Add RTMP server objects to build
    EXTRA_OBJECTS += $(RTMP_SERVER_OBJECTS)
endif

# Include RTMP client module if enabled
ifeq ($(ENABLE_RTMP_CLIENT),1)
    CFLAGS += -DENABLE_RTMP_CLIENT
    include src/modules/rtmp_client/Makefile.rtmp_client
    # Add RTMP client objects to build
    EXTRA_OBJECTS += $(RTMP_CLIENT_OBJECTS)
endif

# Enable RTMPS (TLS) support if enabled
ifeq ($(ENABLE_RTMPS),1)
    CFLAGS += -DENABLE_RTMPS
endif

# Include RTSP module if enabled (enabled by default)
ifeq ($(ENABLE_RTSP),1)
    CFLAGS += -DENABLE_RTSP
    include src/modules/rtsp/Makefile.rtsp
    # Add RTSP objects to build
    EXTRA_OBJECTS += $(RTSP_OBJECTS)
endif

# Include image grab module if enabled
ifeq ($(ENABLE_IMAGE_GRAB),1)
    CFLAGS += -DENABLE_IMAGE_GRAB
    include src/modules/image_grab/Makefile.image_grab
    # Add image grab objects to build
    EXTRA_OBJECTS += $(IMAGE_GRAB_OBJECTS)
endif

# Enable RTSPS (TLS) support if enabled
ifeq ($(ENABLE_RTSPS),1)
    CFLAGS += -DENABLE_RTSPS
endif

# Add TLS backend flags
ifeq ($(RTMPS_BACKEND_OPENSSL),1)
    CFLAGS += -DRTMPS_BACKEND_OPENSSL
endif

ifeq ($(RTMPS_BACKEND_MBEDTLS),1)
    CFLAGS += -DRTMPS_BACKEND_MBEDTLS
endif

# Add RTSPS TLS backend flags (reuse RTMPS backend selection)
ifeq ($(ENABLE_RTSPS),1)
    ifeq ($(RTMPS_BACKEND_OPENSSL),1)
        CFLAGS += -DRTSPS_BACKEND_OPENSSL
    endif

    ifeq ($(RTMPS_BACKEND_MBEDTLS),1)
        CFLAGS += -DRTSPS_BACKEND_MBEDTLS
    endif
endif

# Use core source files plus enabled modules
C_SOURCES_WITH_STUBS = $(C_SOURCES_CORE) $(C_SOURCES_MODULES)

# Conditionally include compatibility shims based on libc type
# Use deferred evaluation to check CFLAGS at build time
C_SOURCES = $(C_SOURCES_WITH_STUBS) $(if $(findstring -DLIBC_UCLIBC,$(CFLAGS)),$(SRC_DIR)/glibc_uclibc_compat.c,$(if $(findstring -DLIBC_GLIBC,$(CFLAGS)),,$(SRC_DIR)/uclibc_musl_shim.c))

# Include only the files we need for pure C build
SOURCES                 = $(C_SOURCES)
OBJECTS                 = $(patsubst $(SRC_DIR)/%.c,$(OBJ_DIR)/%.o,$(C_SOURCES)) $(EXTRA_OBJECTS)

$(info Building objects: $(OBJECTS))

# Target Configuration
# ====================
TARGET                  = $(BIN_DIR)/streamer

# Version Management
# ==================
ifndef commit_tag
commit_tag              = $(shell git rev-parse --short HEAD)
endif

VERSION_FILE            = $(LIBIMP_INC_DIR)/version.h

# Build Options
# =============
STRIP_FLAG              := $(if $(filter 0,$(DEBUG_STRIP)),,"-s")

# =============================================================================
# Build Rules
# =============================================================================

# Version File Generation
# -----------------------
$(VERSION_FILE): src/version.tpl.h
	@if ! grep -q "$(commit_tag)" $(VERSION_FILE) > /dev/null 2>&1; then \
		echo "Updating version.h to $(commit_tag)"; \
		sed 's/COMMIT_TAG/"$(commit_tag)"/g' src/version.tpl.h > $(VERSION_FILE); \
	fi

# C Object Compilation
# --------------------
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c $(VERSION_FILE)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) \
		-I$(LIBIMP_INC_DIR) \
		-I$(LIBIMP_INC_DIR)/imp \
		-I$(LIBIMP_INC_DIR)/sysutils \
		-isystem $(THIRDPARTY_INC_DIR) \
		-c $< -o $@

# Final Binary Linking
# --------------------
$(TARGET): $(OBJECTS) $(VERSION_FILE)
	@mkdir -p $(@D)
	$(CCACHE) $(CC) -L$(LIBIMP_LIB_DIR) $(LDFLAGS) -o $@ $(OBJECTS) $(LIBS) $(STRIP_FLAG)

# =============================================================================
# Phony Targets
# =============================================================================

.PHONY: all clean distclean t31 install

# Default Target
# --------------
all: $(TARGET)

# Special Targets for Different Platform/SDK Combinations
# -------------------------------------------------------

# Standard T31 hardware with T31 SDK
t31:
	@echo "Building for T31 hardware with T31 SDK..."
	$(MAKE) CFLAGS="$(CFLAGS) -DPLATFORM_T31" $(TARGET)
	@echo "Build complete: T31 hardware + T31 SDK"

# Clean Build Artifacts
# ---------------------
clean:
	@echo "Cleaning build artifacts..."
	rm -rf $(OBJ_DIR)
	rm -f $(LIBIMP_INC_DIR)/version.h

# Complete Clean
# --------------
distclean: clean
	@echo "Cleaning all generated files..."
	rm -rf $(BIN_DIR)

# Installation
# ------------
install: $(TARGET)
	mkdir -p $(DESTDIR)/usr/bin
	mkdir -p $(DESTDIR)/etc
	mkdir -p $(DESTDIR)/etc/streamer.d
	install -m 755 $(TARGET) $(DESTDIR)/usr/bin/
	install -m 644 res/streamer.json $(DESTDIR)/etc/
	install -m 644 res/config/*.json $(DESTDIR)/etc/streamer.d/
