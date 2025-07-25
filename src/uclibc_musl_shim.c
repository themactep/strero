/*
 * uclibc_musl_shim.c - uClibc to musl compatibility shim
 * This file provides compatibility functions for Ingenic libraries that
 * were compiled with uClibc but need to run with musl. Also provides
 * Ingenic-specific function stubs for Ingenic hardware.
 * Only compiled when using musl - excluded from uclibc builds via
 * Makefile.
 * Copyright (C) 2025 Paul Philippov, Thingino Project
 */

#define _GNU_SOURCE
#include <sys/stat.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

#include <sys/stat.h>

/*
 * uClibc uses stat64/lseek64 functions that don't exist in musl.
 * In musl, the regular stat/lseek functions are already 64-bit capable.
 */

int stat64(const char* pathname, struct stat* statbuf)
{
    return stat(pathname, statbuf);
}

off_t lseek64(int fd, off_t offset, int whence)
{
    return lseek(fd, offset, whence);
}

int fstat64(int fd, struct stat* statbuf)
{
    return fstat(fd, statbuf);
}

int lstat64(const char* pathname, struct stat* statbuf)
{
    return lstat(pathname, statbuf);
}
