#ifndef __FMT_H
#define __FMT_H

#include <stdint.h>

/*
 * 精简 snprintf：只支持工程用到的 %s %c %d %u %ld %lu、%% 和 0/空格宽度。
 * 不依赖 newlib 的浮点/完整 printf，节省 Flash。
 */
int fmt_snprintf(char *buffer, uint32_t size, const char *format, ...);

#endif
