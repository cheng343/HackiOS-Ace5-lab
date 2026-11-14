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
退出时先停上层/MSI、拉低 PERST#，再依次停 link、PHY、power；恢复失败保留
资源和状态供重试。前置条件标志不能代替真实供电、时钟或 AOP/CESTA 交接。

云端 `test-pcie-bringup.py --cc clang-18` 将生产核心与 mock-MMIO 链接，
检查超时、丢失写入、无效读回和退出恢复；本机仅可用 `--check-only` 检查
输入文件。这些测试不能证明真实 PCIe 链路已建立。

尚待完成板级供电/NoC/GPIO 与实际 MMIO 适配、SMMU/DMA、MSI 路由接合，
以及 MHI、固件、ath12k 和网络接口。**当前仍不能扫描或联网。**
参考的 OnePlus Android 6.1 源码仅作为硬件资料，不作为已验证可启动的内核；
本项目构建 XNU 8796.121.2，未构建或刷入该 Android 内核。
