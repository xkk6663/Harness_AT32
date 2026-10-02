# cortex-debug 使用教程（Step 4 配套）

> cortex-debug 是一个开源 VS Code 调试扩展：把「OpenOCD（GDB server）+ arm-none-eabi-gdb（GDB client）」封装成 F5 一键体验。
> 你现在按 F5 的每一环，都是我们在 Step 3 亲手敲过的命令——本教程讲它的日常操作。

## 一、调试侧边栏（F5 后出现）

| 面板 | 内容 | 对应 GDB 命令 |
|---|---|---|
| 变量（VARIABLES） | 局部变量/静态变量，可展开看结构体 | `info locals` |
| 监视（WATCH） | 手动添加表达式，每步刷新 | `print 表达式` |
| 调用堆栈（CALL STACK） | 函数调用链，点哪层看哪层 | `bt` |
| 断点（BREAKPOINTS） | 管理所有断点/条件 | `info breakpoints` |
| 外设（PERIPHERALS） | **SVD 寄存器实时视图**（本工程已挂 AT32F421xx_v2.svd） | `monitor xxx` |

## 二、快捷键（肌肉记忆）

| 按键 | 作用 | 说明 |
|---|---|---|
| `F5` | 启动调试 / 继续运行 | 第一次会先跑 preLaunchTask（build） |
| `F10` | 单步跳过 | 不进入函数内部 |
| `F11` | 单步进入 | 进入函数内部 |
| `Shift+F11` | 跳出 | 执行完当前函数返回 |
| `Shift+F5` | 停止调试 | 断开 GDB（OpenOCD 也退出） |
| `Ctrl+Shift+F5` | 重启调试 | 重新加载固件再跑 |
| `Ctrl+F5` | 运行不调试 | 直接跑不设断点 |

调试按钮栏（顶部浮动条）从左到右：继续/暂停 → 单步跳过 → 单步进入 → 跳出 → 重启 → 停止。

## 三、断点进阶（GDB 的强项）

1. **行断点**：点击代码行号左侧（红点）。Cortex-M4 有 **6 个硬件断点**，代码在 Flash（只读）里时自动用硬件断点；
2. **条件断点**：右键断点 → "编辑断点" → 输入条件，如 `i >= 100`（等价 GDB `break xxx if cond`，Keil 做不到表达式条件）；
3. **函数断点**：右键 → "添加函数断点"，输入 `SguanESC_Init`；
4. **日志点**：右键 → "添加日志点"，输入 `{变量}` 会在调试控制台打印值、**不停程序**（等价 `dprintf`）。

## 四、看变量 / 看内存

- **hover 变量**：鼠标悬停源码变量即看当前值（配合 -O0 构建变量齐全）；
- **WATCH 面板**：`+` 添加表达式，如 `motor.speed`、`*(uint32_t*)0x40010000`；
- **liveWatch**（本工程 launch.json 已开）：`"expressions": []` 里填表达式，按 100ms 周期自动刷新——适合看电机转速这类变化量；
- **内存视图**：已装 `mcu-debug.memory-view` 扩展 → 命令面板搜 "Memory View" → 填地址看 hex dump（等价 GDB `x/16xw 0x20000000`）；
- **外设寄存器**：PERIPHERALS 面板点开外设（如 TMR1），寄存器实时值 + 位字段解析——SVD 文件就是干这个的。

## 五、本工程 launch.json 逐字段（明牌版）

```jsonc
{
  "name": "AT32 Debug (OpenOCD)",            // 调试配置名（下拉选择）
  "type": "cortex-debug",                     // 调试器类型（固定）
  "request": "launch",                        // launch=自己起server; attach=连已跑的
  "servertype": "openocd",                    // 用 OpenOCD 做 GDB server
  "executable": "${workspaceFolder}/build/Debug/AT32F421G8U7_WorkBench.elf",
  "serverpath": "C:/.../openocd.exe",         // ★ server 可执行文件（cortex-debug 认这个字段）
  "gdbPath": "C:/.../arm-none-eabi-gdb.exe",  // GDB 客户端
  "configFiles": ["openocd/interface/cmsis-dap.cfg", "openocd/target/at32f421xx.cfg"],
  "svdFile": "${workspaceFolder}/svd/AT32F421xx_v2.svd",  // 外设寄存器描述
  "runToEntryPoint": "main",                  // 启动后自动跑到 main 停住
  "preLaunchTask": "build",                   // F5 前先构建（tasks.json 的 build 任务）
  "liveWatch": { "enabled": true, "interval": 100, "expressions": [] },
  "showDevDebugOutput": "none"                // 调 "raw" 可看 GDB 原始协议（排障用）
}
```

## 六、排障三板斧

1. **看 DEBUG CONSOLE**：`Launching gdb-server: <实际命令>` 告诉你它到底跑什么（Step 4 就靠这行定位了 serverpath 字段名问题）；
2. **看 TERMINAL 的 "gdb-server" 终端**：OpenOCD 原始输出（芯片识别/烧录日志都在那）；
3. **`"showDevDebugOutput": "raw"`**：把 GDB 协议原始收发打出来，官方排障姿势。

## 七、和插件调试的区别

| | 插件调试 | cortex-debug |
|---|---|---|
| 背后 | 插件内嵌的调试支持 | 开源扩展 + 我们自己配置的 OpenOCD/GDB |
| 配置 | `${command:at32.get.*}` 黑盒 | 明牌路径，全在 launch.json |
| 升级 | 插件更新可能改行为 | 锁版本，行为可控 |
| 跨芯片 | 只能 AT32 | STM32/AT32/ESP32... 同一套配置逻辑 |
