# Ace 5 Wi-Fi 移植状态

核对日期：2026-11-14。本文记录源码和 Android 只读采集所得证据；不表示
XNU 已能初始化无线芯片、扫描或联网。此前 QRTR 边界修复属于现有代码的
健壮性修复，没有补齐 Ace 5 的硬件传输。

## 已确认的设备和传输

Android 报告 PCI vendor/device `17cb:1107`、revision `01`，绑定 `cnss_pci`；
MHI 的 `IPCR` 绑定 `qcom_mhi_qrtr`，还存在 `ADSP_0..3` satellite 通道。
Linux ath12k 将 `0x1107` 定义为 WCN7850；OnePlus 厂商源码称它为 KIWI。
PCI revision 字节不能直接代替芯片的硬件主/次版本，固件选择仍需芯片版本和
板数据匹配。

- [Linux v6.12 ath12k PCI ID 表](https://github.com/torvalds/linux/blob/v6.12/drivers/net/wireless/ath/ath12k/pci.c#L36)
- [WCN7850 MHI IPCR 通道与事件环配置](https://github.com/torvalds/linux/blob/v6.12/drivers/net/wireless/ath/ath12k/mhi.c#L80)
- [OnePlus CNSS PCI/MHI 初始化](https://github.com/OnePlusOSS/android_kernel_modules_and_devicetree_oneplus_sm8650/blob/453d8875f815bf9dfad9035982861226a4823a6c/vendor/qcom/opensource/wlan/platform/cnss2/pci.c#L7056)

实际设备树与公开 SM8650/KIWI 配置相符的关键信息如下。地址是硬件描述，
本轮没有用它们进行寄存器访问。

| 层 | Ace 5 配置 |
| --- | --- |
| 无线电源/控制 | `qcom,cnss-kiwi`，PCIe RC0；WLAN_EN GPIO16、BT_EN GPIO17、SW_CTRL GPIO18、WLAN_SW_CTRL GPIO19；七路供电 |
| PCIe0 | PARF `0x01c00000`、PHY `0x01c06000`、DBI `0x60000000`、iATU `0x60001000`、配置窗口 `0x60100000`；PERST GPIO94、CLKREQ GPIO95、WAKE GPIO96 |
| 中断 | `msi-map` 指向 GICv3 ITS `0x17140000`，设备 ID `0x1400/0x1401`；独立 `qcom,pcie0_msi` 节点禁用 |
| DMA | apps-SMMU `0x15000000`，PCIe0 stream ID `0x1400/0x1401`；设备树声明 `dma-coherent` |
| 启动 | PCIe SM 区域 `0x01d07000`、SM 序列、QMP PHY 配置、RPMh/时钟/复位和互连请求 |
| 内存描述 | `cnss_wlan_region` 为 32 MiB 可复用池、4 MiB 对齐、4 GiB 以下分配；这不等于已为 XNU 建立可用 DMA 映射 |

公开来源：
[pineapple-pcie.dtsi](https://github.com/OnePlusOSS/android_kernel_modules_and_devicetree_oneplus_sm8650/blob/453d8875f815bf9dfad9035982861226a4823a6c/kernel_platform/qcom/proprietary/devicetree/qcom/pineapple-pcie.dtsi)、
[pineapple-kiwi-cnss.dtsi](https://github.com/OnePlusOSS/android_kernel_modules_and_devicetree_oneplus_sm8650/blob/453d8875f815bf9dfad9035982861226a4823a6c/vendor/qcom/proprietary/wlan-devicetree/pineapple-kiwi-cnss.dtsi)。

## 当前 HackiOS 入口的边界

`xnu/files/bsd/dev/redwood_wifi.c:redwood_wifi_init()` 在非 Redwood 平台拒绝
原有无线电启动；这项保护仍需保留。`wifi/lx/src/lx_bus.c` 固定
`ATH11K_HW_WCN6750_HW10` 和 AHB，启动链为 WPSS/SMEM/GLINK。
`tools/mk-ath11k.sh` 也只构建 ath11k，不包含 ath12k 或 MHI host。
把型号常量或平台判断改成 Ace 5，无法产生 PCIe/MHI 支持。

可复用的现有边界是 `redwood_qmi.c` 的 QMI 编解码/事务、
`redwood_qrtr.h` 的客户端传输接口，以及部分线程、内存、网络接口桥接思路。
现有 QRTR 实现仍嵌在 Redwood GLINK 启动路径中；现有 radio DMA/MSI 接口也
依赖其板级实现，不能直接拿来给 Ace 5 endpoint 使用。

## AERA 项目提供的新证据

已阅读用户项目的补丁、init 配置和历史，固定版本为
[`aea42a23`](https://github.com/cheng343/android_device_oneplus_giulia-AERA/commit/aea42a23a7901c5819e712a32bdfaf4db806a99c)。
用户报告该 Recovery 的 Wi-Fi 已可用；源码中确有以下实质修复：

- [0001 补丁](https://github.com/cheng343/android_device_oneplus_giulia-AERA/blob/aea42a23a7901c5819e712a32bdfaf4db806a99c/patches/bootable/recovery/0001-wlan-configure-the-network-wpa_supplicant-actually-c.patch#L115)
  使用 `ADD_NETWORK` 返回的网络 ID，并检查 supplicant 的拒绝答复，修复
  后续连接仍操作 network 0 的问题。
- [0004 补丁](https://github.com/cheng343/android_device_oneplus_giulia-AERA/blob/aea42a23a7901c5819e712a32bdfaf4db806a99c/patches/bootable/recovery/0004-wlan-stage-the-chip-board-data-and-report-why-assoc.patch#L461)
  优先从本机 modem 镜像取 `bdwlan.*`、`regdb.*` 等板数据，复制到 Recovery
  内存中的固件目录，兼容 `kiwi`/`kiwi_v2`；同时增加 BSS 选择、PMF 和关联重试。
- [init.recovery.qcom.rc](https://github.com/cheng343/android_device_oneplus_giulia-AERA/blob/aea42a23a7901c5819e712a32bdfaf4db806a99c/recovery/root/init.recovery.qcom.rc#L111)
  配置内核固件查找目录；[init.recovery.wifi.rc](https://github.com/cheng343/android_device_oneplus_giulia-AERA/blob/aea42a23a7901c5819e712a32bdfaf4db806a99c/recovery/root/init.recovery.wifi.rc#L1)
  通知 CNSS `fs_ready`，等待 `wlan0`，再用 `nl80211` 启动 supplicant。

其底层依赖也很明确：[BoardConfig.mk](https://github.com/cheng343/android_device_oneplus_giulia-AERA/blob/aea42a23a7901c5819e712a32bdfaf4db806a99c/BoardConfig.mk#L178)
从当前设备镜像加载匹配原厂 Linux 内核的 `cnss2.ko`、`cfg80211.ko`、
`qca_cld3_kiwi_v2.ko` 等模块；Recovery 镜像本身排除内核。因此可直接借鉴的
是芯片/板数据来源、固件准备顺序和连接逻辑，不是可以加载到 XNU 的二进制驱动。
厂商 `bdwlan`/分段固件与 ath12k 的固件容器、board ID/QMI 要求仍要核对，
不能仅改文件名就视作兼容；也不能把板版本数据与每台设备的校准数据混为一谈。

已核实 [构建 38891790949](https://github.com/cheng343/android_device_oneplus_giulia-AERA/actions/runs/38891790949)
成功并对应发布包。[5648195](https://github.com/cheng343/android_device_oneplus_giulia-AERA/commit/56481950e9210cd14842aa95f1f7f6bd47de9b2d)
记录补丁应用/类型检查，[eda565d](https://github.com/cheng343/android_device_oneplus_giulia-AERA/commit/eda565d91c1c7f7cdb9529399c0333cc260dec33)
将补丁接入生产构建。仓库中未发现实际联网日志；构建结果与用户报告的实机结果
分别记录，不互相替代。

## 可参考的 XNU ath12k 实现和尚缺的接入

[`UniXNU/Ath12kWifi@5f7f5a8`](https://github.com/UniXNU/Ath12kWifi/tree/5f7f5a8a9f02f7ab45ed10a2f226e348c519a838)
提供 ath12k、MHI、QRTR/QMI 和 mac80211/cfg80211 层。
[`nw_abi.h`](https://github.com/UniXNU/Ath12kWifi/blob/5f7f5a8a9f02f7ab45ed10a2f226e348c519a838/include/nw_abi.h)
要求板代码提供 `lx_platform` 的 DMA、固件、计时、调度回调，以及 `nw_board`
的 BAR、PCI 配置空间和 MSI 目标。完整组件有独立 source manifest 和构建准备，
并非一个可替换 `lx_bus.c` 的源文件。

参考板的
[`WifiProbe.cpp`](https://github.com/UniXNU/UniXNU-sheng/blob/516a780473a2ac59c8b438e132bb60204dda9e22/package/WifiProbe.cpp)
针对 SM8550/sheng，GPIO、电源、时钟、MSI 路由和 DMA 服务不同。
[上游移植说明](https://unixnu.com/porting/network)也要求先完成这些板级服务。
Ace 5 要达到真正的驱动接入，至少仍需：

1. SM8650 PCIe0 电源/时钟/PHY/SM/复位时序、链路训练、配置空间与 BAR 管理。
2. apps-SMMU 的 endpoint DMA 所有权、地址范围、分配/释放及缓存一致性。
3. 自己拥有的 MSI 路由、分配、投递、屏蔽和退出清理。默认 Android 走 ITS/LPI；
   SM8650 也描述了 SNPS 内部 MSI 到 SPI 的路线，见下节。不能从另一块板直接
   复制 IRQ 或沿用 Android 的 MSI 地址。
4. MHI BHI/BHIe 固件启动、控制/事件/传输环和 IPCR→QRTR→QMI 的真实连接。
5. 匹配 KIWI 版本和板数据的 ath12k CE/DP/WMI，加上本项目 iOS 16.5 的
   网络接口、认证和事件 ABI；逐项验证扫描、认证、DHCP、收发及停机。

许可证需按实际导入文件处理。上述 sheng 板文件明确使用 BSD-2-Clause-Patent；
Linux ath12k 为 BSD-3-Clause-Clear，MHI/mac80211 等具有各自的 GPL 声明。
本次读取的 Ath12kWifi 提交没有顶层 LICENSE/COPYING，原创 shim/接口文件也
未普遍带许可声明；不能把另一个仓库的许可证自动套到它们。此次仅将其作为
接口研究参考，没有导入其源码或固件，也没有新增假定底层成功的启动入口。

## 2026-11-14：PCIe MSI 接收层

手机已按用户要求从 iOS 正常退出并回到 Android，ADB 确认
`sys.boot_completed=1`。后续仅只读采集 PCI/MHI sysfs、配置空间及固件文件名。
实际 endpoint 为 `0000:01:00.0`，BAR0 覆盖 `0x60400000..0x605fffff`，
链路为 5.0 GT/s ×1。MSI capability 位于 `0x50`，支持 32 个向量、32 位
消息地址及逐向量 mask。Android 当前配置的地址为 `0x98000040`、data 为 0；
这个地址处于 Android 建立的映射中，不能作为 XNU 的现成门铃地址使用。
原始采集保持本地私有，未导入仓库。

现有 GIC 驱动只管理普通 SPI，还把 `INTID >= 1020` 全部当作无效中断。
因此它不能处理默认 ITS 路线产生的 LPI。实现 ITS 不只是修改这个判断，还需
命令队列、设备/collection/translation 表、redistributor 属性和 pending 表，以及
从 XNU 返回 loader 时仍有效的内存所有权。此次未启用 ITS、复用固件的表或修改
这个中断判断。

独立核对的另一条硬件路线：

- [Linux v6.12 SM8650 PCIe0 描述](https://github.com/torvalds/linux/blob/v6.12/arch/arm64/boot/dts/qcom/sm8650.dtsi#L2092)
  列出了 SNPS 的 SPI 141..148，group 0 对应 **GIC INTID 173**，level-high。
  同一节点仍有 `msi-map`；这些 SPI 的存在不等于 Android 默认使用它们。
- [DesignWare MSI 实现](https://github.com/torvalds/linux/blob/v6.12/drivers/pci/controller/dwc/pcie-designware-host.c)
  使用 DBI `0x820/0x824` 保存目标地址，group 0 的 enable/mask/status 位于
  `0x828/0x82c/0x830`，status 为 write-one-to-clear，data 是向量号。
  SNPS 内部接收器与 ITS TRANSLATER、独立 QCOM MSI doorbell 是不同路径。
- OnePlus `pci-msm-msi.c` 区分内部 SNPS 和外部 QGIC 路径；后者还需要
  `dma_map_resource`。禁用的 `qcom,pcie0_msi` 节点不能当作无需映射的捷径。

第一阶段针对 SNPS group 0 编写独立接收层，而不搬运 Linux `.ko`。板级调用者
必须先拥有 PCIe/DBI、完成链路和外部 MSI 路由选择、保留合适的目标地址，再接入
现有 GIC SPI 服务。只有真实 endpoint 发消息、RC status 和 GIC handler 计数变化，
且停止后不再触发中断，才能算硬件通过。主机 mock 和内核编译不证明这些条件。

本轮新增 `giulia_pcie_msi.c/.h` 处理 32 个 MSI 向量的配置、mask/unmask、
W1C 分发和关闭；`giulia_pcie_msi_xnu.c/.h` 提供实际 MMIO 操作及 GIC SPI
注册，限制在 owner CPU 上串行操作。停止失败保留接收器、IRQ 和映射的所有权，
允许重试，不假装资源已释放。两段源文件加入内核构建；主机回归编译相同的
驱动逻辑，模拟寄存器行为及 XNU 服务。

**当前没有自动启动入口，也没有新增“Wi-Fi 已连接”的界面或标志。**
此接收层需要后续 PCIe 板级驱动调用；现有 `--wifi` 保护仍然生效。PCIe 电源、
PHY、链路训练、SMMU/DMA、MHI 和 ath12k 网络收发尚未接通，不能扫描或联网。
没有在这轮修改中导入厂商固件，也没有在手机上执行新增寄存器写入。

## 2026-11-14：PCIe 电源、PHY 与链路驱动核心

新增三段实际寄存器操作代码，分别链接相同源码执行云端 mock 测试，并加入
完整 XNU 编译。它们通过回调访问寄存器，目前没有板级自动启动调用者。

- `giulia_pcie_power.c`：PCIe0 GDSC、GCC/TCSR 时钟、AUX/RCHNG RCG、
  CORE/PHY reset 和 PIPE 切换；外部无线供电与 NoC 请求不在这段代码中。
- `giulia_pcie_phy.c`：QMP Gen3 x2 / v5 控制布局，Ace 5 公开设备树的
  112 项 PHY 参数、PIPE 开启与 reset/start 顺序、PHYSTATUS 有界等待。
  两条 TX lane 的参数不同于 SM8550 参考板，未整套照搬参考板设置。
  已与留存的三份本机 ColorOS 17 设备树逐项、逐序核对，这 112 项参数
  及另外三项 power/reset/start 控制值均一致；这仍不代替 XNU 实机初始化验证。
- `giulia_pcie_link.c`：PARF/根端口设置、链路训练、配置窗口 iATU、
  `17cb:1107` 端点核对、capability 与 64 位 BAR 尺寸核对、BAR 分配。
  端点内存解码和 Bus Master 均保持关闭，不启动 DMA 或无线固件。

需要由同一板级 owner 串行执行：完成真实供电/NoC/所有权交接并拉低 PERST#，
启动 power，执行 link prepare，再启动 PHY，经规定 PERST# 延时后训练链路。
退出时先停上层/MSI、拉低 PERST#，再让 link 静止、停 PHY、最终恢复 link，
然后停 power。不能在 PHY 清理前恢复可能关闭 PHY 访问的 PARF 配置；恢复失败
保留资源和状态供重试。前置条件标志不能代替真实供电、时钟或 AOP/CESTA 交接。

云端 `test-pcie-bringup.py --cc clang-18` 将生产核心与 mock-MMIO 链接，
检查超时、丢失写入、无效读回和退出恢复；本机仅可用 `--check-only` 检查
输入文件。这些测试不能证明真实 PCIe 链路已建立。

尚待完成板级供电/NoC/GPIO 与实际 MMIO 适配、SMMU/DMA、MSI 路由接合，
以及 MHI、固件、ath12k 和网络接口。**当前仍不能扫描或联网。**
参考的 OnePlus Android 6.1 源码仅作为硬件资料，不作为已验证可启动的内核；
本项目构建 XNU 8796.121.2，未构建或刷入该 Android 内核。

上述构建的 link prepare 要求复位后的 DBI/iATU 窗口已可安全读取；后续修改
已将 PARF decoder 设置移到首次 DBI/iATU 访问之前，见下一节。power 使用的
TCSR `0x01fc0000 + 0x31100` 来自主线驱动，但本机 Android 设备树的同地址
syscon 仅声明 `0x30000` 长度。未来 MMIO 适配必须单独确认并覆盖该寄存器，
不能按这个 syscon 长度映射后越界访问。CESTA 退出、NoC voter 与直接 GCC
时钟投票的交接也尚未实机验证。

源码提交 `a900b14fcd51ea0c9ff71d548bafbe8d32b818bc` 的
[云端构建 43584687111](https://github.com/cheng343/HackiOS-Ace5-lab/actions/runs/43584687111)
已通过 power、PHY、link 和 MSI 的 mock 回归及完整 XNU 构建；下载后核对了
全部 7 项构建产物的长度和 SHA-256。内核 ccache 命中 1045/1049 次可缓存
调用（99.62%）。本轮没有将新代码加载到手机，这不是无线功能的实机通过记录。

## 2026-11-14：板级时序、无线供电与交接诊断

以下是后续源码实现，验证结果与前一构建分开记录；尚未在手机执行无线寄存器
写入，不能据此宣称 Wi-Fi 可扫描或联网。

- `giulia_rpmh.c` 解析本次启动的 CMD DB，执行 DRV2 active TCS 事务、完成检查、
  有界等待及超时恢复。Wi-Fi 与原触摸电源路径共用非递归事务锁；超时保留锁，
  避免另一客户端覆盖尚未结束的事务。其他 RSC 客户端和中断所有权仍需平台证明。
- `giulia_wlan_rails.c` 控制设备树所列七路电源的电压与 enable 投票。默认要求
  已知的原始每客户端投票，才能失败回滚；聚合电压、sysfs 和 TCS 残留都不能
  充当这个基线。显式的保留投票试验模式不会自动启用，也不声称恢复成功。
  电源 mode、AOP/QMP 与共享资源的接管仍是外部前提。
- `giulia_wifi_gpio.c` 实现 WLAN_EN、PERST、CLKREQ、WAKE 的真实 GPIO 配置与
  延时，保留不属于驱动的位；BT/SW_CTRL 只读取。失败保存恢复状态，支持重试。
- `giulia_pcie_link.c` 在首次读取 DBI/iATU 前完成 PARF decoder 设置，分别跟踪
  PARF 和核心窗口所有权。新增独立的 CPU BAR 内存解码开关；仍禁止 Bus Master
  与 MSI。退出拆成 quiesce 和最终恢复，保证 PHY 停止前仍能访问所需资源。
- `giulia_wifi_board.c` 串行调用 GPIO、power、link、PHY，成功上限是 CPU 可访问
  BAR。每阶段重新检查外部资源租约；失败退出遵循 PERST、link quiesce、PHY、
  link restore、power、GPIO 的顺序。停止失败不释放仍在使用的电源/NoC 租约。
- `giulia_pcie_noc.c` 提供有界的 CESTA/CRM 只读快照，不写投票，不根据快照
  自动认定 NoC ready 或 CESTA 已交权。完整运行时快照需要另行证明访问条件。
- `giulia_mhi_snapshot.c` 在 BAR 已可访问后，仅读取 MHI/BHI/BHIE 状态白名单，
  校验完整偏移及 BAR 边界，最多 28 次读取。不读取芯片序列/密钥，不启动 DMA、
  不加载固件；BHIE RX 上电值未定义，不能当作成功传输。

新增 XNU 入口 `ace5_wifi_probe=1|2` 默认关闭，只允许 giulia。模式 1 读取已知
GCC、GPIO、RSC 和 CMD DB；模式 2 仅额外读取 vendor 早期 probe 访问的两个
SM override/mask 与四个 CRM header 寄存器，完整运行时表留待后续。此入口没有
设备寄存器写入，也不会调用上述电源或 PCIe 启动核心。读取 MMIO 本身仍可能因
未验证的启动环境而异常；可读 header 也不证明控制权已经移交。

`phone.py run --wifi-probe 2` 要求显式 `--kernel`、`--bootcpu 1` 和正数
`--seconds`。最终日志必须包含该模式的开始及完成标记，模式 2 还需 header
完成标记；仅成功启动内核不能算诊断完成。短诊断只需小 root 镜像，无需传输
整套 iOS system 镜像，也不以进入桌面为成功标准。

云端统一测试入口仍为 `tools/ace5/test-pcie-bringup.py --cc clang-18`，新增
RPMh/rails、GPIO、board、NoC、MHI 与 XNU probe 用例。board 测试检查实际协调器
的调用顺序，叶子驱动由各自真实源码 mock-MMIO 测试覆盖；这些均不证明硬件时序。
本机只执行 Python/源码检查，C 回归与完整内核构建使用 GitHub Actions。

下一步依赖真实交接状态补齐 AOP/NoC/CESTA 的平台实现，再接 SMMU/DMA、MSI、
MHI 固件启动、ath12k 与网络接口；当前没有用全置位前置条件跳过这些缺口。

提交 `cd98988ebde62c421b64757b8ea8048eca044b64` 已通过
[云端构建 43587684062](https://github.com/cheng343/HackiOS-Ace5-lab/actions/runs/43587684062)：
上述新增模拟回归、loader 与完整 XNU 均成功。下载后核对了全部 7 项产物长度与
SHA-256，内核为 `9d43c3434bac22359fb63a09ebf0eaffbc2f25e823b6bf1a9ed7836a3deed090`。
ccache 命中 1045/1056 次可缓存编译（98.96%），11 次未命中；这不是整棵 make
工作目录的增量复用。仅准备了临时诊断包，尚未取得本次真机交接日志，不能把
编译和模拟回归通过记为 Wi-Fi 实机可用。

## 后续：继承时钟与 AOP/QMP

在 `cd98988` 的短只读真机会话中，七路 VRM 地址成功解析，CESTA/CRM header
读取完成，静态 init 的 35 项检查通过，内核正常返回并重启回 Android。
采集表明 core/PHY GDSC 均报告 down complete，普通 PCIe0 leaf votes 已撤销，
但 RCHNG 的 GCC vote bit 22 仍为 1、分支仍运行。这不是 Wi-Fi 启动成功记录。

`giulia_pcie_power` 增加显式的 RCHNG handoff 路径：调用者须真实取得前一使用者
移交的控制权，同时保留原有供电、NoC、CESTA 和 PERST 前提。核心在写入前再次
检查两个域均 collapsed/down、RCHNG 运行且其他 PCIe leaves 已关。整个启动、
失败恢复和停止过程保留原 bit 22，恢复原 RCG 配置；默认入口继续拒绝继承票。
板级协调器将这份可选租约一直保留到 power restore 完成，不能在 stop 时省略。

`giulia_aop.c/.h` 实现实际的 32-bit AOSS/QMP descriptor/握手/消息传输协议。
冷启动通道和明确取得租约后借用已打开通道是不同策略；消息缓冲区地址和长度
从有效 descriptor 得到，全部做边界校验。消息长度在 payload 写完后才发布，
等待 ACK 有一秒上限。超时保留待确认消息，恢复只观察迟到 ACK，不重发、不清除
长度；关闭借用的通道不写 DOWN。它不恢复 AOP 已消费的电源表，也不将 ACK 等同
于某个 regulator 命令的结果或原表读回。平台仍需提供独占 QMP 服务与 IPCC kick。

协议与 doorbell 依据固定官方
[qcom_aoss.c](https://github.com/OnePlusOSS/android_kernel_oneplus_sm8650/blob/66305c56af1fcd7299c906c3257e585be93d93ed/drivers/soc/qcom/qcom_aoss.c)、
[qcom-ipcc.c](https://github.com/OnePlusOSS/android_kernel_oneplus_sm8650/blob/66305c56af1fcd7299c906c3257e585be93d93ed/drivers/mailbox/qcom-ipcc.c)
和相同提交的 `qcom-ipcc.h`。AOP/QMP client/signal 是 0/0，SEND_ID offset 为
`0xc`，编码为 `(client << 16) | signal`；尚未自动向该寄存器发送。

`ace5_wifi_probe=3` 额外只读采集 AOP header、mailbox length 和前后变化状态，
不读取消息内容、不触发通道握手或发送电源消息。对应 cloud runner 直接链接
生产 AOP 核心，覆盖握手失败、全局 owner、超时/迟到 ACK、借用通道保留、payload
先于 length、布局漂移，以及 XNU 映射回收。新增代码的硬件写入路径尚未真机验证。

提交 `1797b2a1d982229e12683ff21d8d210587dcc160` 的
[云端构建 43590878053](https://github.com/cheng343/HackiOS-Ace5-lab/actions/runs/43590878053)
通过新增回归与完整 XNU 构建，七项产物均已核对长度和 SHA-256；ccache 命中
1052/1057（99.53%）。模式 3 真机只读采集完整结束，静态 init 35 项检查无失败，
内核正常返回，Android 启动完成。

这次实测 QMP magic/version 正确，host mailbox offset=`0x128`、size=`0x2d8`，
与声明的 `0x400` 资源边界吻合，mailbox length 为 0，header 前后没有变化。
AOP link/channel 均 UP；host link/ACK 为 UP，host channel/ACK 为 DOWN。
因此新增第三种策略 `OPEN_ON_INHERITED_LINK`：仍须真实的独占交接租约，只打开
主机 channel，停止或部分失败清理时保留 inherited link。测试采用这次实测的
布局，并检查成功、握手超时、丢写和 doorbell 失败均不写 link control。
此写入策略仍未真机执行，也未初始化 WLAN PDC 表或无线芯片。

第三种策略的提交 `34e5f2f9dcb03fd43348a6dcc767788ad95e20e7` 已通过
[云端构建 43591408182](https://github.com/cheng343/HackiOS-Ace5-lab/actions/runs/43591408182)，
包含实际核心的 inherited-link 成功/故障测试和完整 XNU 构建。七项产物长度与
SHA-256 已核对，ccache 命中 1054/1057（99.72%）。本轮真机实际加载的是前一个
`1797b2a` 的只读 probe 3；这些构建结果不能算第三种写入策略的硬件通过记录。

## 后续：固件交接窗口与 AOP 通道实测入口

`xnuloader` 以 CPU0 观察 CPU1 上的 XNU，原来仍可能调用 UEFI 日志、USB、存储
和充电服务。现在增加显式的 firmware-quiet 请求/确认 ABI：仅指定
`ace5_aop_channel_test=1` 的 giulia 副核运行会发布对应设备树属性。loader 屏蔽
其 CPU 的中断后才确认，在请求解除前仅轮询共享内存并按原预算喂 hypervisor
watchdog，不调用上述 UEFI 服务。XNU 结束时也解除等待；不会因任意短超时而在
仍活跃的借用者下面恢复固件服务。请求、确认分别占独立的 128-byte cache 区域，
避免不同 CPU 的 cache clean 覆盖对方写入。

`giulia_aop_xnu.c` 提供真正的 MMIO/IPCC adapter 和独立通道测试入口，要求精确
giulia、单 XNU CPU、bootcpu1、10..300 秒有限会话，以及其他实验硬件入口关闭。
它验证 loader ABI，取得 quiet ACK 后才打开主机通道，随后关闭该通道并核对
继承 link/local channel 状态；全部成功才打印 handshake complete。失败不冒充
成功；若通道不能关闭，保留映射和 quiet 请求到内核返回。此入口不发送 PDC 表、
不改 rail votes，也不启动无线芯片。

主机回归直接编译这个 XNU adapter 和生产 QMP 核心，模拟真实观测的 descriptor
与 AOP ACK。另有测试提取实际 loader quiet 控制流，检查中断先于 ACK、请求取消、
内核返回和 ACK 清除/中断恢复；CPU 间缓存、真实固件 ACK 仍需真机验证。
使用 `phone.py run --aop-channel-test` 显式选择；普通启动流程默认关闭。

此轮首次构建 `74ba6f7` 的 host tests 与 loader 通过，但完整 XNU 因共享头文件
目录错误而失败；修正至 `bsd/sys` 后，提交
`1b8545d8b152e88f19ac31eabf6cc27d0fdf2a78` 的
[云端构建 43593228782](https://github.com/cheng343/HackiOS-Ace5-lab/actions/runs/43593228782)
全部通过。七项构建产物均已核对长度和 SHA-256，ccache 命中 1056/1058（99.81%）。

该版本随后实际加载到 Ace 5：内核自检成功，日志确认 channel open acknowledged，
随后 channel handshake complete / inherited link preserved / firmware resumed；
静态 init 35 项检查无失败，内核正常返回。测试只执行 QMP 主机通道的打开/关闭，
没有发送 WLAN PDC/rail 消息、没有上电无线芯片，尚未扫描或联网。此结果为一次
有限会话的通道验证，不代表持续电源管理或完整 Wi-Fi 功能通过。
按用户要求，测试后设备转入原厂 Fastboot，供用户测试自行编译的 Recovery。

## 后续：无线 PDC 电源表与七路电源的模式投票

`giulia_wlan_pdc.c` 将已验证的 QMP 传输用于原厂的十条 `wlan_pdc` 消息：
RF S3C 916/612、S1C 1316/944、S6C 1864/1820，BB S2C 976/512、S4I
940/420，数值单位为 mV，分别是 upval/dwnval。来源为固定官方
[`pineapple-kiwi-cnss.dtsi`](https://github.com/OnePlusOSS/android_kernel_modules_and_devicetree_oneplus_sm8650/blob/453d8875f815bf9dfad9035982861226a4823a6c/vendor/qcom/proprietary/wlan-devicetree/pineapple-kiwi-cnss.dtsi)
与同提交 `cnss2/power.c`。留存的本机 ColorOS 17 设备树包含相同十条消息，
属性总长 508 字节。启动要求当前 DT 属性逐字匹配，依次发送 64 字节零填充消息，
第一处失败即停止；迟到 ACK 只观察、不重发。

电源表没有已实现的原值读取接口。默认恢复策略要求调用者提供真实的全部原值，
按相反顺序补偿所有可能已发布的消息；部分补偿超时保留所有权和进度，收到迟到
ACK 后继续剩余项。显式 KEEP_TABLE 为单向修改，不声称关闭通道能恢复原表。
两种策略都要求真实持有 QMP/quiet、无线/蓝牙/控制线和 DMA 的交接条件；没有
从 DT、TX 缓冲区或 ACK 伪造原值，也没有把这段代码接入普通自动启动。

`giulia_wlan_rails_start_with_dt_modes` 是新增可选入口：根据同一份真机 DT，
L15B/L3C 的 `qcom,init-mode=4` 经 PMIC5 LDO mode map 转成 HPM 原始值 7，
S1C 的 `qcom,init-mode=1` 经 PMIC5 FTSMPS map 转成 RET 原始值 3；另外四路
未指定的 mode 不写。编码依据固定官方
[`rpmh-regulator.c`](https://github.com/OnePlusOSS/android_kernel_oneplus_sm8650/blob/66305c56af1fcd7299c906c3257e585be93d93ed/drivers/regulator/rpmh-regulator.c)。
启用顺序为电压、模式、enable，撤销禁用票时为 enable、电压、模式。
旧入口仍只写电压与 enable。mode 写入另需证明对应物理电源已关闭，或真实
owner 已证明目标模式正在生效；本客户端 enable=0 不能证明共享电源物理关闭。
可恢复策略还要求三路旧 mode 票已知，部分失败后恢复全部已尝试的字段。

云端测试直接链接生产 PDC/QMP、RPMh/rails 源码，检查消息内容、填充与顺序、
未知基线拒绝、迟到 ACK、部分发送/补偿失败和模式写入顺序。本机仅检查源码与
Python，不编译 C。新 PDC 与 mode 写入尚未真机执行；完整供电/NoC 交接、PCIe
端点、DMA/MHI/无线固件和网络栈仍需继续接合，当前仍不能扫描或联网。

上述提交 `0d3e07ff655e171cd23fcb122e28597f5760acf2` 已通过
[云端构建 43595839430](https://github.com/cheng343/HackiOS-Ace5-lab/actions/runs/43595839430)：
新增 PDC 与 modes 回归、既有回归、两类 loader 和完整 XNU 构建全部成功。
下载后核对了全部七项产物长度与 SHA-256；内核 SHA-256 为
`46722e2a657aca88001d0635ce1d53a4f809a7b15fe5278af07d9324758430e1`。
ccache 命中 1056/1059（99.72%），三次未命中。本轮未操作手机，未执行供电
写入或无线功能的真机测试。

## 后续：CRM 双通道控制与完整 NoC 只读诊断

`giulia_crm.c/.h` 新增实际的 PCIe CRM HW DRV0 寄存器控制层，协议参考固定
官方 `crm.c` 的 `crm_write_pwr_states`、`crm_switch_channel` 等接口：独占租约
成立后读取两份完整表，仅接受 UPDATE/BUSY 一致的单一已生效通道；克隆其全部
状态和资源到另一通道，对指定字段做互不重叠的 masked 更新，逐字读回后请求
一次切换。原活动表保持不写，IP0 和未管理状态／资源均保留。超时不重复请求，
迟到确认通过 recover 观察；退出先确认切回原表，再恢复原空闲表。失败保留
快照、映射和所有权，不在仍使用的通道下恢复表。未初始化／歧义状态不会猜成
channel 0。该核心只做表传输，不产生 NoC-ready 或 CESTA 已交权的结论，也没有
普通自动启动调用者。BCM 参数、当前 CMD DB、物理资源完成状态仍待平台接合。

原厂 CRM 接口可在 CESTA 状态机保持运行时更新通道表；这不等于已验证可以将
现有 direct-GCC 电源核心与 CESTA 混用，也不替代对其时序与所有权的后续验证。

新增显式 `phone.py run --wifi-probe 4`：在模式 3 的只读内容上尝试 vendor
诊断使用的 SM/CRM 寄存器白名单，记录两个通道、全部声明的 PERF/BW 状态表、
状态索引以及前后变化。没有 write 回调，不写表、channel、CLKREQ 指令、rail、
GPIO 或 PCIe；不读取 DBI/PHY/BAR。相同 DT 资源及 early header 已验证，但更广
诊断读取仍属于首次测试路径，MMIO 总线异常不能由普通轮询捕获。只有 full
snapshot 和最终 probe complete 均出现，工具才报告诊断完成；完成不意味着
稳定原子快照或真实资源租约。本模式仍要求 CPU1、显式内核和有限会话。

云端测试直接链接 CRM 生产核心，覆盖两个原始通道、最大布局、保留字段、全部
表写入位置的丢写、切换／回退超时、迟到确认、租约丢失和部分恢复。XNU probe
测试链接真实 NoC 采集核心，核对完整采集、读失败与 MMIO 内容不变。

提交 `2ddaf4124736132a457fb494945bc38cdae3ca82` 已通过
[云端构建 43597076888](https://github.com/cheng343/HackiOS-Ace5-lab/actions/runs/43597076888)：
实际 CRM 核心回归、probe 4 回归、loader 与完整 XNU 均成功，下载后全部七项
产物长度与 SHA-256 已核对。ccache 命中 1057/1060（99.72%）。已离线准备只读
诊断包；用户暂时拔下手机并选择先继续源码，因此尚未执行新诊断或 CRM 写入。

## 后续：从 CMD DB 到 BCM 带宽投票，再到 CRM

`giulia_bcm.c/.h` 将当前 CMD DB 的 BCM 地址及 8-byte 辅助数据（unit、width、
VCD）接入带宽转换和真实 CRM 传输。解析只接受 BCM slave5、匹配的当前 DB
版本、唯一资源名、完整有界的 entry/aux 数据，不使用其他机型的地址／比例。
算法依照同一固定官方提交的
[`icc-rpmh.c`](https://github.com/OnePlusOSS/android_kernel_oneplus_sm8650/blob/66305c56af1fcd7299c906c3257e585be93d93ed/drivers/interconnect/qcom/icc-rpmh.c)、
[`bcm-voter.c`](https://github.com/OnePlusOSS/android_kernel_oneplus_sm8650/blob/66305c56af1fcd7299c906c3257e585be93d93ed/drivers/interconnect/qcom/bcm-voter.c)
与 `tcs.h`：零 unit/width 按 vendor 规则归一到 1，分两级整数除法并保留极小
非零票，scale=1000，溢出拒绝、14-bit 饱和显式报告。SH1/ACV 使用 enable
编码，不套用数值带宽比例；CRM 命令带 valid/commit。

Gen1x1 PCIe0 数据路径按官方 `pineapple.c` 生成 12 个字段更新：PERF resource0
在 states0/1 设为 2，MC0/SH0/SH1/SN4/ACV 分别更新 CRM nodes0/1/2/3/5 的
states0/1。IP0/node4、其他资源及 states2..4 均保留。公开拓扑中各 node 的
buswidth/channels 与 BCM enable 编码已分别核对，不能把 VCD 当作 CRM node。

特别核对了留存真机 DT：PCIe0 没有 `qcom,no-client-based-bw-voting`，所以该
vendor 分支使用 avg=500、peak=800 kB/s。只有 DT 指定按速率投票时，Gen1x1
才使用 avg=250000、peak=0。两种策略由调用者显式选择，不把 mainline 的 peak
参数混入此路径。该实现仅覆盖 PCIe 数据路径投票；CPU→PCIe 的 CN0/QoS、CESTA
当前状态与物理完成仍需接合，不会因此设置整个 NoC ready。

`giulia_bcm_pcie0_start` 连通解析、投票转换和生产 CRM 通道控制。云端集成测试
使用合成 CMD DB 和 mock MMIO，检查其实际写入、IP0／其他状态保留及退出恢复；
另测辅助数据损坏、边界、两级除法、enable 编码、溢出与饱和。本机仍不编译 C。
probe 4 还会只读解析 MC0/SH0/SH1/SN4/ACV/CN0 的地址与辅助参数，逐项记录查询
结果，不因为某项缺失而伪造参数，也不在此诊断中发送 BCM 票。

提交 `ccec71db8d66464ee6c92abc52c5edbe88a06c49` 已通过
[云端构建 43597942720](https://github.com/cheng343/HackiOS-Ace5-lab/actions/runs/43597942720)：
BCM→CRM 集成、CRM 时序／恢复、NoC 只读采集及既有回归全部通过，两类 loader
和完整 XNU 构建成功。全部七项产物长度与 SHA-256 已下载核对，内核为
`a112c9f37706f459bf226cc72c9f03588d047dc298d8da43cce2cceaf42d6801`。
ccache 命中 1058/1061（99.72%），三次未命中。新版 90 秒小镜像诊断包已离线
准备，未启动；用户选择稍后接手机，继续源码。BCM/CRM 写入仍未真机执行，
这些结果不代表 Wi-Fi 已能扫描或联网。

## 2026-11-16：完整只读真机采集与空通道首次初始化

使用 `ccec71d` 云端内核在 Ace 5 实际运行了 probe 4。219 次白名单读取完成，
前后 guard 未报告变化；CRM 版本 1.0、2 个通道、5 个状态、2 个 PERF 和 16 个
BW 资源。UPDATE/BUSY 前后均为 0，20 项 PERF 与 160 项 BW 表值全部为 0。
SM START/PC 原始读数均为 0，power-index 原始值为 4；这些读数只记录观测，
不能单独证明 CESTA 已停止、允许接管，或前一系统的物理票已撤销。

六项 BCM 参数均成功解析；例如 MC0 unit=4700000/width=16，SH0/SH1
unit=4800000/width=64，SN4 unit=530000/width=16，ACV unit=1/width=1，
CN0 unit=140000/width=4。原始日志与设备树保留在私有工作目录，没有公开。
静态 init 35 项检查无失败，内核正常返回并确认 Android 启动完成。本轮没有
发送电源、BCM 或 CRM 通道请求，也没有启动无线芯片。用户随后拔下手机，
要求只继续源码；本段之后的新增初始化代码未在手机执行。

观测的无活动通道状态不能走原有“保留旧活动表、切换、切回”策略，因此新增
显式 `giulia_crm_start_empty_keep` 与 `giulia_bcm_pcie0_start_empty_keep`：
在真实独占租约下重新核对 UPDATE=BUSY=0、两个完整声明表均为空，准备通道 0
并只请求一次。这与固定官方 `crm_get_channel` 的首次选通道规则一致；旧入口
继续拒绝空状态，不自动降级到新策略。

首次请求之前若准备失败，可在硬件仍未选通道时恢复空表。请求一旦可能发出，
就没有已验证的“切回无通道”命令：不能把 UPDATE=0 当作回滚。新增入口因此
采用显式保留策略：确认完成后 stop 返回 RETAINED，保留通道、表、owner 和
平台租约；超时或状态不明则保留故障状态与映射。调用者必须安排独立的受控
复位／重新初始化，才可结束该资源生命周期。空表与 START=0 不代替该平台
条件，也不产生整个 NoC-ready 结论。此 API 没有普通开机调用者。

云端测试按实测几何覆盖每一项非空拒绝、全部 90 处准备写失败后的恢复、发布
前租约丢失、单次请求、迟到 ACK、发布后状态漂移与保留生命周期。保留策略的
场景在独立进程运行，不在生产代码加测试清锁接口。BCM 集成用实测 unit/width
配对、合成 DB 验证首次通道实际 MMIO 写入；地址与 DB 内容不是私人固件转储。

上述提交 `b3ce329b8cd5915e615bdf606cf5c1043e4c8b42` 已通过
[云端构建 43735660729](https://github.com/cheng343/HackiOS-Ace5-lab/actions/runs/43735660729)：
全部新增与既有回归、两类 loader 和完整 XNU 构建成功。全部七项产物长度及
SHA-256 已下载核对，内核为
`8bd117c0e8b165db6217dd412c9e8642a7b0e629ffc5ae0866d99a0b90a9eae0`。
ccache 命中 1057/1061（99.62%），四次未命中。这次云端构建与前述 probe 4
真机采集使用不同提交；首次初始化入口尚未真机执行，仍不能扫描或联网。

## 后续：独立 CRM 通道测试入口

新增 `giulia_crm_xnu.c` 与显式 `--crm-channel-test`，将生产首次初始化核心接到
XNU MMIO。此测试仅将全零表选到 channel0：不启用 PERF 或 BCM 投票，不写
rail、GPIO、PHY、PCIe 链路或状态机程序。MMIO adapter 只允许本次声明的
channel0 表地址写 0，以及 CHN_UPDATE 写 1；禁止写其他通道、header、状态字、
非零表值或 UPDATE=0。实际测试默认关闭，要求 giulia、CPU1、单 XNU CPU、
10..120 秒有限会话，且其他实验硬件客户端均关闭。

首次写入前取得 loader firmware-quiet ACK，暂停固件 CPU 的 UEFI/USB/存储等
服务并屏蔽中断；每次事务与写入还要求 PCIe0 core/PHY GDSC 保持 collapsed /
down-complete，功能 leaf 票撤销。未接管继承 RCHNG、PLL 或其他 CRM DRV。
这建立的是本次 APPS 侧独占、无 PCIe0 活动用户的零表通道测试条件，不声称
CESTA 已停或允许 direct-GCC 接管。控制器仍须通过真实 ACK 才报告通道完成。

请求后保留通道、映射与 quiet 到 XNU 返回；平台资源生命周期由 loader 接续
保留，必须冷重启结束。loader 设置 `crm-reset-required=yes`，此后只允许查询、
取回日志／屏幕、停止请求及冷重启，禁止新内核、payload 覆盖、ramdisk 等操作。
因此没有在一次试验后把未知硬件状态交给下一次启动。quiet 在内核返回前不
服务 USB，host 相应等待有限会话结束后才取结果，不进行 live-log 轮询。

云端测试直接链接真实 XNU adapter 与 CRM 核心，检查 opt-in、CPU/其他客户端
隔离、cold-domain 条件、映射、空表、写入白名单、一次 channel ACK 和超时保留。
另从真实 loader 提取精确参数解析和保留状态命令限制，防止伪前缀／参数值误开。
此入口是否被硬件确认，必须记录随后真实会话结果；编译成功不能代替它。

提交 `fcbd93719de4d5de16b5f113544dfdf6a932c8d5` 的
[云端构建 43786597908](https://github.com/cheng343/HackiOS-Ace5-lab/actions/runs/43786597908)
已通过全部新增／既有回归、两类 loader 和完整 XNU 构建，七项产物长度与
SHA-256 均已下载校验。ccache 命中 1058/1062（99.62%）。随后在本机 Ace 5
实际运行 60 秒通道测试：rc=0，BUSY=UPDATE=1，全零表 channel0 请求得到硬件
确认；power-index 仍只记录原始值 4。35 项静态 init 检查无失败，内核正常返回，
host 核对 `crm-reset-required=yes` 后冷重启，Android 启动完成。只验证全零
表首次通道确认，没有启用带宽／性能票或无线初始化，不代表 Wi-Fi 已能联网。
用户在返回 Android 后再次拔下手机；后续带宽入口只做源码／云端验证。

## 后续：实际带宽投票的独立测试入口

新增 `--crm-bandwidth-test`（wire 参数 `ace5_crm_channel_test=2` 与显式
`ace5_crm_fixed_bw=1`）。该入口在同一 quiet／无活动 PCIe0 用户条件下，从
本次启动的 CMD DB 生成原厂 fixed avg500/peak800 方案，然后将 12 个 PERF/BW
字段提交到首次 channel0。私有准备工具逐次核对提供的 DT：只允许 PCIe0 的
`qcom,no-client-based-bw-voting` 缺省策略，不能将这一带宽政策自动套到不同 DT。
运行时缺少显式政策、DB 损坏或票值饱和均不发送通道请求。

XNU adapter 仅允许写 0 或该字段计算出的目标值；请求前再逐项核对整份目标表，
防止未完成的／全零方案冒充带宽测试成功。发布后禁止表写入与重复通道请求，
保留无关节点、states2..4 和 channel1；继续使用相同冷重启生命周期。此阶段只
确认方案被通道接受，不将 ACK 当作实际 NoC 频率、电源状态、CPU→PCIe QoS 或
整个总线 ready 的证明。它不启动 GPIO/PHY/链路、无线固件或 DMA。

云端测试以实测 unit/width 和合成 CMD DB 链接真实 BCM→CRM→XNU adapter，检查
目标字段、IP0 等保留项、严格写入白名单、政策／元数据拒绝、ACK 和超时保留。
该非零带宽方案尚未真机执行，不能把前一个全零通道测试的成功沿用于此阶段。

提交 `deedb796ccfffed1541b2de35440244774e13a08` 已通过
[云端构建 43788667774](https://github.com/cheng343/HackiOS-Ace5-lab/actions/runs/43788667774)：
带宽方案完整写入／超时保留、已有零表路径及其他回归成功，两类 loader 和
完整 XNU 编译通过。七项产物长度与 SHA-256 均已下载核对；ccache 命中
1060/1062（99.81%），两次未命中。带宽测试小镜像已用留存私有 DT 离线准备，
仍要求接机后重新采集当前 DT 并复核政策，尚未启动。用户拔机后没有再次进行
设备操作；这轮源码结果没有新增无线功能的真机通过结论。
