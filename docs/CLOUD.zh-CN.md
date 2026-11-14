# GitHub ARM64 云编译

本研究仓库基于 [YiKongk/HackiOS-Ace5](https://github.com/YiKongk/HackiOS-Ace5)，保留原署名与许可证，加入源码修复和云端构建。此流程编译 XNU 内核及 diagnostic/kernel 两种 loader，未包含完整 iOS、Mu UEFI、手机 DTB 或厂商固件。

在 Actions 中选择 **Ace 5 ARM64 build → Run workflow**，或向 main 推送提交。使用标准 `ubuntu-24.04-arm`，默认两个编译任务并行。每次在安装依赖前后、构建后记录实际磁盘总量/可用量和内存，容量以本次日志为准。

依赖按现有教程的固定版本取得。Theos SDK 只检出 iPhoneOS16.5.sdk，SDK、依赖与缓存不提交到 Git，也不作为构建附件上传。先执行主机回归，再编译 loader、准备 XNU 依赖并执行 `build` 目标。每次都从干净的 runner 构建，失败时保留构建诊断。

## 编译缓存

XNU 的 C/C++ 编译器通过实际可执行的 wrapper 接入 `ccache`，GitHub Actions
在各次运行间保存编译结果缓存，大小上限为 4 GiB。兼容键区分 runner 系统/架构、
编译器内容、SDK/工具链与相关构建配置；每次保存使用独立键，后续运行可恢复
同提交或同兼容环境的缓存。缺失或被淘汰的缓存只会导致重新编译。

首次运行填充缓存，后续复用输入匹配的编译结果。每轮仍重新生成构建目录，检查
依赖和头文件并执行最终链接；没有恢复旧 `BUILD`、SDK 或整份 XNU 源码。
测试程序、loader 和不经过 XNU 编译器 wrapper 的主机工具不在本轮缓存范围内。
因此这是编译结果缓存，不是跳过整个构建，也不能保证所有步骤都变快。

恢复缓存后清零统计，构建产物中保留配置、恢复信息及 `ccache` 统计。确认是否
生效，应看本轮 direct/preprocessed 编译命中数和完整构建结果，不能仅凭
Actions 的缓存恢复提示。缓存校验保持默认严格行为，未启用 `sloppiness`。

## 产物和边界

Actions 附件 `ace5-arm64-运行编号-尝试编号` 包含已成功生成的内核/loader、SHA-256 清单、资源记录与构建日志，保存七天。失败任务也可能有附件；确认整个任务成功，且 `build-manifest.json` 中 `kernel_built` 为 true，再把它作为内核构建结果。

`loader.efi` 是 UEFI 应用，`usb-payload.bin` 是 loader payload，均不是可刷写的 Android boot image。后续仍需按 [UEFI 教程](UEFI.zh-CN.md)使用本机 DTB 准备 UEFI 启动镜像，并准备系统和 GPU 固件。云编译成功不代表这台 ColorOS 17 已实机兼容，也不代表 Wi-Fi、SIM、相机完成适配。

本机仅用于编辑/上传源码与下载云端产物；此流程不会连接手机或运行刷写命令。

另一个 **Ace 5 UEFI components** 流程在云端编译固定 Mu UEFI、BootShim 与 loader，产物不含本机设备树。在电脑上运行 `tools/ace5/assemble-boot.py`，将这些已编译组件与只保存在本机的 giulia DTB 组装成临时启动镜像；组装过程只压缩/打包字节，不调用编译器、不连接设备。

**Ace 5 runtime components** 可在没有 iOS IPSW 的情况下单独编译项目的 init/stage0、注入库、Mesa、EAGL 和 Metal 组件。它只下载构建工具、SDK 与开源依赖，不下载 iOS 系统固件，不读取手机或打包厂商固件。附件中的 `components.json` 标明完整性与 SHA-256；这些组件仍需结合匹配的 iOS 系统文件、真实 GPU 固件及系统镜像后才能启动桌面。
