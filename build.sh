#!/bin/bash

THINGINO_DIR=/home/paul/dev/thingino-dev
OVERRIDES_DIR=/home/paul/dev/thingino-streamer
OUTPUT_DIR=/home/paul/output-wyze_cam3_t31x_gc2053_rtl8189ftv
SHARE_DIR=/home/paul/nfs

BUILDROOT_PACKAGE_DIR=$THINGINO_DIR/package/thingino-streamer

BUILD_LOG_FILE=/tmp/thingino-streamer-build.log

TARGET_PACKAGE_DIR=$OUTPUT_DIR/per-package/thingino-streamer/target

# Copy buildroot package overrides
rm -r $BUILDROOT_PACKAGE_DIR
mkdir -p $BUILDROOT_PACKAGE_DIR
cp -r $OVERRIDES_DIR/buildroot/package/thingino-streamer/* $BUILDROOT_PACKAGE_DIR/

# Clean old .config files
rm -f $OUTPUT_DIR/.config*

# Build
cd $THINGINO_DIR
BOARD=wyze_cam3_t31x_gc2053_rtl8189ftv make rebuild-thingino-streamer | tee $BUILD_LOG_FILE

# Copy streamer binary and configs to share dir
cp $TARGET_PACKAGE_DIR/usr/bin/streamer $SHARE_DIR/streamer
cp $OVERRIDES_DIR/res/*.json $SHARE_DIR/streamer.json
cp $OVERRIDES_DIR/res/config/*.json $SHARE_DIR/

# Go back to overrides dir
cd $OVERRIDES_DIR
