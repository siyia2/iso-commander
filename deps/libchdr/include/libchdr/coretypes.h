#ifndef CORETYPES_H
#define CORETYPES_H

#include <stdint.h>
#include <stddef.h>

/* Basic types required by chd.h */
typedef uint64_t UINT64;
typedef uint32_t UINT32;
typedef uint16_t UINT16;
typedef uint8_t  UINT8;

typedef int64_t  INT64;
typedef int32_t  INT32;
typedef int16_t  INT16;
typedef int8_t   INT8;

/* MAME compatibility stubs */
typedef struct core_file core_file;

#endif
