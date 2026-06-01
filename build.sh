#!/bin/bash

BOARD=$1
if [[ -z "$BOARD" ]]; then
	echo "$0 <camera_name>"
	exit 1
fi

BRANCH=master
THINGINO_DIR=$HOME/thingino/firmware/$BRANCH
OVERRIDES_DIR=$HOME/thingino/overrides/thingino-streamer
OUTPUT_DIR=$THINGINO_DIR/output/$BRANCH/$BOARD
SHARE_DIR=$HOME/nfs

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
BOARD=$BOARD make rebuild-thingino-streamer | tee $BUILD_LOG_FILE

# Copy streamer binary and configs to share dir
cp $TARGET_PACKAGE_DIR/usr/bin/streamer $SHARE_DIR/streamer
cp $OVERRIDES_DIR/res/*.json $SHARE_DIR/streamer.json
cp $OVERRIDES_DIR/res/config/*.json $SHARE_DIR/

# Go back to overrides dir
cd $OVERRIDES_DIR

