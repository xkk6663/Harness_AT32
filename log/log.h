/**
 * log.h — 轻量串口日志模块（工具链层，Step 7: 串口闭环）
 *
 * 设计要点:
 *  - 不依赖具体硬件: 输出走 printf(已由 __io_putchar 重定向到 USART1)
 *  - 时间戳来自 wk_timebase_get()(1ms tick), 弱耦合仅声明不 include
 *  - 等级裁剪: 编译期 -DLOG_LEVEL=LOG_LEVEL_DEBUG 可全开, 默认 INFO
 *  - LOG_EVERY_MS: 高频中断里周期打一条, 防刷屏(高速环 62.5us 周期,
 *    每条都打会阻塞串口, 周期 500ms 足够观察)
 */
#ifndef LOG_H
#define LOG_H

#include <stdint.h>

/* ---- 日志等级 ---- */
#define LOG_LEVEL_DEBUG  0
#define LOG_LEVEL_INFO   1
#define LOG_LEVEL_WARN   2
#define LOG_LEVEL_ERROR  3

#ifndef LOG_LEVEL
#define LOG_LEVEL LOG_LEVEL_INFO
#endif

/* wk_system.h 的毫秒 tick(仅声明, 避免 log.h 依赖生成代码头) */
uint32_t wk_timebase_get(void);

void log_init(void);
void log_write(int level, const char *fmt, ...);

/* ---- 等级宏(编译期裁剪) ---- */
#define LOG_DEBUG(...)  do { if (LOG_LEVEL <= LOG_LEVEL_DEBUG)  log_write(LOG_LEVEL_DEBUG,  __VA_ARGS__); } while (0)
#define LOG_INFO(...)   do { if (LOG_LEVEL <= LOG_LEVEL_INFO)   log_write(LOG_LEVEL_INFO,   __VA_ARGS__); } while (0)
#define LOG_WARN(...)   do { if (LOG_LEVEL <= LOG_LEVEL_WARN)   log_write(LOG_LEVEL_WARN,   __VA_ARGS__); } while (0)
#define LOG_ERROR(...)  do { if (LOG_LEVEL <= LOG_LEVEL_ERROR)  log_write(LOG_LEVEL_ERROR,  __VA_ARGS__); } while (0)

/* ---- 周期日志: 高频环防刷屏 ----
 * 用法: LOG_EVERY_MS(500, LOG_LEVEL_INFO, "tick=%lu", (unsigned long)wk_timebase_get());
 * static 变量跨调用保持, 线程安全要求不高(仅在中断/主循环单点使用) */
#define LOG_EVERY_MS(period_ms, level, ...)                          \
    do { static uint32_t _log_last = 0;                              \
         uint32_t _log_now = wk_timebase_get();                      \
         if ((_log_now - _log_last) >= (period_ms)) {                \
             _log_last = _log_now;                                   \
             log_write((level), __VA_ARGS__);                        \
         } } while (0)

#endif /* LOG_H */
