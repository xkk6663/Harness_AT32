# AT32F421G8U7 BLDC 电调工程

无刷直流电机（BLDC）电调，主控 **AT32F421G8U7**（Cortex-M4F，120MHz，64KB+4KB Flash，16KB SRAM）。
电机算法库：`SguanESC/`。图形化配置由 **AT32 Work Bench**（`.ATWP`）生成，构建/烧录由 **CMake + OpenOCD 命令行全家桶**完成。

## 目录结构

```
AT32F421G8U7_WorkBench/
├── CMakeLists.txt                # 工程总菜单（用户源码挂载点）
├── CMakePresets.json             # 一键参数包（Ninja + toolchain + 输出目录）
├── cmake/
│   ├── gcc-arm-none-eabi.cmake   # 交叉编译工具链（芯片身份证 TARGET_FLAGS）
│   └── at32_workbench/           # BSP 子工程（生成代码 + 驱动库）
├── project/                      # ATWP 生成代码（main.c、wk_*.c）+ 配置头
├── libraries/                    # CMSIS + AT32 标准外设库
├── SguanESC/                     # 电机库（BLDC 算法，勿动）
├── Hardware/                     # 硬件层（Timer.c）
├── openocd/
│   ├── interface/cmsis-dap.cfg   # 调试器配置（HID 后端，已验证）
│   └── target/at32f421xx.cfg     # 芯片配置（SWD / Cortex-M4）
├── svd/AT32F421xx_v2.svd         # 外设寄存器描述（调试用）
├── tools/
│   ├── build.ps1                 # 构建入口
│   └── flash.ps1                 # 烧录入口
├── docs/                         # 学习路线 / 踩坑指南 / 经验总结
├── startup_at32f421.s            # 启动文件
└── AT32F421x8_FLASH.ld           # 链接脚本
```

## 快速开始

```powershell
# 构建（等价于插件"编译"）
powershell -ExecutionPolicy Bypass -File tools\build.ps1

# 烧录（等价于插件"烧录"，已校验 + 复位运行）
powershell -ExecutionPolicy Bypass -File tools\flash.ps1
```

前置要求：AT32 插件已安装（提供 `%LOCALAPPDATA%\at32-tools` 下的 cmake/ninja/OpenOCD），
`arm-none-eabi-gcc` 在 PATH（本机 `C:\DevEnv\GNU-tools-for-STM32`）。

## 学习文档（重要）

| 文档 | 内容 |
|---|---|
| `docs/学习路线.md` | 从插件到命令行工具链的 commit 式迁移路线（Step 0~6） |
| `docs/踩坑指南.md` | 真实踩坑记录（编码/调试器接口/TCL 路径/双目录...） |
| `docs/经验总结.md` | 认知升级与收益清单 |

## 状态

- [x] Step 0 基线 + .gitignore
- [x] Step 1 命令行构建（`tools/build.ps1`）
- [x] Step 2 命令行烧录（`tools/flash.ps1`，DAPLink HID 后端）
- [ ] Step 3 GDB 命令行调试
- [ ] Step 4 自建 launch.json / tasks.json
- [ ] Step 5 环境统一（单构建目录 / 单 GCC / clangd 校准）
- [ ] Step 6 coredump + CI
