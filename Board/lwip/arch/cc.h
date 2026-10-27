// cc.h — lwIP compiler/platform glue for Nano R4 (arm-none-eabi-gcc, bare metal)
#ifndef LWIP_ARCH_CC_H
#define LWIP_ARCH_CC_H

// NO_SYS=1 and a single cooperative context (the tea scheduler) plus no lwIP
// calls from interrupts, so there is nothing to protect.
typedef int sys_prot_t;

#define PACK_STRUCT_BEGIN
#define PACK_STRUCT_STRUCT __attribute__((__packed__))
#define PACK_STRUCT_END
#define PACK_STRUCT_FIELD(x) x

// Asserts are compiled out (LWIP_NOASSERT in lwipopts.h) to save flash.

#endif // LWIP_ARCH_CC_H
