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
3. GIC ITS/LPI 的 MSI 分配、投递、屏蔽和退出清理，不能套用 sheng 的 SPI141。
4. MHI BHI/BHIe 固件启动、控制/事件/传输环和 IPCR→QRTR→QMI 的真实连接。
5. 匹配 KIWI 版本和板数据的 ath12k CE/DP/WMI，加上本项目 iOS 16.5 的
   网络接口、认证和事件 ABI；逐项验证扫描、认证、DHCP、收发及停机。

许可证需按实际导入文件处理。上述 sheng 板文件明确使用 BSD-2-Clause-Patent；
Linux ath12k 为 BSD-3-Clause-Clear，MHI/mac80211 等具有各自的 GPL 声明。
本次读取的 Ath12kWifi 提交没有顶层 LICENSE/COPYING，原创 shim/接口文件也
未普遍带许可声明；不能把另一个仓库的许可证自动套到它们。此次仅将其作为
接口研究参考，没有导入其源码或固件，也没有新增假定底层成功的启动入口。
