/* Minimal <stdint.h> for the v810 toolchain, which ships no
 * libc headers. Fixed-width types for the V810 (char 8,
 * short 16, int 32). Used only by the Virtual Boy build of
 * the Z-machine core (zmachine.h); the host harness uses
 * the system <stdint.h>. */
#ifndef ZM_VB_STDINT_SHIM_H
#define ZM_VB_STDINT_SHIM_H

typedef unsigned char uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int uint32_t;
typedef signed char int8_t;
typedef short int16_t;
typedef int int32_t;

#endif
