# Linux Driver Development 101

这份笔记记录 `userspace_drivers/` 例子背后的知识点。它不是项目构建说明，而是学习 Linux PCI / userspace driver / MMIO 时的备忘录。

当前学习路线：

```text
01_pci_enum.cpp       发现 PCI 设备，读取 sysfs 中的设备信息
02_config_space.cpp   读取 PCI config space，理解标准 PCI header 和 capability
03_mmio.cpp           mmap memory BAR，读取设备寄存器
04_bind_unbind.cpp    手动控制 PCI device 的 driver bind/unbind
05_sriov_vfs.cpp      创建/销毁 SR-IOV VF，控制 VF 是否自动绑定 driver
06_vfio_info.cpp      通过 VFIO API 查看 vfio-pci 设备的 regions/IRQs
07_vfio_region_dump.cpp  通过 VFIO mmap region 并读取 BAR 内容
08_vfio_dma_map.cpp   把 userspace buffer 映射到 VFIO IOMMU IOVA
09_vfio_irq_eventfd.cpp  把 VFIO interrupt 连接到 eventfd 并 poll 等待
10_virtio_vfio_pci_caps.cpp  通过 VFIO CONFIG region 解析 virtio PCI capabilities
11_virtio_vfio_common_cfg.cpp  mmap virtio COMMON_CFG 并读取基础状态
12_virtio_vfio_queue_info.cpp  写 queue_select 并枚举 virtqueue 状态
13_virtio_vfio_reset_status.cpp  练习 virtio device_status reset/ACK/DRIVER 状态机
14_virtio_vfio_feature_bits.cpp  读取 feature bits 并练习 FEATURES_OK 协商
15_virtio_vfio_vring_map.cpp  计算 split vring 布局并用 VFIO 映射 DMA 内存
16_virtio_vfio_queue_program.cpp  写 queue_size/desc/avail/used 但不 enable queue
17_virtio_vfio_queue_enable.cpp  写 queue_enable=1 但不 notify / DRIVER_OK
18_virtio_vfio_notify_info.cpp  解析 NOTIFY_CFG 并计算 queue notify MMIO offset
19_virtio_vfio_notify_write.cpp  写一次 queue notify 但不 DRIVER_OK
20_virtio_vfio_driver_ok.cpp  写 DRIVER_OK，启动空 queue 后 reset 清理
21_virtio_net_rx_buffers.cpp  配置 RX/TX queue pair，发布 RX buffer 并观察 used ring
22_virtio_net_rx_observe.cpp  等待 RX used ring，dump 收到的 RX buffer 前缀
23_virtio_net_tx_packet.cpp  发布 TX descriptor，发送一帧并观察 TX completion
24_virtio_net_rx_tx_echo.cpp  收到一帧后构造 reply，并通过 TX queue 回发
25_virtio_net_echo_loop.cpp  多包 echo loop，回收 RX/TX descriptor
26_virtio_net_irq_echo_loop.cpp  使用 VFIO IRQ eventfd 驱动 echo loop
27_virtio_net_ctrl_promisc.cpp  通过 control virtqueue 发送 RX_PROMISC 命令
28_virtio_net_promisc_observe.cpp  设置 promisc 后保持运行并观察非本机 MAC 的 RX
29_virtio_net_ctrl_mac_addr.cpp  通过 control virtqueue 临时设置 MAC 并观察 RX
30_virtio_net_ctrl_mac_table.cpp  通过 control virtqueue 设置额外 unicast MAC filter
```

当前 `userspace_drivers/` 的代码组织：

```text
11-20: 仍保持每个 sample 尽量自包含，方便逐步学习 VFIO / virtio PCI 机制
21-30: 已提取公共 virtio-net VFIO datapath helper，sample 文件只保留本节新增逻辑

vfio_utils.hpp       通用 VFIO / PCI config / region / capability helper
virtio_net_vfio.hpp  virtio-net queue、vring、DMA、feature、notify、packet helper
```

这个边界是刻意的：前半段更重视“每一步展开看清楚”，后半段已经进入重复的
virtio-net datapath，所以复用 helper 能降低维护成本，同时保留每个 sample 的教学目标。

## Contents

