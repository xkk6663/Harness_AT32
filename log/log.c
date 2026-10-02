/**
 * log.c — 轻量串口日志实现(Step 7: 串口闭环)
 *
 * 输出格式: [等级][tick] 消息
 *   D=DEBUG I=INFO W=WARN E=ERROR; tick 为 ms 时间戳
 * 依赖: printf -> __io_putchar -> USART1(在 at32f421_int.c 钩子区实现)
 */
#include "log.h"

#include <stdarg.h>
#include <stdio.h>

static const char *const g_level_name[] = { "D", "I", "W", "E" };

void log_init(void)
{
    /* 无硬件初始化: printf 重定向在生成代码钩子区已就绪 */
}

void log_write(int level, const char *fmt, ...)
{
    char buf[128];
    va_list ap;

    if (level < LOG_LEVEL_DEBUG || level > LOG_LEVEL_ERROR) {
        level = LOG_LEVEL_ERROR;
    }

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    printf("[%s][%lu] %s\r\n",
           g_level_name[level], (unsigned long)wk_timebase_get(), buf);
}
