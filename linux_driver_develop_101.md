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
```

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
- [25. 最小心智模型](#25-最小心智模型)

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

## 25. 最小心智模型

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
