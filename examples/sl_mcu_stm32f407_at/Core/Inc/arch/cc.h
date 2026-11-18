/*
 * arch/cc.h - lwIP compiler/platform glue for STM32 + newlib.
 */

#ifndef ARCH_CC_H_
#define ARCH_CC_H_

#include <stdio.h>
#include <stdint.h>

#ifndef BYTE_ORDER
#define BYTE_ORDER LITTLE_ENDIAN
#endif

#define LWIP_PLATFORM_DIAG(x)   do { printf x; } while (0)
#define LWIP_PLATFORM_ASSERT(x) do { printf("lwip assert \"%s\" at %s:%d\n", x, __FILE__, __LINE__); while (1) { } } while (0)

uint32_t lwip_rand(void);
#define LWIP_RAND()             lwip_rand()

#endif /* ARCH_CC_H_ */
