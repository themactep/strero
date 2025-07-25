/**
 * glibc_uclibc_compat.c - glibc to uclibc compatibility shim
 * This file provides compatibility functions for libraries
 * that were compiled with glibc but need to run with uclibc.
 * These functions handle the differences between glibc and uclibc.
 * Copyright (C) 2025 Paul Philippov, Thingino Project
 */

#include <ctype.h>
#include <stddef.h>

/**
 * glibc provides __ctype_b_loc() and __ctype_tolower_loc() functions
 * that return pointers to ctype tables. uclibc doesn't have these.
 * We need to provide compatible implementations.
 */

/* uclibc ctype compatibility */
const unsigned short** __ctype_b_loc(void)
{
    static const unsigned short* ctype_b = NULL;
    static const unsigned short** ctype_b_ptr = &ctype_b;
    return ctype_b_ptr;
}

const int** __ctype_tolower_loc(void)
{
    static const int* ctype_tolower = NULL;
    static const int** ctype_tolower_ptr = &ctype_tolower;
    return ctype_tolower_ptr;
}
