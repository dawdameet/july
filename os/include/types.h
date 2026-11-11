#ifndef JULYOS_TYPES_H
#define JULYOS_TYPES_H

typedef unsigned char      uint8_t;
typedef unsigned short     uint16_t;
typedef unsigned int       uint32_t;
typedef unsigned long long uint64_t;

typedef signed char        int8_t;
typedef signed short       int16_t;
typedef signed int         int32_t;
typedef signed long long   int64_t;

typedef uint64_t           size_t;
typedef uint64_t           uintptr_t;
typedef int64_t            ssize_t;

#if !defined(__STDC_VERSION__) || __STDC_VERSION__ < 202311L
#ifndef __bool_true_false_are_defined
typedef uint8_t            bool;
#define true  1
#define false 0
#define __bool_true_false_are_defined 1
#endif
#endif
#define NULL  ((void *)0)

#endif /* JULYOS_TYPES_H */

