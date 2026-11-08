#ifndef __BOOTGUARD_H
#define __BOOTGUARD_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

void bootguard_init(void);
void bootguard_task(uint32_t now_ms);
uint8_t bootguard_is_active(void);
void bootguard_abort(void);

#ifdef __cplusplus
}
#endif

#endif