- [1. Driver 开发的基本路线](#1-driver-开发的基本路线)
- [2. PCI 设备和 BDF](#2-pci-设备和-bdf)
- [3. Sample 01: PCI Enumeration](#3-sample-01-pci-enumeration)
- [4. Sample 02: PCI Config Space](#4-sample-02-pci-config-space)
- [5. Sample 03: MMIO BAR Mapping](#5-sample-03-mmio-bar-mapping)
- [6. Prefetchable BAR](#6-prefetchable-bar)
- [7. 64-bit BAR](#7-64-bit-bar)
- [8. vfio-pci 和 sysfs resource mmap](#8-vfio-pci-和-sysfs-resource-mmap)
- [9. 读写安全规则](#9-读写安全规则)
- [10. Static Linking for Old Linux](#10-static-linking-for-old-linux)
- [11. ldd vs readelf](#11-ldd-vs-readelf)
- [12. 下一步学习方向](#12-下一步学习方向)
- [13. Sample 05: SR-IOV VF Control](#13-sample-05-sr-iov-vf-control)
- [14. Sample 06: VFIO Device Info](#14-sample-06-vfio-device-info)
- [15. Sample 07: VFIO Region Dump](#15-sample-07-vfio-region-dump)
- [16. Sample 08: VFIO DMA Map / Unmap](#16-sample-08-vfio-dma-map--unmap)
- [17. Sample 09: VFIO IRQ eventfd](#17-sample-09-vfio-irq-eventfd)
- [18. Sample 10: Virtio PCI capabilities through VFIO](#18-sample-10-virtio-pci-capabilities-through-vfio)
- [19. Sample 11: Virtio common config through VFIO](#19-sample-11-virtio-common-config-through-vfio)
- [20. Sample 12: Virtio queue info through VFIO](#20-sample-12-virtio-queue-info-through-vfio)
- [21. Sample 13: Virtio reset/status through VFIO](#21-sample-13-virtio-resetstatus-through-vfio)
- [22. Sample 14: Virtio feature bits through VFIO](#22-sample-14-virtio-feature-bits-through-vfio)
- [23. Sample 15: Virtio split vring DMA map through VFIO](#23-sample-15-virtio-split-vring-dma-map-through-vfio)
- [24. Sample 16: Program virtio queue addresses through VFIO](#24-sample-16-program-virtio-queue-addresses-through-vfio)
- [25. Sample 17: Enable one virtio queue through VFIO](#25-sample-17-enable-one-virtio-queue-through-vfio)
- [26. Sample 18: Virtio notify information through VFIO](#26-sample-18-virtio-notify-information-through-vfio)
- [27. Sample 19: Write one virtio queue notify through VFIO](#27-sample-19-write-one-virtio-queue-notify-through-vfio)
- [28. Sample 20: Set virtio DRIVER_OK through VFIO](#28-sample-20-set-virtio-driver_ok-through-vfio)
- [29. Sample 21: Post virtio-net RX buffers through VFIO](#29-sample-21-post-virtio-net-rx-buffers-through-vfio)
- [30. Sample 22: Observe virtio-net RX packets through VFIO](#30-sample-22-observe-virtio-net-rx-packets-through-vfio)
- [31. Sample 23: Send virtio-net TX packet through VFIO](#31-sample-23-send-virtio-net-tx-packet-through-vfio)
- [32. Sample 24: Echo one virtio-net RX packet through VFIO](#32-sample-24-echo-one-virtio-net-rx-packet-through-vfio)
- [33. Sample 25: Run a small virtio-net echo loop through VFIO](#33-sample-25-run-a-small-virtio-net-echo-loop-through-vfio)
- [34. Sample 26: Drive the echo loop with VFIO IRQ eventfds](#34-sample-26-drive-the-echo-loop-with-vfio-irq-eventfds)
- [35. Sample 27: Send virtio-net control command through VFIO](#35-sample-27-send-virtio-net-control-command-through-vfio)
- [36. Sample 28: Observe virtio-net promiscuous RX behavior](#36-sample-28-observe-virtio-net-promiscuous-rx-behavior)
- [37. Sample 29: Set virtio-net MAC address through VFIO](#37-sample-29-set-virtio-net-mac-address-through-vfio)
- [38. Sample 30: Set virtio-net MAC filter table through VFIO](#38-sample-30-set-virtio-net-mac-filter-table-through-vfio)
- [39. Refactor 结论：samples 21-30 的代码结构](#39-refactor-结论samples-21-30-的代码结构)
- [40. 最小心智模型](#40-最小心智模型)

## 1. Driver 开发的基本路线

Linux driver 可以大致分成两条路线：

```text
userspace driver
    app -> sysfs / vfio / mmap -> device

kernel driver
    device -> kernel driver -> /dev/xxx or netdev/block/input subsystem -> userspace
```

本项目前几个例子走的是 userspace 学习路线。它适合先理解：

```text
设备在哪里
设备是谁
它暴露了哪些资源
当前哪个 kernel driver 控制它
如何读 BAR/MMIO
```

真正 kernel driver 以后会涉及：

```text
kernel module
probe/remove
char device
PCI driver id table
interrupts
DMA
IOMMU
locking
memory ordering
```

学习顺序不要一开始就写寄存器。先读清楚设备信息，再读 config space，再读 BAR，最后才考虑写寄存器。

## 2. PCI 设备和 BDF

Linux 会把 PCI 设备暴露在：

```text
/sys/bus/pci/devices/<BDF>/
```

BDF 是：

```text
domain:bus:device.function
```

例如：

```text
0000:c1:02.7
```

含义：

```text
0000 = PCI domain
c1   = bus number
02   = device number
7    = function number
```

一个物理设备可以有多个 function。例如同一张卡上可能有 function 0、function 1、function 2 等。

短 BDF：

```text
c1:02.7
```

通常可以补成：

```text
0000:c1:02.7
```

## 3. Sample 01: PCI Enumeration

文件：

```text
userspace_drivers/01_pci_enum.cpp
```

目标：

```text
枚举 PCI 设备，打印 vendor/device/class/BAR/driver 信息。
```

运行：

```bash
./build/01_pci_enum
./build/01_pci_enum --class 02
./build/01_pci_enum --vendor 1af4
./build/01_pci_enum 0000:c1:02.7
./build/01_pci_enum c1:02.7
```

静态版本：

```bash
./build/01_pci_enum_static c1:02.7
```

示例输出：

```text
0000:c1:02.7  1af4:1041  [Network]  driver=<unbound>
    BAR4: 0x105108a0000  size=32768  mem prefetch 64-bit
```

### 3.1 vendor/device ID

sysfs 文件：

```text
/sys/bus/pci/devices/<BDF>/vendor
/sys/bus/pci/devices/<BDF>/device
```

例如：

```text
1af4:1041
```

含义：

```text
vendor id = 0x1af4
device id = 0x1041
```

真正的 kernel PCI driver 通常会有 id table：

```c
static const struct pci_device_id ids[] = {
    { PCI_DEVICE(0x1af4, 0x1041) },
    {}
};
```

kernel 发现设备 ID 匹配后，就会调用 driver 的 `probe()`。

### 3.2 class code

sysfs 文件：

```text
/sys/bus/pci/devices/<BDF>/class
```

PCI class code 是 24-bit：

```text
class : subclass : prog-if
```

常见 class：

```text
0x01 = Mass Storage
0x02 = Network
0x03 = Display
0x06 = Bridge
0x0c = Serial Bus
```

过滤网络设备：

```bash
./build/01_pci_enum --class 02
```

### 3.3 driver symlink

sysfs 路径：

```text
/sys/bus/pci/devices/<BDF>/driver
```

如果存在，它通常是 symlink，指向当前绑定的 kernel driver。

输出可能是：

```text
driver=virtio-pci
driver=vfio-pci
driver=<unbound>
```

含义：

```text
driver=virtio-pci   kernel virtio-pci driver 正在控制设备
driver=vfio-pci     VFIO 正在控制设备，应该通过 VFIO API 访问
driver=<unbound>    当前没有 kernel driver 绑定这个设备
```

不要和已经绑定的 kernel driver 抢设备。只读通常比写安全，但也不是绝对安全。

### 3.4 resource 和 BAR

sysfs 文件：

```text
/sys/bus/pci/devices/<BDF>/resource
```

每行格式：

```text
start end flags
```

前 6 行对应普通 BAR0 到 BAR5。

输出示例：

```text
BAR4: 0x105108a0000  size=32768  mem prefetch 64-bit
```

含义：

```text
BAR4             第 4 个 BAR
0x105108a0000    CPU 物理地址窗口起点
size=32768       BAR 大小
mem              memory BAR，可 mmap
prefetch         可预取
64-bit           64-bit BAR，通常占两个 BAR slot
```

## 4. Sample 02: PCI Config Space

文件：

```text
userspace_drivers/02_config_space.cpp
```

目标：

```text
读取 PCI config space，解析标准 PCI header 和 capability list。
```

运行：

```bash
./build/02_config_space 0000:c1:02.7
./build/02_config_space c1:02.7
./build/02_config_space --class 02
```

静态版本：

```bash
./build/02_config_space_static 0000:c1:02.7
```

### 4.1 config space 是什么

sysfs 文件：

```text
/sys/bus/pci/devices/<BDF>/config
```

这是一个 binary 文件，不是文本文件。

PCI config space 可以理解为：

```text
设备说明书 + 标准控制寄存器 + capability 链表入口
```

它和 BAR/MMIO 不同：

```text
config space  说明设备是谁、资源在哪里、支持什么能力
BAR/MMIO      设备真正的工作寄存器窗口
```

### 4.2 标准 header

常见 offset：

```text
0x00  vendor_id
0x02  device_id
0x04  command
0x06  status
0x08  revision
0x09  class_code
0x10  BAR0
0x14  BAR1
0x18  BAR2
0x1c  BAR3
0x20  BAR4
0x24  BAR5
0x2c  subsystem_vendor
0x2e  subsystem_device
0x34  capability pointer
0x3c  interrupt line
0x3d  interrupt pin
```

PCI config space 是 little-endian，所以读取 16-bit / 32-bit 字段时要按小端组合。

例如 bytes：

```text
f4 1a
```

读成：

```text
0x1af4
```

不是：

```text
0xf41a
```

### 4.3 command register

offset：

```text
0x04
```

常见 bit：

```text
bit 0   I/O Space Enable
bit 1   Memory Space Enable
bit 2   Bus Master Enable
bit 8   SERR Enable
bit 10  Interrupt Disable
```

最重要的前三个：

```text
I/O Space Enable       允许访问 I/O BAR
Memory Space Enable    允许访问 memory BAR / MMIO
Bus Master Enable      允许设备发起 DMA
```

真正 driver 初始化设备时，经常会 enable memory space 和 bus mastering。

### 4.4 status register

offset：

```text
0x06
```

其中 bit 4 表示：

```text
Capabilities List present
```

如果这个 bit set，说明设备有 capability list。

### 4.5 Capability List

入口：

```text
config[0x34]
```

每个 capability 节点格式：

```text
cap[0]    capability id
cap[1]    next pointer
cap[2..]  capability-specific data
```

常见 capability：

```text
0x01  Power Management
0x05  MSI
0x10  PCI Express
0x11  MSI-X
0x09  Vendor Specific
```

这是一条链表。`next pointer == 0` 表示结束。

### 4.6 MSI-X

MSI-X capability 里有两个重要区域：

```text
MSI-X table
PBA
```

它们不一定在 config space 里，通常在某个 BAR 中。

MSI-X capability 会告诉你：

```text
table 在哪个 BAR
table 在 BAR 内的 offset
PBA 在哪个 BAR
PBA 在 BAR 内的 offset
```

例如：

```text
table at BAR4 + 0x2000
```

意思是：

```text
MSI-X table 位于 BAR4 的 offset 0x2000
```

这说明 sample 01 的 BAR、sample 02 的 capability、sample 03 的 MMIO 是连在一起的。

## 5. Sample 03: MMIO BAR Mapping

文件：

```text
userspace_drivers/03_mmio.cpp
```

目标：

```text
mmap PCI memory BAR，然后读取设备寄存器。
```

运行：

```bash
./build/03_mmio 0000:c1:02.7 4 0 64
./build/03_mmio_static 0000:c1:02.7 4 0 64
```

参数：

```text
0000:c1:02.7   BDF
4              BAR index
0              offset
64             length
```

它会打开：

```text
/sys/bus/pci/devices/0000:c1:02.7/resource4
```

然后 mmap。

### 5.1 BAR 类型

BAR 分两种：

```text
I/O BAR      不能 mmap，需要 inb/outb/inl/outl 等 port I/O
Memory BAR   可以 mmap，用 load/store 访问，也叫 MMIO
```

本例只支持 memory BAR。

如果传 I/O BAR，程序应该报错：

```text
BAR0 is an I/O BAR; only memory BARs can be mmap'd
```

### 5.2 mmap resourceN

userspace 版本：

```text
open("/sys/bus/pci/devices/<BDF>/resourceN")
mmap(...)
volatile read
munmap(...)
```

kernel driver 版本大致是：

```c
pci_request_regions()
pci_iomap()
readl()
writel()
pci_iounmap()
pci_release_regions()
```

### 5.3 volatile

MMIO 不是普通内存。

设备寄存器可能：

```text
每次读取结果不同
读一次清除状态
读一次推进 FIFO
硬件异步修改值
```

如果不用 `volatile`，编译器可能缓存读结果，导致没有真正再次访问设备。

本项目 sample 里用：

```cpp
volatile uint8_t *base_;
```

真正 kernel driver 更应该用：

```c
readb()
readw()
readl()
writeb()
writew()
writel()
```

### 5.4 hexdump 输出

示例：

```text
Mapped /sys/bus/pci/devices/0000:c1:02.7/resource4 (32768 bytes)
Dumping 64 bytes at offset 0x0:
00000000  01 00 00 00 23 01 00 81 01 00 00 00 00 00 00 00  ....#...........
```

这些 bytes 是 BAR 中的设备寄存器或设备内存内容。

但是：

```text
看到 bytes 不等于知道含义
```

含义必须来自设备规范。

## 6. Prefetchable BAR

输出可能看到：

```text
mem prefetch
```

含义：

```text
这是 memory BAR
设备声明读这个区域没有副作用
CPU/bridge 可以预取、合并读、采用更宽松的缓存策略
```

常见用途：

```text
framebuffer
device memory window
shared memory area
large data buffer
PCIe BAR aperture
```

普通 `mem` 更像控制寄存器区：

```text
读写顺序重要
读可能有副作用
不应随便预取
```

`prefetchable` 不代表：

```text
可以随便写
它就是普通 RAM
可以忽略设备手册
```

它只表示设备声明这个 memory region 的读取没有副作用，平台可以优化访问。

## 7. 64-bit BAR

如果看到：

```text
mem prefetch 64-bit
```

说明这是 64-bit BAR。

在 PCI config space 中，64-bit BAR 会占两个 BAR slots。

例如：

```text
BAR4 64-bit
```

通常意味着 BAR4 和 BAR5 一起编码这个地址。

sysfs 的 `resource` 文件已经帮你整理好了，所以 sample 01/03 直接看 `resource` 更方便。

## 8. vfio-pci 和 sysfs resource mmap

你观察到过：

```text
0000:c1:02.4  driver=vfio-pci
0000:c1:02.7  driver=<unbound>
```

对 `<unbound>` 设备：

```bash
./03_mmio_static 0000:c1:02.7 4 0 64
```

可以 mmap 成功。

对 `vfio-pci` 设备：

```bash
./03_mmio_static 0000:c1:02.4 4 0 512
```

可能失败：

```text
mmap: Invalid argument
```

这很可能不是静态链接问题，也不一定是 BAR 本身问题，而是：

```text
设备已经被 vfio-pci 接管
```

绑定到 `vfio-pci` 的设备，正确访问路径通常是 VFIO API：

```text
/dev/vfio/vfio
/dev/vfio/<iommu_group>
VFIO_DEVICE_GET_REGION_INFO
mmap VFIO region
```

而不是直接 mmap：

```text
/sys/bus/pci/devices/<BDF>/resourceN
```

经验规则：

```text
driver=<unbound>       sysfs resourceN mmap 更可能可用
driver=vfio-pci        用 VFIO API
driver=normal driver   不要和 kernel driver 抢设备
```

## 9. 读写安全规则

### 9.1 读也可能有副作用

不要以为只读一定安全。

某些 MMIO register 可能是：

```text
read-to-clear
read advances FIFO
read latches status
read triggers hardware behavior
```

读通常比写安全，但不是绝对安全。

### 9.2 写寄存器必须知道语义

不要随便做：

```cpp
write32(0, 1);
```

offset 0 可能是：

```text
reset register
doorbell
queue notify
control register
interrupt clear
```

写错可能直接改变设备状态。

### 9.3 width/alignment 很重要

有些寄存器要求：

```text
只能 32-bit 访问
只能 64-bit 访问
必须 aligned
不能 byte access
不能 unaligned access
```

当前 sample 的 byte hexdump 适合学习观察，但不一定适合所有设备的正式访问方式。

### 9.4 memory ordering

MMIO 顺序很重要。

kernel driver 会用：

```c
readl()
writel()
mb()
wmb()
rmb()
```

userspace sample 里的 `volatile` 只是教学级别，不等于完整的跨架构 memory ordering 模型。

## 10. Static Linking for Old Linux

你在 Oracle Linux 9.5 上遇到过：

```text
GLIBC_2.38 not found
GLIBCXX_3.4.32 not found
```

原因：

```text
binary 在较新的系统上编译
目标机器 glibc/libstdc++ 较旧
```

解决方式之一是让 CMake 额外生成全静态版本：

```bash
cmake -S userspace_drivers -B userspace_drivers/build -DBUILD_STATIC_BINARIES=ON
cmake --build userspace_drivers/build
```

生成的程序名会带 `_static` 后缀：

```text
userspace_drivers/build/01_pci_enum_static
userspace_drivers/build/02_config_space_static
userspace_drivers/build/03_mmio_static
userspace_drivers/build/04_bind_unbind_static
```

等价的手工编译方式是：

```bash
g++ -std=c++23 -O2 -static -static-libstdc++ -static-libgcc \
    -o 01_pci_enum_static 01_pci_enum.cpp
```

注意：

```text
-static-libstdc++ -static-libgcc
```

只静态链接 C++ runtime 和 gcc runtime。

真正没有动态库依赖需要：

```text
-static
```

验证：

```bash
readelf -d ./01_pci_enum_static
```

期望输出：

```text
There is no dynamic section in this file.
```

## 11. ldd vs readelf

### 11.1 ldd

`ldd` 问的是：

```text
这个程序在当前系统运行时会加载哪些动态库？
这些库解析到哪里？
```

用法：

```bash
ldd ./binary
```

优点：

```text
直观
能看到实际路径
适合排查当前机器缺哪个 .so
```

缺点：

```text
依赖当前系统环境
显示的是这台机器上的解析结果
不适合严格判断 ELF 文件是否全静态
```

### 11.2 readelf

`readelf` 问的是：

```text
ELF 文件本身记录了什么？
有没有 dynamic section？
声明了哪些 NEEDED libraries？
```

用法：

```bash
readelf -d ./binary
```

优点：

```text
不执行程序
不依赖当前动态 linker 解析
判断是否全静态更可靠
适合交叉环境检查
```

缺点：

```text
输出更底层
不告诉你当前系统最终从哪里加载库
```

常用组合：

```bash
file ./binary
readelf -d ./binary
ldd ./binary
```

判断全静态时优先看：

```bash
readelf -d ./binary
```

## 12. 下一步学习方向

目前你已经覆盖：

```text
PCI enumeration
PCI config space
BAR resource parsing
MMIO mmap
static binary portability
vfio-pci vs unbound
```

下一步是：

```text
04_bind_unbind.cpp
```

学习：

```text
/sys/bus/pci/devices/<BDF>/driver
/sys/bus/pci/devices/<BDF>/driver_override
/sys/bus/pci/drivers/<driver>/unbind
/sys/bus/pci/drivers/<driver>/bind
```

这个 sample 用 `--show` 查看当前绑定关系，用 `--dry-run` 预览 bind/unbind 会写哪个 sysfs 文件。真正执行必须显式传 `--yes`，因为这个操作会改变真实设备绑定状态。

如果想把“先 unbind 当前 driver，再 bind 到目标 driver”合成一步，可以用 `--rebind`：

```bash
./04_bind_unbind_static 0000:c1:00.3 --rebind vfio-pci --override --yes
```

如果设备已经是 unbound，`--rebind` 会跳过 unbind，然后继续 bind。

绑定到 `vfio-pci` 时，直接写：

```bash
./04_bind_unbind_static 0000:c1:00.3 --bind vfio-pci --yes
```

不一定能成功。原因是 `bind` 仍然要经过 driver match。`vfio-pci` 通常需要先设置这个设备的 `driver_override`：

```bash
./04_bind_unbind_static 0000:c1:00.3 --bind vfio-pci --override --yes
```

这等价于先写：

```text
vfio-pci -> /sys/bus/pci/devices/0000:c1:00.3/driver_override
```

再写：

```text
0000:c1:00.3 -> /sys/bus/pci/drivers/vfio-pci/bind
```

如果仍然失败，要看 `dmesg`，常见原因包括 VFIO/IOMMU 没准备好、IOMMU group 不满足隔离要求、设备还被其他组件占用，或者 driver probe 失败。

更后面再进入：

```text
VFIO API
kernel module
PCI probe/remove
interrupts
DMA rings
IOMMU
```

## 13. Sample 05: SR-IOV VF Control

文件：

```text
userspace_drivers/05_sriov_vfs.cpp
```

SR-IOV 里通常有两类 function：

```text
PF = Physical Function，真实物理功能，负责管理 VF
VF = Virtual Function，给 VM、VFIO、DPDK 或普通 kernel driver 使用的轻量功能
```

支持 SR-IOV 的 PF 会暴露：

```text
/sys/bus/pci/devices/<PF_BDF>/sriov_totalvfs
/sys/bus/pci/devices/<PF_BDF>/sriov_numvfs
/sys/bus/pci/devices/<PF_BDF>/sriov_drivers_autoprobe
/sys/bus/pci/devices/<PF_BDF>/virtfn<N>
```

常用命令：

```bash
./05_sriov_vfs_static 0000:c1:00.0 --show
./05_sriov_vfs_static 0000:c1:00.0 --set-autoprobe 0 --yes
./05_sriov_vfs_static 0000:c1:00.0 --create 4 --yes
./05_sriov_vfs_static 0000:c1:00.0 --destroy --yes
```

`sriov_drivers_autoprobe` 控制的是“新创建 VF 时，kernel 是否自动 probe 并绑定匹配 driver”：

```text
1 = 创建 VF 后自动找 driver
0 = 创建 VF 后不自动绑定，VF 通常保持 unbound
```

如果目标是把 VF 交给 `vfio-pci`，常见流程是：

```bash
./05_sriov_vfs_static 0000:c1:00.0 --destroy --yes
./05_sriov_vfs_static 0000:c1:00.0 --set-autoprobe 0 --yes
./05_sriov_vfs_static 0000:c1:00.0 --create 4 --yes
./05_sriov_vfs_static 0000:c1:00.0 --show
./04_bind_unbind_static 0000:c1:00.3 --bind vfio-pci --override --yes
```

这个顺序的关键点：

```text
PF 要先绑定在 vendor PF driver 上，不能先把 PF 绑到 vfio-pci
sriov_drivers_autoprobe 要在 create VF 之前设置
改变 autoprobe 前要求 sriov_numvfs=0
create 时要求 sriov_numvfs=0
destroy 会删除现有 VF，可能影响 VM、VFIO、DPDK、网络配置
```

virtio-net PF 还有一个容易踩的坑：

```text
PF PCI driver = virtio-pci 还不够
virtio child device 需要先绑定 virtio_net，让设备进入 DRIVER_OK
否则写 sriov_numvfs 可能返回 EBUSY，即使 sriov_numvfs=0 且没有 virtfn<N>
```

遇到这种情况先加载 `virtio_net`，再创建 VF：

```bash
modprobe virtio_net
./05_sriov_vfs_static 0000:c1:00.0 --set-autoprobe 0 --yes
./05_sriov_vfs_static 0000:c1:00.0 --create 4 --yes
```

你测试到的现象是正确的：

```text
PF driver = vfio-pci 时，通常不能创建 VF
```

原因是 VF 创建不是单纯 PCI core 自己完成的动作。它需要 PF driver 参与设备相关的 SR-IOV 初始化。`vfio-pci` 是通用 passthrough driver，不负责这个设备的 PF 管理逻辑。正确模型是：

```text
PF 绑定 vendor driver -> 创建 VF -> VF 按需绑定 vfio-pci
```

## 14. Sample 06: VFIO Device Info

文件：

```text
userspace_drivers/06_vfio_info.cpp
```

当设备已经绑定到 `vfio-pci` 后，userspace 不应该再把主要访问路径建立在：

```text
/sys/bus/pci/devices/<BDF>/resourceN
```

正确入口变成：

```text
/dev/vfio/vfio
/dev/vfio/<iommu_group_id>
```

运行：

```bash
./06_vfio_info_static 0000:c1:00.3 --show
```

这个 sample 做的是只读查询：

```text
1. 找到 /sys/bus/pci/devices/<BDF>/iommu_group
2. 打开 /dev/vfio/vfio
3. 检查 VFIO API version
4. 检查 VFIO_TYPE1_IOMMU / VFIO_TYPE1v2_IOMMU
5. 打开 /dev/vfio/<group_id>
6. 检查 group 是否 viable
7. 把 group attach 到 container
8. 设置 container IOMMU type
9. 获取 device fd
10. 打印 device info
11. 打印 region info
12. 打印 IRQ info
```

VFIO 里最重要的三个对象：

```text
container = 一个 VFIO 地址空间/IOMMU 上下文
group     = IOMMU 隔离单位，同组设备必须一起安全处理
device    = 具体的 vfio-pci 设备 fd
```

`group viable` 是一个关键检查。它大致表示：

```text
这个 IOMMU group 里的设备都处在 VFIO 认为安全的状态
```

如果 group 不 viable，常见原因是同一个 IOMMU group 里还有设备绑定在普通 kernel driver 上。`06_vfio_info` 会打印 group 里的设备和当前 driver，方便排查。

region 对应的是设备暴露给 userspace 的地址空间：

```text
BAR0..BAR5 = PCI BAR
ROM        = PCI ROM region
CONFIG     = PCI config space
VGA        = legacy VGA region
```

`num_regions` 表示可以尝试查询的最大 region index + 1，但不代表每个 index 对当前设备都一定可用。比如非 VGA 设备查询 `VFIO_PCI_VGA_REGION_INDEX` 时可能返回 `EINVAL`，这不是 VFIO 初始化失败，只表示这个 legacy VGA region 对该设备不可用。

region flags 里常见字段：

```text
READ   = 支持 read/pread
WRITE  = 支持 write/pwrite
MMAP   = 支持 mmap
CAPS   = 有额外 capability 信息，例如 sparse mmap
```

这也解释了之前的现象：

```text
driver=vfio-pci 时，sysfs resourceN mmap 可能失败
```

因为设备已经交给 VFIO 管理，后续应该通过 VFIO device fd 和 VFIO region offset 来 mmap BAR。下一步可以做：

```text
07_vfio_region_dump.cpp
```

用 VFIO mmap region，替代 `03_mmio.cpp` 的 sysfs resource mmap。

## 15. Sample 07: VFIO Region Dump

文件：

```text
userspace_drivers/07_vfio_region_dump.cpp
```

`03_mmio.cpp` 的访问路径是：

```text
/sys/bus/pci/devices/<BDF>/resourceN
```

这适合未绑定普通 kernel driver、或者没有交给 `vfio-pci` 的设备。设备绑定到 `vfio-pci` 后，应该走：

```text
VFIO device fd + VFIO region offset
```

运行：

```bash
./07_vfio_region_dump_static 0000:c1:00.3 4 0 64
```

参数含义：

```text
0000:c1:00.3 = BDF
4             = VFIO region index，也就是 BAR4
0             = region 内偏移
64            = dump 长度
```

它做的事情：

```text
1. 确认设备当前 driver 是 vfio-pci
2. 打开 VFIO container/group/device
3. 查询 region info
4. 确认 region size 非 0
5. 确认 region 有 VFIO_REGION_INFO_FLAG_MMAP
6. 如果 region 有 sparse mmap capability，确认请求范围在可 mmap area 内
7. 把 region-relative offset 做 page align
8. mmap device fd 的 region.offset + aligned_offset
9. 用 volatile read8 做 hexdump
```

你在 sample 06 看到：

```text
region 4 (BAR4)
  size: 0x8000
  offset: 0x40000000000
  flags: READ|WRITE|MMAP|CAPS
```

所以 BAR4 可以用 sample 07 读取。后续如果要写 register，需要单独加显式写接口和更强的安全限制；现在这个 sample 只读。

`CAPS` 里可能包含 sparse mmap 信息。含义是：

```text
不是整个 region 都保证可以 mmap
只能 mmap capability 列出的部分 offset/size
```

这常见于 BAR 里混有 MSI-X table 或其它不应该直接 mmap 的区域。sample 07 会解析这个 capability，避免 mmap 到不可映射的子范围。

## 16. Sample 08: VFIO DMA Map / Unmap

文件：

```text
userspace_drivers/08_vfio_dma_map.cpp
```

设备做 DMA 时不会使用你的 userspace virtual address。设备看到的是 IOVA：

```text
userspace virtual address -> VFIO_IOMMU_MAP_DMA -> IOVA
```

运行 dry-run：

```bash
./08_vfio_dma_map_static 0000:c1:00.3
```

真正做一次 map/unmap 测试：

```bash
./08_vfio_dma_map_static 0000:c1:00.3 0x100000000 4096 --yes
```

参数含义：

```text
0000:c1:00.3 = BDF
0x100000000  = IOVA，也就是设备侧将来看到的地址
4096         = buffer size，会按 page size 向上取整
--yes        = 真的调用 VFIO_IOMMU_MAP_DMA / VFIO_IOMMU_UNMAP_DMA
```

这个 sample 做的事情：

```text
1. 确认设备当前 driver 是 vfio-pci
2. 打开 VFIO container/group/device
3. 设置 VFIO Type1 IOMMU
4. 查询 VFIO_IOMMU_GET_INFO
5. mmap 一块匿名 userspace buffer
6. 填入简单 pattern
7. VFIO_IOMMU_MAP_DMA，把 buffer 映射到 IOVA
8. VFIO_IOMMU_UNMAP_DMA，立刻解除映射
```

这个 sample 不会：

```text
写 BAR register
告诉设备这个 IOVA
启动 device DMA
```

所以它只是练习 IOMMU 映射流程。真正让设备 DMA 还需要后续步骤：

```text
把 IOVA 写进设备 descriptor / queue / register
启动设备队列
处理中断或 polling completion
```

`VFIO_DMA_MAP_FLAG_READ` 和 `VFIO_DMA_MAP_FLAG_WRITE` 是从设备视角说的：

```text
READ  = device 可以读 host memory
WRITE = device 可以写 host memory
```

默认 sample 使用 `READ|WRITE`。也可以显式指定：

```bash
./08_vfio_dma_map_static 0000:c1:00.3 0x100000000 4096 --read --yes
./08_vfio_dma_map_static 0000:c1:00.3 0x100000000 4096 --read --write --yes
```

## 17. Sample 09: VFIO IRQ eventfd

文件：

```text
userspace_drivers/09_vfio_irq_eventfd.cpp
```

VFIO interrupt 通常不会直接变成 Unix signal。userspace driver 会提供一个
`eventfd`，让 kernel 在中断到来时递增这个 fd 的 counter：

```text
eventfd() -> VFIO_DEVICE_SET_IRQS -> poll(eventfd)
```

先查看设备暴露了哪些 IRQ index：

```bash
./09_vfio_irq_eventfd_static 0000:c1:00.6 --show
```

输出里常见的 PCI IRQ index 是：

```text
0 = INTx
1 = MSI
2 = MSI-X
3 = ERR
4 = REQ
```

把某个 IRQ index/vector 绑定到 eventfd，然后等待：

```bash
./09_vfio_irq_eventfd_static 0000:c1:00.6 --irq 2 --vector 0 --wait-ms 5000 --yes
```

如果只是想验证 VFIO eventfd wiring 本身，不依赖真实设备产生中断，可以使用
VFIO 的 loopback trigger：

```bash
./09_vfio_irq_eventfd_static 0000:c1:00.6 --irq 2 --vector 0 --trigger-test --yes
```

这个 sample 做的事情：

```text
1. 确认设备当前 driver 是 vfio-pci
2. 打开 VFIO container/group/device
3. 查询 VFIO_DEVICE_GET_INFO
4. 查询 VFIO_DEVICE_GET_IRQ_INFO
5. eventfd(0, EFD_NONBLOCK)
6. VFIO_DEVICE_SET_IRQS，把 IRQ trigger 绑定到 eventfd
7. poll(eventfd)
8. read(eventfd) 读取 counter
9. 用 eventfd=-1 解除 IRQ 绑定
```

这个 sample 不会：

```text
配置设备 queue
写 BAR register
启动 device DMA
保证真实硬件一定会产生 interrupt
```

如果没有 `--trigger-test`，程序只是在等真实设备 interrupt。没有 queue/DMA 或设备事件时，
timeout 是正常结果。

如果打开 group 时报 `cannot open /dev/vfio/<group>: Device or resource busy`，
说明这个 VFIO group 已经被另一个进程打开。VFIO group 通常是独占的，先定位占用者：

```bash
fuser -v /dev/vfio/87
lsof /dev/vfio/87
```

## 18. Sample 10: Virtio PCI capabilities through VFIO

文件：

```text
userspace_drivers/10_virtio_vfio_pci_caps.cpp
```

modern virtio PCI 设备会在 PCI config space 里放 vendor capability。这些
capability 不是寄存器本身，而是“地图”：告诉 driver `common_cfg`、
`notify_cfg`、`isr_cfg`、`device_cfg` 等结构在哪个 BAR、哪个 offset。

运行：

```bash
./10_virtio_vfio_pci_caps_static 0000:c1:00.6 --show
```

这个 sample 做的事情：

```text
1. 确认设备当前 driver 是 vfio-pci
2. 打开 VFIO container/group/device
3. 查询 VFIO PCI CONFIG region
4. 通过 VFIO device fd pread PCI config space
5. 从 PCI_CAPABILITY_LIST 开始遍历 capability list
6. 找 PCI_CAP_ID_VNDR 的 virtio capability
7. 打印 cfg_type、bar、offset、length
8. 对 NOTIFY_CFG 额外打印 notify_off_multiplier
```

常见 virtio cfg_type：

```text
1 = COMMON_CFG
2 = NOTIFY_CFG
3 = ISR_CFG
4 = DEVICE_CFG
5 = PCI_CFG
8 = SHM_CFG
9 = VENDOR_CFG
```

这个 sample 不会：

```text
mmap BAR
写 common_cfg
reset device
enable queue
启动 device DMA
```

它的作用是先把 virtio register map 找出来。后续 sample 再基于这些
BAR/offset 去读 `device_status`、`num_queues`、`queue_size` 等字段。

## 19. Sample 11: Virtio common config through VFIO

文件：

```text
userspace_drivers/11_virtio_vfio_common_cfg.cpp
```

sample 10 找到了 `COMMON_CFG` 在哪个 BAR、哪个 offset。sample 11 会把这个
BAR range 通过 VFIO mmap 出来，并读取 common config 里的基础字段：

```bash
./11_virtio_vfio_common_cfg_static 0000:c1:00.6 --show
```

这个 sample 做的事情：

```text
1. 确认设备当前 driver 是 vfio-pci
2. 打开 VFIO container/group/device
3. 解析 virtio COMMON_CFG capability
4. 查询 COMMON_CFG 所在 BAR 的 VFIO region info
5. mmap COMMON_CFG 对应的 BAR range
6. 读取 device_status、num_queues、config_generation
7. 读取当前 selected queue 的 queue_size、queue_enable、queue_notify_off
```

这个 sample 不会：

```text
写 device_status
写 queue_select
协商 feature
enable queue
notify device
启动 device DMA
```

注意：`queue_*` 字段是“当前 selected queue”的视图。本 sample 不写
`queue_select`，所以它只读取设备当前 selection 对应的 queue。后续 sample
再专门练习选择 queue、读 queue 参数、配置 vring。

## 20. Sample 12: Virtio queue info through VFIO

文件：

```text
userspace_drivers/12_virtio_vfio_queue_info.cpp
```

virtio common config 里的 queue 字段是 selected-queue window：

```text
write queue_select = N
read queue_size / queue_enable / queue_notify_off / queue_desc / queue_avail / queue_used
```

默认 dry-run 不写寄存器，只显示当前 selected queue，并说明将要写哪些 selection：

```bash
./12_virtio_vfio_queue_info_static 0000:c1:00.6 --show
```

真正枚举所有 queue：

```bash
./12_virtio_vfio_queue_info_static 0000:c1:00.6 --show --yes
```

只看单个 queue：

```bash
./12_virtio_vfio_queue_info_static 0000:c1:00.6 --show --queue 0 --yes
```

这个 sample 做的事情：

```text
1. 确认设备当前 driver 是 vfio-pci
2. 打开 VFIO container/group/device
3. 解析 virtio COMMON_CFG capability
4. mmap COMMON_CFG 所在 BAR range
5. 读取原始 queue_select
6. 写 queue_select=N
7. 读取 queue_size、queue_enable、queue_notify_off、queue 地址字段
8. 退出前恢复原始 queue_select
```

这个 sample 不会：

```text
写 device_status
协商 feature
写 queue_desc / queue_avail / queue_used
enable queue
notify device
启动 device DMA
```

虽然只写 `queue_select`，它仍然是设备寄存器写入，所以真实枚举需要 `--yes`。

## 21. Sample 13: Virtio reset/status through VFIO

文件：

```text
userspace_drivers/13_virtio_vfio_reset_status.cpp
```

virtio 初始化的第一个状态机是 `device_status`：

```text
0 -> ACKNOWLEDGE -> ACKNOWLEDGE|DRIVER -> feature negotiation -> FEATURES_OK -> DRIVER_OK
```

sample 13 只练习最前面的安全步骤：

```bash
./13_virtio_vfio_reset_status_static 0000:c1:00.6 --show
./13_virtio_vfio_reset_status_static 0000:c1:00.6 --reset --yes
./13_virtio_vfio_reset_status_static 0000:c1:00.6 --reset --ack-driver --yes
```

这个 sample 做的事情：

```text
1. 确认设备当前 driver 是 vfio-pci
2. 打开 VFIO container/group/device
3. 解析 virtio COMMON_CFG capability
4. mmap COMMON_CFG 所在 BAR range
5. 读取 device_status
6. 可选写 device_status=0，等待 reset 完成
7. 可选写 ACKNOWLEDGE，再写 ACKNOWLEDGE|DRIVER
```

这个 sample 不会：

```text
协商 feature
写 FEATURES_OK
写 DRIVER_OK
配置 queue
notify device
启动 device DMA
```

`--reset` 会改变真实设备状态，可能清掉之前的 queue/config 状态，所以真实写入需要
`--yes`。

## 22. Sample 14: Virtio feature bits through VFIO

文件：

```text
userspace_drivers/14_virtio_vfio_feature_bits.cpp
```

virtio feature registers 也是 selected-window 形式：

```text
write device_feature_select = N
read  device_feature

write guest_feature_select = N
write guest_feature
```

sample 14 有两种模式：

```bash
./14_virtio_vfio_feature_bits_static 0000:c1:00.6 --show
./14_virtio_vfio_feature_bits_static 0000:c1:00.6 --show --yes
./14_virtio_vfio_feature_bits_static 0000:c1:00.6 --negotiate-minimal --yes
```

`--show` 默认不写寄存器，只显示当前 selected feature view，并说明如果加
`--yes` 会写哪些 selector。`--show --yes` 会写
`device_feature_select=0..2` 和 `guest_feature_select=0..2` 来枚举 feature
words，然后恢复原 selector。

`--negotiate-minimal --yes` 做的事情：

```text
1. 写 device_status=0，等待 reset 完成
2. 写 ACKNOWLEDGE
3. 写 ACKNOWLEDGE|DRIVER
4. 读取 device feature words
5. 写最小 guest feature set：
   - VIRTIO_F_VERSION_1
   - 如果设备提供 VIRTIO_F_ACCESS_PLATFORM，也接受它
6. 写 ACKNOWLEDGE|DRIVER|FEATURES_OK
7. 读取 device_status，确认 FEATURES_OK 没被设备清掉
```

这个 sample 不会：

```text
写 DRIVER_OK
配置 queue
写 queue_desc / queue_avail / queue_used
notify device
启动 device DMA
```

`FEATURES_OK` 只是说明 device 接受了 guest feature subset。真正让设备开始工作的
是更后面的 `DRIVER_OK`，需要先配置 virtqueue 和 DMA buffer。

## 23. Sample 15: Virtio split vring DMA map through VFIO

文件：

```text
userspace_drivers/15_virtio_vfio_vring_map.cpp
```

sample 15 进入 virtqueue 内存布局，但仍然不启动设备。split virtqueue 可以看成三块：

```text
descriptor table
available ring
used ring
```

modern virtio PCI 有三个独立地址寄存器：

```text
queue_desc
queue_avail
queue_used
```

本 sample 只计算这三块在一个连续 userspace buffer 里的 offset，并把整个 buffer
通过 `VFIO_IOMMU_MAP_DMA` 映射成 IOVA。它不会把这些 IOVA 写进设备寄存器。

运行：

```bash
./15_virtio_vfio_vring_map_static 0000:c1:00.6 --queue 0
./15_virtio_vfio_vring_map_static 0000:c1:00.6 --queue 0 --yes
./15_virtio_vfio_vring_map_static 0000:c1:00.6 --queue 0 --queue-size 128 --iova 0x200000000 --yes
```

默认 dry-run 不打开 VFIO、不写寄存器。如果指定 `--queue-size`，dry-run 可以直接
计算 vring 布局；如果不指定，则真实执行时从设备的目标 queue 读取 `queue_size`。

`--yes` 做的事情：

```text
1. 确认设备当前 driver 是 vfio-pci
2. 打开 VFIO container/group/device
3. mmap virtio COMMON_CFG
4. 保存原始 queue_select
5. 写 queue_select=N，读取目标 queue_size / queue_enable / queue_notify_off
6. 计算 split vring 的 desc/avail/used offset 和 IOVA
7. mmap 一个 zeroed userspace buffer
8. 用 VFIO_IOMMU_MAP_DMA 把 buffer 映射到指定 IOVA
9. 立即 VFIO_IOMMU_UNMAP_DMA
10. 恢复原始 queue_select
```

这个 sample 不会：

```text
写 queue_desc / queue_avail / queue_used
写 queue_enable
notify device
写 DRIVER_OK
启动 device DMA
```

如果目标 queue 已经 enabled，sample 会拒绝继续。这个检查避免在已有 driver 状态上
做教程实验。

## 24. Sample 16: Program virtio queue addresses through VFIO

文件：

```text
userspace_drivers/16_virtio_vfio_queue_program.cpp
```

sample 16 在 sample 15 的基础上再前进一步：把 vring 的三个 IOVA 写入设备的
queue 地址寄存器。

运行：

```bash
./16_virtio_vfio_queue_program_static 0000:c1:00.6 --queue 0
./16_virtio_vfio_queue_program_static 0000:c1:00.6 --queue 0 --yes
./16_virtio_vfio_queue_program_static 0000:c1:00.6 --queue 0 --queue-size 128 --iova 0x200000000 --yes
```

`--yes` 做的事情：

```text
1. reset device
2. 写 ACKNOWLEDGE、DRIVER
3. 读取 device features，写最小 guest features
4. 写 FEATURES_OK，并确认设备接受
5. 写 queue_select=N
6. 读取目标 queue 的 max queue_size
7. 计算 split vring desc/avail/used IOVA
8. mmap zeroed userspace buffer，并 VFIO_IOMMU_MAP_DMA
9. 写 queue_size
10. 写 queue_desc / queue_avail / queue_used
11. 读回 queue registers 做验证
12. 恢复原始 queue_select
13. reset device，清掉刚写入的 queue 地址
14. VFIO_IOMMU_UNMAP_DMA
```

这个 sample 不会：

```text
写 queue_enable
notify device
写 DRIVER_OK
启动 device DMA
```

为什么退出前要 reset：

```text
只要 queue_desc/queue_avail/queue_used 指向 userspace IOVA，
unmap 之前就应该先让设备忘掉这些地址。
```

虽然本 sample 不 enable queue、不 DRIVER_OK，理论上设备不应该开始使用这些地址，
但 reset cleanup 是 userspace driver 里更稳妥的习惯。

## 25. Sample 17: Enable one virtio queue through VFIO

文件：

```text
userspace_drivers/17_virtio_vfio_queue_enable.cpp
```

sample 17 在 sample 16 的基础上再写一步：

```text
queue_enable = 1
```

运行：

```bash
./17_virtio_vfio_queue_enable_static 0000:c1:00.6 --queue 0
./17_virtio_vfio_queue_enable_static 0000:c1:00.6 --queue 0 --yes
./17_virtio_vfio_queue_enable_static 0000:c1:00.6 --queue 0 --queue-size 128 --iova 0x200000000 --yes
```

`--yes` 做的事情：

```text
1. reset device
2. 写 ACKNOWLEDGE、DRIVER
3. 协商最小 feature set，并确认 FEATURES_OK
4. 写 queue_select=N
5. 读取目标 queue 的 max queue_size
6. 计算 split vring desc/avail/used IOVA
7. mmap zeroed userspace buffer，并 VFIO_IOMMU_MAP_DMA
8. 写 queue_size / queue_desc / queue_avail / queue_used
9. 写 queue_enable=1
10. 读回 queue registers，确认 queue_enable=1
11. 恢复原始 queue_select
12. reset device，清掉 enabled queue 和 queue 地址
13. VFIO_IOMMU_UNMAP_DMA
```

这个 sample 不会：

```text
notify device
写 DRIVER_OK
启动 device DMA
```

`queue_enable=1` 表示这条 virtqueue 的配置已经交给 device。真正让 device 进入
运行状态还需要后续的 `DRIVER_OK`。本 sample 退出前 reset，是为了避免 enabled
queue 在 DMA memory 被 unmap 后仍留在设备里。

## 26. Sample 18: Virtio notify information through VFIO

文件：

```text
userspace_drivers/18_virtio_vfio_notify_info.cpp
```

sample 18 不启动设备，也不写 notify register。它只回答一个问题：

```text
如果后续要 notify 某个 queue，应该写哪个 MMIO offset？
```

modern virtio PCI 的 notify 地址来自两部分：

```text
NOTIFY_CFG.offset + queue_notify_off * notify_off_multiplier
```

运行：

```bash
./18_virtio_vfio_notify_info_static 0000:c1:00.6 --show
./18_virtio_vfio_notify_info_static 0000:c1:00.6 --show --queue 0
./18_virtio_vfio_notify_info_static 0000:c1:00.6 --show --queue 0 --yes
```

默认 `--show` 不写寄存器，只基于当前 selected queue 计算 notify 地址。如果指定
`--queue N` 但不加 `--yes`，程序只说明将要写 `queue_select=N`。加 `--yes` 后才会
写 `queue_select`，读取目标 queue 的 `queue_notify_off`，计算 notify offset，然后
恢复原始 `queue_select`。

这个 sample 做的事情：

```text
1. 解析 COMMON_CFG 和 NOTIFY_CFG capabilities
2. 读取 notify_off_multiplier
3. mmap COMMON_CFG
4. 读取当前 selected queue 的 queue_notify_off
5. 可选写 queue_select=N，读取目标 queue_notify_off
6. 计算 BAR-relative notify offset
7. 打印 VFIO device fd file offset
8. 恢复原始 queue_select
```

这个 sample 不会：

```text
写 notify MMIO
写 queue_enable
写 DRIVER_OK
启动 device DMA
```

后续真正 notify 时，如果没有协商 `VIRTIO_F_NOTIFICATION_DATA`，通常写入 queue
index；如果协商了 `VIRTIO_F_NOTIFICATION_DATA`，则写入 `queue_notify_data`。
sample 18 会把这两个候选值都打印出来。

## 27. Sample 19: Write one virtio queue notify through VFIO

文件：

```text
userspace_drivers/19_virtio_vfio_notify_write.cpp
```

sample 19 在 sample 17/18 的基础上写一次真正的 notify MMIO：

```text
notify_addr = NOTIFY_CFG.offset + queue_notify_off * notify_off_multiplier
write16(notify_addr, queue_index)
```

运行：

```bash
./19_virtio_vfio_notify_write_static 0000:c1:00.6 --queue 0
./19_virtio_vfio_notify_write_static 0000:c1:00.6 --queue 0 --yes
./19_virtio_vfio_notify_write_static 0000:c1:00.6 --queue 0 --queue-size 128 --iova 0x200000000 --yes
```

`--yes` 做的事情：

```text
1. reset device
2. 写 ACKNOWLEDGE、DRIVER
3. 协商最小 feature set，并确认 FEATURES_OK
4. 写 queue_select=N
5. 配置 split vring DMA，并写 queue_size/desc/avail/used
6. 写 queue_enable=1
7. 解析 NOTIFY_CFG，计算目标 notify MMIO offset
8. mmap notify MMIO window
9. 写一个 16-bit notify value
10. 恢复原始 queue_select
11. reset device，清掉 enabled queue 和 queue 地址
12. VFIO_IOMMU_UNMAP_DMA
```

这个 sample 不会：

```text
写 DRIVER_OK
启动正常 device operation
提交任何 available descriptor
```

因为本 sample 没有协商 `VIRTIO_F_NOTIFICATION_DATA`，notify value 使用 queue
index。即使 kernel header 暴露了 `queue_notify_data`，这里只把它作为候选值打印。

## 28. Sample 20: Set virtio DRIVER_OK through VFIO

文件：

```text
userspace_drivers/20_virtio_vfio_driver_ok.cpp
```

sample 20 第一次写 `DRIVER_OK`。这意味着 device 可以开始正常运行，所以本 sample
仍然保持 queue 为空：

```text
avail.idx = 0
没有 descriptor
不 notify queue
```

运行：

```bash
./20_virtio_vfio_driver_ok_static 0000:c1:00.6 --queue 0
./20_virtio_vfio_driver_ok_static 0000:c1:00.6 --queue 0 --yes
./20_virtio_vfio_driver_ok_static 0000:c1:00.6 --queue 0 --queue-size 128 --iova 0x200000000 --wait-ms 500 --yes
```

`--yes` 做的事情：

```text
1. reset device
2. 写 ACKNOWLEDGE、DRIVER
3. 协商最小 feature set，并确认 FEATURES_OK
4. 写 queue_select=N
5. 配置 split vring DMA，并写 queue_size/desc/avail/used
6. 写 queue_enable=1
7. 写 DRIVER_OK
8. 等待 --wait-ms，读回 device_status
9. 恢复原始 queue_select
10. reset device，清掉 enabled queue 和 queue 地址
11. VFIO_IOMMU_UNMAP_DMA
```

这个 sample 不会：

```text
提交 available descriptor
notify queue
提供 packet buffer
```

注意：`DRIVER_OK` 后 device 已经可以读取 virtqueue 元数据，因此 vring DMA memory
必须一直保持映射，直到 reset 完成。

### 28.1 Gotcha: DRIVER_OK 后出现 NEEDS_RESET

这个 sample 可能看到：

```text
after DRIVER_OK: 0x0f (ACKNOWLEDGE|DRIVER|DRIVER_OK|FEATURES_OK)
after wait: 0x4f (ACKNOWLEDGE|DRIVER|DRIVER_OK|FEATURES_OK|NEEDS_RESET)
```

这不表示 VFIO mmap 或 queue register 写入失败。它表示 device 在 `DRIVER_OK`
之后发现当前 driver setup 不足以继续运行。

本 sample 故意只启用一个空 queue：

```text
没有 RX buffer
没有 TX descriptor
没有 notify queue
没有完整的 virtio-net queue pair
```

对 virtio-net 来说，这种最小状态很可能触发 `NEEDS_RESET`。这个现象正好说明：

```text
DRIVER_OK 是真正进入 device runtime 的边界。
```

所以 sample 20 的定位是演示 `DRIVER_OK` 状态边界，而不是一个可持续运行的
virtio-net userspace driver。

## 29. Sample 21: Post virtio-net RX buffers through VFIO

文件：

```text
userspace_drivers/21_virtio_net_rx_buffers.cpp
```

sample 21 是第一个真正给 virtio-net device 提供 RX DMA buffer 的例子。它默认使用：

```text
queue 0 = RX
queue 1 = TX
```

这对应没有协商 `VIRTIO_NET_F_MQ` 时的第一个 RX/TX queue pair。

运行：

```bash
./21_virtio_net_rx_buffers_static 0000:c1:00.6
./21_virtio_net_rx_buffers_static 0000:c1:00.6 --queue-size 128 --iova 0x200000000 --yes
./21_virtio_net_rx_buffers_static 0000:c1:00.6 --queue-size 128 --rx-buffers 16 --rx-buffer-size 4096 --wait-ms 2000 --yes
```

`--yes` 做的事情：

```text
1. reset device
2. 写 ACKNOWLEDGE、DRIVER
3. 协商最小 feature set，并确认 FEATURES_OK
4. 读取 RX queue 0 和 TX queue 1 的 queue_size
5. 分配一块 DMA memory，里面放 RX vring、TX vring、RX packet buffers
6. 把 RX descriptors 填成 writable buffer descriptors
7. 写 RX avail.ring[] 和 RX avail.idx
8. 配置并 enable RX queue
9. 配置并 enable TX queue
10. 写 DRIVER_OK
11. notify RX queue
12. 在 --wait-ms 时间内观察 RX used.idx
13. 恢复原始 queue_select
14. reset device
15. VFIO_IOMMU_UNMAP_DMA
```

这个 sample 仍然不会：

```text
发送 TX packet
处理 control virtqueue
解析收到的 packet
回收 used RX descriptor
长期运行
```

注意：从这个 sample 开始，device 可以真实 DMA 写入 userspace buffer。它应该只在隔离的
VFIO 测试设备上运行。

如果 `RX used.idx` 没有变化，不一定表示 setup 失败；可能只是没有外部流量进入这个
virtio-net device。更关键的观察点是：`DRIVER_OK` 后是否还能维持一段时间而不立刻进入
`NEEDS_RESET`。

## 30. Sample 22: Observe virtio-net RX packets through VFIO

文件：

```text
userspace_drivers/22_virtio_net_rx_observe.cpp
```

sample 22 继续使用 sample 21 的 RX/TX queue pair setup，但它会等待 RX used ring
前进，并根据 used descriptor id 找回对应的 RX buffer，dump buffer 前缀。

和前面的只读/半初始化 sample 不同，这个 sample 需要 device 真实执行 DMA，所以它还会
通过 VFIO CONFIG region 检查并打开 PCI command register 里的：

```text
PCI_COMMAND_MEMORY
PCI_COMMAND_MASTER
```

退出前会 reset device，并把 PCI command register 恢复到进入 sample 前的值。

它还会解析 `DEVICE_CFG` 并打印 virtio-net config 中的 MAC/status。feature negotiation
会在 device 提供时接受：

```text
VIRTIO_NET_F_MAC
VIRTIO_NET_F_STATUS
```

这有助于确认 userspace 发送端使用的目的 MAC 是否等于 VF#3 的真实 virtio-net config
MAC，也能看到 backend 暴露的 link status。

运行：

```bash
./22_virtio_net_rx_observe_static 0000:c1:00.6
./22_virtio_net_rx_observe_static 0000:c1:00.6 --queue-size 128 --rx-buffer-size 4096 --wait-ms 10000 --yes
./22_virtio_net_rx_observe_static 0000:c1:00.6 --rx-buffers 64 --stop-after 4 --dump-bytes 128 --yes
```

`--yes` 做的事情：

```text
1. reset device
2. 打开 PCI Memory Space / Bus Master
3. 写 ACKNOWLEDGE、DRIVER
4. 协商 feature set，并确认 FEATURES_OK
5. 读取并打印 virtio-net device config MAC/status
6. 配置并 enable RX queue 0
7. 配置并 enable TX queue 1
8. 发布 RX descriptors
9. 写 DRIVER_OK
10. notify RX queue
11. 等待 RX used.idx 前进，最多等 --wait-ms
12. 对 used entries 打印 desc id 和 len
13. 根据 desc id 找到 RX buffer
14. dump virtio-net header、Ethernet header 和前 --dump-bytes 字节
15. reset device
16. 恢复 PCI command register
17. VFIO_IOMMU_UNMAP_DMA
```

因为本 sample 协商了 `VIRTIO_F_VERSION_1`，RX buffer 开头按 modern
`virtio_net_hdr_v1` 解释，也就是 12-byte virtio-net header：

```text
flags
gso_type
hdr_len
gso_size
csum_start
csum_offset
num_buffers
```

随后才是 Ethernet frame。

如果想看到 `RX used.idx` 增长，需要让对端向这个 virtio-net device 发流量。比如让对端
发 ARP、ping 或 broadcast。没有流量时，sample 可以保持 `DRIVER_OK` 但不会 dump packet。

这个 sample 仍然不是完整网络 driver：

```text
不 recycle RX descriptor
不发送 TX packet
不处理 control virtqueue
不注册 Linux netdev
```

它的定位是证明：userspace 已经可以让 virtio-net device DMA 写入 packet buffer，并从
used ring 取回完成信息。

## 31. Sample 23: Send virtio-net TX packet through VFIO

文件：

```text
userspace_drivers/23_virtio_net_tx_packet.cpp
```

sample 23 走 TX 方向：userspace 构造一个 modern virtio-net TX header 加 Ethernet
frame，把它作为 device-readable descriptor 放进 TX queue，然后 notify TX 并等待 TX
used ring completion。

运行：

```bash
./23_virtio_net_tx_packet_static 0000:c1:00.6
./23_virtio_net_tx_packet_static 0000:c1:00.6 --dst-mac fe:bf:30:01:30:01 --wait-ms 5000 --yes
./23_virtio_net_tx_packet_static 0000:c1:00.6 --ethertype 0x88b5 --payload-hex 00112233445566778899 --yes
```

默认行为：

```text
src-mac   = virtio-net DEVICE_CFG 里的 MAC
dst-mac   = ff:ff:ff:ff:ff:ff
ethertype = 0x88b5
payload   = 内置 vfio-tx-test payload
```

`--yes` 做的事情：

```text
1. 打开 PCI Memory Space / Bus Master
2. reset device
3. 协商 feature set，并确认 FEATURES_OK
4. 读取并打印 virtio-net device config MAC/status
5. 发布 RX buffers，避免 device 进入不完整 queue pair 状态
6. 构造 TX packet：12-byte virtio_net_hdr_v1 + Ethernet frame
7. 发布一个 TX descriptor，并写 TX avail.idx=1
8. 配置并 enable RX queue 0
9. 配置并 enable TX queue 1
10. 写 DRIVER_OK
11. notify RX queue
12. notify TX queue
13. 等待 TX used.idx 前进，最多等 --wait-ms
14. reset device
15. 恢复 PCI command register
16. VFIO_IOMMU_UNMAP_DMA
```

如果 TX used ring 前进，说明 device/backend 已经读取了 TX descriptor 对应的 userspace
DMA buffer。这个 sample 不保证对端一定收到 frame；对端接收还要看 DPU OVS 转发、MAC
学习、VF port 状态和目标 host 的 RX path。

## 32. Sample 24: Echo one virtio-net RX packet through VFIO

文件：

```text
userspace_drivers/24_virtio_net_rx_tx_echo.cpp
```

sample 24 把 sample 22 的 RX path 和 sample 23 的 TX path 接在一起。它先给 RX queue
发布 buffer，进入 `DRIVER_OK` 后等待一帧匹配的 RX packet；收到后把 Ethernet frame 拷贝到
TX buffer，目的 MAC 改成收到帧的源 MAC，源 MAC 默认使用 virtio-net DEVICE_CFG 里的 VF
MAC，然后发布一个 TX descriptor 并 notify TX queue。

运行：

```bash
./24_virtio_net_rx_tx_echo_static 0000:c1:00.6
./24_virtio_net_rx_tx_echo_static 0000:c1:00.6 --wait-ms 60000 --yes
./24_virtio_net_rx_tx_echo_static 0000:c1:00.6 --match-ethertype 0x88b5 --dump-bytes 192 --yes
```

默认行为：

```text
RX queue          = 0
TX queue          = 1
RX buffers        = 64 x 2048 bytes
match-ethertype   = 0x88b5
reply source MAC  = virtio-net DEVICE_CFG 里的 MAC
reply destination = received Ethernet source MAC
```

测试时，可以先启动 sample 24，再从另一个 VF 向目标 VF MAC 发一帧 `0x88b5` 的 raw
Ethernet packet。成功时应该看到：

```text
RX used.idx after wait: ... (delta 1)
Selected RX packet for reply:
  reply dst-mac: <sender MAC>
  reply src-mac: <this VF MAC>
Published one TX reply descriptor
TX used.idx after wait: ... (delta 1)
```

这个 sample 仍然只做一次性 datapath 验证：

```text
不 recycle RX descriptor
不持续处理多包
不处理 checksum/offload metadata
不实现 ARP/IP/TCP 协议栈
```

它的定位是证明：userspace 已经能完成一个最小闭环：device DMA 写入 RX buffer，
userspace 消费 RX used entry，再通过 TX queue 把 reply 交还给 device/backend。

## 33. Sample 25: Run a small virtio-net echo loop through VFIO

文件：

```text
userspace_drivers/25_virtio_net_echo_loop.cpp
```

sample 25 把 sample 24 的一次性 echo 扩展成一个有边界的小 loop。它继续使用 RX queue 0
和 TX queue 1，但会追踪 `last_rx_used_idx` / `last_tx_used_idx`，处理多个 RX used entry，
并把完成的 RX descriptor 重新放回 RX avail ring。

运行：

```bash
./25_virtio_net_echo_loop_static 0000:c1:00.6
./25_virtio_net_echo_loop_static 0000:c1:00.6 --run-ms 60000 --max-packets 3 --yes
./25_virtio_net_echo_loop_static 0000:c1:00.6 --tx-buffers 8 --rx-buffers 64 --yes
./25_virtio_net_echo_loop_static 0000:c1:00.6 --max-packets 3 --dump-every-packet --yes
```

配套测试脚本：

```bash
python3 userspace_drivers/25_test_raw_echo_loop.py --count 3 --expect 3
```

默认行为：

```text
RX buffers       = 64
TX buffers       = 8
run-ms           = 30000
max-packets      = 3
match-ethertype  = 0x88b5
dump-every-packet = false
```

loop 内部做的事情：

```text
1. poll RX used.idx，扫描新增的 RX used entries
2. 过滤 ethertype，不匹配的 RX descriptor 也会 recycle
3. 找一个空闲 TX descriptor
4. 把 RX Ethernet frame 拷贝到 TX buffer，交换 src/dst MAC
5. 发布 TX descriptor，notify TX queue
6. 把 RX descriptor 放回 RX avail ring，notify RX queue
7. poll TX used.idx，释放完成的 TX descriptor
8. 到达 --max-packets 或 --run-ms 后 reset device 并 unmap DMA
```

默认只 dump 第一包，避免多包测试时刷屏。需要逐包 dump 时加
`--dump-every-packet`。

这个 sample 开始接近最小 datapath driver 的形状，但仍然有明显边界：

```text
不使用 IRQ/eventfd
不支持多 descriptor chained packet
不处理 checksum/offload metadata
不实现控制队列和协议栈
```

它的定位是证明：userspace 不仅能收一包、发一包，还能维护 ring index 和 descriptor
生命周期，持续处理一个小批量的 packet。

## 34. Sample 26: Drive the echo loop with VFIO IRQ eventfds

文件：

```text
userspace_drivers/26_virtio_net_irq_echo_loop.cpp
```

sample 26 延续 sample 25 的 echo loop，但等待机制从 sleep/poll `used.idx` 改成
VFIO IRQ eventfd。它仍然会在 eventfd 被唤醒后读取 RX/TX used ring；eventfd 只是
wait primitive，不替代 ring index 检查。

运行：

```bash
./26_virtio_net_irq_echo_loop_static 0000:c1:00.6
./26_virtio_net_irq_echo_loop_static 0000:c1:00.6 --run-ms 60000 --max-packets 3 --yes
./26_virtio_net_irq_echo_loop_static 0000:c1:00.6 --rx-vector 0 --tx-vector 1 --dump-every-packet --yes
```

默认行为：

```text
IRQ index       = 2 (MSI-X)
RX MSI-X vector = 0
TX MSI-X vector = 1
RX queue        = 0
TX queue        = 1
RX buffers      = 64
TX buffers      = 8
run-ms          = 30000
max-packets     = 3
match-ethertype = 0x88b5
```

`--yes` 做的事情：

```text
1. 打开 VFIO container/group/device
2. 检查 IRQ index/vector 是否支持 EVENTFD
3. eventfd() 创建 RX/TX interrupt eventfd
4. VFIO_DEVICE_SET_IRQS 把 MSI-X vector 绑定到 eventfd
5. reset device 并协商 FEATURES_OK
6. 写 RX/TX queue_msix_vector
7. 配置 RX/TX queue address
8. enable RX/TX queues
9. 写 DRIVER_OK
10. notify RX queue
11. poll(eventfd)
12. eventfd 被唤醒后扫描 RX/TX used ring
13. echo 匹配的 RX packet，并 recycle RX/TX descriptors
14. 退出时 disable IRQ eventfd binding、reset device、unmap DMA
```

关键点：

```text
interrupt/eventfd 只表示“可能有 queue work”
真正的完成信息仍然以 used.idx / used.ring 为准
```

Gotcha：这个设备的 MSI-X IRQ flags 里有 `NORESIZE`，所以 sample 26 不能像 sample 09
那样对 vector 0 和 vector 1 分别做两次单独的 `VFIO_DEVICE_SET_IRQS` 绑定。它需要一次性
提交完整的 vector fd 数组，例如 `fd_rx, fd_tx, -1, -1`，否则第二次扩大 enabled vector
集合时可能返回 `EINVAL`。

所以 sample 26 的 loop 仍然必须维护：

```text
last_rx_used_idx
last_tx_used_idx
RX descriptor recycle
TX descriptor in-flight/free 状态
```

如果没有 eventfd signal，需要先确认：

```text
VFIO IRQ index 是否是 MSI-X，一般是 2
IRQ count 是否覆盖 --rx-vector / --tx-vector
queue_msix_vector 是否写入并 read back 成功
对端是否真的向目标 VF 发包
device/backend 是否会对对应 queue 产生 interrupt
```

sample 26 的定位是把 sample 25 从“主动 polling”推进到“interrupt-driven wait”。
它仍然不处理 chained descriptors、checksum/offload metadata、control virtqueue、
多队列调度或协议栈。

## 35. Sample 27: Send virtio-net control command through VFIO

文件：

```text
userspace_drivers/27_virtio_net_ctrl_promisc.cpp
```

sample 27 补上 virtio-net 的第三个 queue：control virtqueue。前面的 sample 已经使用
RX queue 0 和 TX queue 1；这个设备 `num_queues=3`，是因为它还提供
`VIRTIO_NET_F_CTRL_VQ`，默认 queue 2 就是 control queue。

本 sample 默认发送：

```text
class = VIRTIO_NET_CTRL_RX
cmd   = VIRTIO_NET_CTRL_RX_PROMISC
data  = 1
```

也就是临时打开 promiscuous mode。退出前会 reset device，所以这个模式不会被 sample
长期留在设备上。

运行：

```bash
./27_virtio_net_ctrl_promisc_static 0000:c1:00.6
./27_virtio_net_ctrl_promisc_static 0000:c1:00.6 --promisc on --yes
./27_virtio_net_ctrl_promisc_static 0000:c1:00.6 --queue-size 128 --promisc off --yes
```

`--yes` 做的事情：

```text
1. 打开 VFIO container/group/device
2. 打开 PCI Memory Space / Bus Master
3. reset device
4. 协商 FEATURES_OK，并额外接受 VIRTIO_NET_F_CTRL_VQ / VIRTIO_NET_F_CTRL_RX
5. 配置 RX queue 0、TX queue 1、control queue 2
6. 发布 RX buffers，避免 queue pair 处于明显不完整状态
7. 在 control queue 中发布 3 个 descriptor 组成的 chain：
   readable control header -> readable state byte -> writable ACK byte
8. enable RX/TX/control queues
9. 写 DRIVER_OK
10. notify RX queue
11. notify control queue
12. 等待 control used.idx 前进，读取 ACK byte
13. reset device
14. 恢复 PCI command register
15. VFIO_IOMMU_UNMAP_DMA
```

control virtqueue 的 descriptor chain 很像真实 driver 里常见的 scatter-gather request：

```text
desc 0: device-readable struct virtio_net_ctrl_hdr
desc 1: device-readable command data, 这里是 1 byte on/off state
desc 2: device-writable ACK byte
```

如果 ACK 是 `0x00`，表示 `VIRTIO_NET_OK`；如果是 `0x01`，表示 `VIRTIO_NET_ERR`。
如果 control used ring 没有前进，说明 device 没有消费这个 control command，需要检查：

```text
是否协商了 VIRTIO_NET_F_CTRL_VQ
是否协商了 VIRTIO_NET_F_CTRL_RX
control queue index 是否正确，当前设备默认是 2
control queue 是否 enable
notify offset/value 是否正确
DRIVER_OK 后 device_status 是否保持不带 NEEDS_RESET
```

sample 27 的定位是演示 control virtqueue 的 request/ACK 模型。它仍然不实现完整的 MAC
filter table、VLAN table、multiqueue 配置、offload 控制或长期运行的 control-plane。

## 36. Sample 28: Observe virtio-net promiscuous RX behavior

文件：

```text
userspace_drivers/28_virtio_net_promisc_observe.cpp
```

sample 28 在 sample 27 的基础上继续往前走一步：control command ACK 成功后不马上
reset，而是在 promisc 状态保持期间继续观察 RX queue。它的目标是验证 `RX_PROMISC=on/off`
是否真的改变了 VF 的 RX 过滤行为。

运行：

```bash
./28_virtio_net_promisc_observe_static 0000:c1:00.6
./28_virtio_net_promisc_observe_static 0000:c1:00.6 --promisc on --wait-ms 60000 --yes
./28_virtio_net_promisc_observe_static 0000:c1:00.6 --promisc off --wait-ms 10000 --yes
```

测试方式：

```text
1. 先启动 sample 28，promisc=on，等待 RX
2. 从另一个 VF 发一帧 ethertype=0x88b5 的 raw Ethernet packet
3. 目的 MAC 故意不要写 VF#3 的 MAC fe:bf:30:01:30:04
4. 如果 backend/OVS 把该帧送到 VF#3 representor，promisc=on 时 sample 应该能看到它
5. 再用 promisc=off 重复同样发送，通常不应该看到这类非本机 MAC 的 unicast
```

接收端：

```bash
./28_virtio_net_promisc_observe_static 0000:c1:00.6 --promisc on --wait-ms 60000 --stop-after 1 --yes
```

发送端可以用 repo 里的 raw frame 测试脚本。这里从 VF#0 `ens6f1v0` 发一帧目的 MAC
不是 VF#3 MAC 的 `0x88b5` unknown-unicast frame：

```bash
python 22_test_raw.py \
  --iface ens6f1v0 \
  --src-mac fe:bf:30:01:30:01 \
  --dst-mac 02:00:00:00:28:01 \
  --ethertype 0x88b5 \
  --payload-hex 70726f6d6973632d746573742d756e6b6e6f7766e2d647374000102030405060708090a0b0c0d0e0f \
  --count 3 \
  --interval 0.1
```

DPU 侧可以用 representor 确认 frame 确实被送到 VF#3：

```bash
tcpdump -eni en3f0pf0sf3003 'ether proto 0x88b5 or ether host 02:00:00:00:28:01'
```

对照测试：

```bash
./28_virtio_net_promisc_observe_static 0000:c1:00.6 --promisc off --wait-ms 10000 --stop-after 1 --yes
```

`--yes` 做的事情：

```text
1. 打开 VFIO container/group/device
2. 打开 PCI Memory Space / Bus Master
3. reset device
4. 协商 FEATURES_OK，并接受 VIRTIO_NET_F_CTRL_VQ / VIRTIO_NET_F_CTRL_RX
5. 配置 RX queue 0、TX queue 1、control queue 2
6. 发布 RX buffers
7. 发布 RX_PROMISC control descriptor chain
8. enable RX/TX/control queues
9. 写 DRIVER_OK
10. notify RX queue
11. notify control queue
12. 等待 control ACK 0x00
13. 在 promisc 状态保持期间继续 poll RX used.idx
14. 对收到的 packet 打印 destination/source MAC、ethertype、是否匹配 VF MAC
15. recycle RX descriptor，保持 RX queue 可以继续收包
16. reset device，恢复 PCI command，unmap DMA
```

通过标准：

```text
control ACK byte: 0x00 (OK)
Observed RX packet ...
dst matches VF MAC: no
this is the expected promisc-path observation
```

如果 promisc=on 也看不到非本机 MAC 的包，不一定是 sample 失败；可能是 DPU/OVS 没有把
该 frame 送到 VF#3 的 backend port。先在 DPU representor 上用 tcpdump 确认该 frame
是否经过 `en3f0pf0sf3003`，再判断 virtio-net RX filter。

sample 28 仍然不是完整 sniffing driver：它只演示 control-plane 改 RX filter 后的
observable datapath 行为，不处理 offload、multi-buffer packet、VLAN filter、MAC table
或长期运行的 packet capture。

## 37. Sample 29: Set virtio-net MAC address through VFIO

文件：

```text
userspace_drivers/29_virtio_net_ctrl_mac_addr.cpp
```

sample 29 继续使用 control virtqueue，但命令从 sample 27/28 的 `RX_PROMISC` 换成
`MAC_ADDR_SET`。它协商 `VIRTIO_NET_F_CTRL_MAC_ADDR`，通过 control queue 临时设置 VF 的
MAC 地址，然后保持 device running，观察发往这个临时 MAC 的 RX packet 是否进入 used
ring。

默认临时 MAC：

```text
02:00:00:00:29:01
```

运行：

```bash
./29_virtio_net_ctrl_mac_addr_static 0000:c1:00.6
./29_virtio_net_ctrl_mac_addr_static 0000:c1:00.6 --new-mac 02:00:00:00:29:01 --wait-ms 60000 --yes
./29_virtio_net_ctrl_mac_addr_static 0000:c1:00.6 --new-mac 02:00:00:00:29:02 --accept-any-ethertype --yes
```

接收端：

```bash
./29_virtio_net_ctrl_mac_addr_static 0000:c1:00.6 --new-mac 02:00:00:00:29:01 --wait-ms 60000 --stop-after 1 --yes
```

发送端从 VF#0 `ens6f1v0` 发一帧目的 MAC 为临时 MAC 的 `0x88b5` frame：

```bash
python 22_test_raw.py \
  --iface ens6f1v0 \
  --src-mac fe:bf:30:01:30:01 \
  --dst-mac 02:00:00:00:29:01 \
  --ethertype 0x88b5 \
  --payload-hex 6d61632d616464722d7365742d746573742d30303239000102030405060708090a0b0c0d0e0f \
  --count 3 \
  --interval 0.1
```

DPU 侧可以用 representor 确认 frame 经过 VF#3：

```bash
tcpdump -eni en3f0pf0sf3003 'ether proto 0x88b5 or ether host 02:00:00:00:29:01'
```

`--yes` 做的事情：

```text
1. 打开 VFIO container/group/device
2. 打开 PCI Memory Space / Bus Master
3. reset device
4. 协商 FEATURES_OK，并接受 VIRTIO_NET_F_CTRL_VQ / VIRTIO_NET_F_CTRL_MAC_ADDR
5. 配置 RX queue 0、TX queue 1、control queue 2
6. 发布 RX buffers
7. 发布 MAC_ADDR_SET control descriptor chain：
   control header -> 6-byte MAC data -> writable ACK byte
8. enable RX/TX/control queues
9. 写 DRIVER_OK
10. notify RX queue
11. notify control queue
12. 等待 control ACK 0x00
13. 在临时 MAC 生效期间继续 poll RX used.idx
14. 对收到的 packet 打印 destination/source MAC、是否匹配原始 MAC/临时 MAC
15. recycle RX descriptor，保持 RX queue 可以继续收包
16. reset device，恢复 PCI command，unmap DMA
```

通过标准：

```text
control ACK byte: 0x00 (OK)
Observed RX packet ...
dst matches temporary MAC: yes
this is the expected MAC_ADDR_SET-path observation
```

如果 control ACK 是 OK，但发往临时 MAC 的包没有进入 RX，需要先在 DPU representor 上确认
frame 是否真的经过 VF#3 backend port。这个 command 改的是 virtio-net device/filter
侧状态，不会自动修改 OVS bridge 的转发规则。

sample 29 退出前会 reset device，所以临时 MAC 不会长期保留。它仍然不是完整 netdev
driver：不处理 Linux netdev 地址同步、MAC table、多播过滤、VLAN filter 或长期 control
plane。

## 38. Sample 30: Set virtio-net MAC filter table through VFIO

文件：

```text
userspace_drivers/30_virtio_net_ctrl_mac_table.cpp
```

sample 30 继续练习 virtio-net control virtqueue，但这次不改 primary MAC。它发送
`VIRTIO_NET_CTRL_MAC_TABLE_SET`，把一个额外的 unicast MAC 写入设备的 MAC filter
table，然后保持 device running，观察发往这个 filter MAC 的 RX packet 是否进入 used
ring。

默认 filter MAC：

```text
02:00:00:00:30:01
```

运行：

```bash
./30_virtio_net_ctrl_mac_table_static 0000:c1:00.6
./30_virtio_net_ctrl_mac_table_static 0000:c1:00.6 --filter-mac 02:00:00:00:30:01 --wait-ms 60000 --yes
./30_virtio_net_ctrl_mac_table_static 0000:c1:00.6 --filter-mac 02:00:00:00:30:02 --accept-any-ethertype --yes
```

接收端：

```bash
./30_virtio_net_ctrl_mac_table_static 0000:c1:00.6 --filter-mac 02:00:00:00:30:01 --wait-ms 60000 --stop-after 1 --yes
```

发送端从 VF#0 `ens6f1v0` 发一帧目的 MAC 为 filter MAC 的 `0x88b5` frame：

```bash
python 22_test_raw.py \
  --iface ens6f1v0 \
  --src-mac fe:bf:30:01:30:01 \
  --dst-mac 02:00:00:00:30:01 \
  --ethertype 0x88b5 \
  --payload-hex 6d61632d7461626c652d7365742d746573742d30303330000102030405060708090a0b0c0d0e0f \
  --count 3 \
  --interval 0.1
```

DPU 侧可以用 representor 确认 frame 经过 VF#3：

```bash
tcpdump -eni en3f0pf0sf3003 'ether proto 0x88b5 or ether host 02:00:00:00:30:01'
```

`--yes` 做的事情：

```text
1. 打开 VFIO container/group/device
2. 打开 PCI Memory Space / Bus Master
3. reset device
4. 协商 FEATURES_OK，并接受 VIRTIO_NET_F_CTRL_VQ / VIRTIO_NET_F_CTRL_RX
5. 配置 RX queue 0、TX queue 1、control queue 2
6. 发布 RX buffers
7. 发布 MAC_TABLE_SET control descriptor chain：
   control header -> unicast table -> multicast table -> writable ACK byte
8. unicast table 内容为 count=1 + filter MAC
9. multicast table 内容为 count=0
10. enable RX/TX/control queues
11. 写 DRIVER_OK
12. notify RX queue
13. notify control queue
14. 等待 control ACK 0x00
15. 在 filter table 生效期间继续 poll RX used.idx
16. 对收到的 packet 打印 destination/source MAC、是否匹配 config MAC/filter MAC
17. recycle RX descriptor，保持 RX queue 可以继续收包
18. reset device，恢复 PCI command，unmap DMA
```

通过标准：

```text
control ACK byte: 0x00 (OK)
Observed RX packet ...
dst matches filter MAC: yes
this is the expected MAC_TABLE_SET-path observation
```

和 sample 29 的区别：

```text
sample 29: MAC_ADDR_SET，改 primary MAC，feature 是 VIRTIO_NET_F_CTRL_MAC_ADDR
sample 30: MAC_TABLE_SET，改 RX filter table，feature 是 VIRTIO_NET_F_CTRL_RX
```

virtio spec 说明 MAC table filtering 可以是 non-perfect filtering，也就是说设备可能因为
backend 资源或实现策略接收 filter table 之外的 packet。因此 sample 30 的重点是验证“发往
filter MAC 的帧能进来”，不是证明“其他 MAC 的帧一定进不来”。

sample 30 退出前会 reset device，所以 filter table 不会长期保留。它仍然不是完整 netdev
driver：不处理 Linux netdev unicast list 同步、多播列表管理、VLAN filter 或长期 control
plane。

## 39. Refactor 结论：samples 21-30 的代码结构

本轮 refactor 的目标不是把 tutorial 改成一个 framework，而是把 21-30 这组已经进入
virtio-net datapath 的 sample 从“大量重复实现”收敛成“每个 sample 只展示新增概念”。

完成后的分工：

```text
vfio_utils.hpp
    UniqueFd / AnonymousBuffer / MappedRegion / DmaMapping
    VFIO container/group/device 打开
    IOMMU group 和 region info 打印
    PCI config region 读写
    VFIO IRQ info / eventfd binding helper
    virtio PCI capability 解析
    COMMON_CFG / NOTIFY_CFG / DEVICE_CFG mmap helper

virtio_net_vfio.hpp
    virtio-net feature negotiation helper
    QueueSelectionGuard / DeviceResetGuard / PciCommandGuard
    split vring layout 和 DMA memory layout
    RX descriptor 发布、RX descriptor recycle
    TX descriptor 发布
    queue address programming、queue_enable、notify、DRIVER_OK
    virtio-net config 读取和打印
    RX used entry / packet dump / echo reply helper
```

21-30 现在的 sample 文件只保留：

```text
Options
usage()
dry_run()
本 sample 的 run_*() 主流程
main() 参数解析
```

各 sample 保留的边界：

```text
21  只发布 RX buffers，使用 transport-only minimal features；
    不读 DEVICE_CFG，不引入 MAC/STATUS，保持“给 device buffer 后是否还能运行”的教学点。

22  在 21 基础上读取 DEVICE_CFG，接受 MAC/STATUS，等待 RX used ring 并 dump packet。

23  在 22 的 queue setup 基础上发布一个 TX descriptor，观察 TX completion。

24  把 22 的 RX path 和 23 的 TX path 接起来，做一次 RX->TX echo。

25  在 24 基础上维护 RX/TX descriptor 生命周期，做有限个 packet 的 echo loop。

26  在 25 基础上把 wait primitive 换成 VFIO IRQ eventfd；
    eventfd 唤醒后仍然以 used ring 作为真实完成来源。

27  补上 control virtqueue，发送 RX_PROMISC control command；
    展示 control header / command data / ACK byte 这类 descriptor chain。

28  在 RX_PROMISC command ACK 后保持 device running；
    观察非 VF MAC 目的地址的 packet 是否进入 RX used ring。

29  发送 MAC_ADDR_SET control command；
    观察发往临时 MAC 的 packet 是否进入 RX used ring。

30  发送 MAC_TABLE_SET control command；
    观察发往额外 unicast filter MAC 的 packet 是否进入 RX used ring。
```

为什么 `virtio_net_vfio.hpp` 里的函数是 `inline`：

```text
这是 header-only helper，被 21/22/23/24/25/26 多个独立 executable 同时 include。
函数定义放在 header 里时必须用 inline，否则链接多个 sample 时会出现 multiple definition。
这里的 inline 是链接语义，不是为了强迫编译器内联优化。
```

如果后续 helper 继续增长，可以把 `virtio_net_vfio.hpp` 拆成：

```text
virtio_net_vfio.hpp   declarations / small templates
virtio_net_vfio.cpp   function definitions
libvirtio_net_vfio    CMake static library
```

暂时保留 header-only 的原因是：当前 samples 都是教学用独立 executable，header-only
可以减少 CMake target 复杂度，也方便阅读时从 sample 直接跳到 helper 实现。

本轮没有重构 11-20。原因是 11-20 还处于逐步展开 VFIO / virtio PCI 基础机制的阶段，
重复代码虽然多，但有助于每个 sample 单独说明“这一步新增了什么”。等这部分稳定后，可以
再考虑只抽出最底层的安全/RAII helper，而不要把状态机和 queue 操作过早隐藏起来。

## 40. 最小心智模型

把现在学到的内容压缩成一张图：

```text
/sys/bus/pci/devices/<BDF>/
    |
    +-- vendor/device/class
    |       设备是谁、是什么类型
    |
    +-- driver
    |       当前谁在控制设备
    |
    +-- resource
    |       BAR start/end/flags
    |
    +-- config
    |       PCI 标准 header 和 capability list
    |
    +-- resourceN
            memory BAR 可 mmap
            I/O BAR 不可 mmap
```

driver 开发的第一原则：

```text
先识别设备，再理解资源，再读寄存器，最后才考虑写寄存器。
```
