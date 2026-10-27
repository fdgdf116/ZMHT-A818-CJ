# 上位机退出时的板端停流验证

改动范围：TYPE7 关闭、DMA 异常停流、线程退出确认，以及 TYPE5 重新启动。
图像处理算法、编码参数、DMA 地址、分包格式和正常命令格式保持原样。
初始化阶段的工作线程等待 TYPE5；FPGA 使能后才开始处理和发送。

## 行为

- TYPE7 先关闭软件任务入口并通知编码退出，等待在途任务结束，确认编码、OSD、拼接线程退出，然后丢弃队列、复位并执行原有资源清理。
- DMA 读写失败或短传输也会关闭入口，由原 FIFO 控制线程串行执行相同清理流程，不与配置指令并发释放资源。
- DMA 超时自动停流默认关闭（`STREAM_DMA_WATCHDOG_ENABLED=0`），不再因为传输超过 1 秒主动停流。设为 1 可恢复 FIFO 控制循环的 DMA 耗时检查，监测阈值为 1000 ms。等待在途任务和各线程退出的超时仍为 2000 ms。定义位于 `src/common/stream_lifecycle.h`，使用单调时钟，不受 TYPE1 校时影响。
- 监测阈值不是驱动超时，检查也会受控制指令处理时间影响。
- 任一停止步骤尚未完成：保持 `STREAM_STOPPING`，跳过复位、队列重置和资源释放；配置和启动返回 74。TYPE7 本次等待超时也返回 74，但可重试；控制线程每 100 ms 非阻塞检查任务和线程回收，完成后自动清理并进入 `STOPPED`。TYPE6 仍可使用。显式 TYPE7 的各等待阶段仍使用 2000 ms 上限，并非整个指令的总上限。
- 成功停止后，按原有初始化、配置、TYPE5 流程重新开始，不发送旧队列内容。

## 驱动边界

本仓库没有 XDMA 内核驱动源码；现有 ioctl 头文件没有普通 DMA 传输中止接口。应用不会用强杀线程、关闭 fd 或强制释放内存来模拟驱动中止。

如果驱动一直阻塞在 `read/write`，应用可以停止新任务并让关闭处理超时返回，但不能让该内核调用强制结束，也不能保证免重启恢复。停止中保留缓冲区；DMA 迟到返回且线程退出后可自动继续清理。若 DMA 永不返回则保持停止中，不能强行恢复。完整自动恢复仍需要驱动提供有界等待和正确中止传输的能力。

## 本机测试

在项目根目录执行：

```sh
gcc -D_GNU_SOURCE -Wall -Wextra -Werror -pthread \
  -fsanitize=address,undefined -g -I rk_a818_prj/src/common \
  rk_a818_prj/tests/stream_lifecycle_test.c \
  rk_a818_prj/src/common/stream_lifecycle.c -o /tmp/a818-stream-test
ASAN_OPTIONS=detect_leaks=0 /tmp/a818-stream-test
cmake -S rk_a818_prj -B /tmp/a818-stop-build
cmake --build /tmp/a818-stop-build -j4
```

LeakSanitizer 在当前受跟踪的执行环境不支持运行，因此上述命令仅禁用泄漏检测，保留 AddressSanitizer 和 UndefinedBehaviorSanitizer。
线程测试覆盖：在途 DMA 时关闭、重复关闭、拒绝新任务、等待与 join 超时、DMA 返回后继续清理、20 次停止再启动、无关闭指令的超时停流、作用域退出的计数释放，以及故障态禁止恢复。
这些测试不模拟真实 FPGA/MPP/DRM，不代替板端验证。

## 板端复测

1. 单路和多路分别确认原有分辨率、帧率、码流及 DP 显示正常。
2. 发送过程中正常退出上位机：确认关闭完成后不再持续打印写失败或队列满；按原有流程重新启动能恢复图像。
3. 发送过程中强杀上位机：若下游停止导致 DMA 错误或阻塞，确认触发自动停流。驱动能返回时应清理完成；一直不返回时应报 `stream stop pending`，后续启动返回 74。看门狗默认关闭，只有 DMA 返回错误或 TYPE7 才会发起停止；仅仅上位机消失不会被主动检测。
4. 连续重复开关至少 100 次，观察线程数、内存是否持续增长，是否出现旧帧、异常画面或重复释放。
5. 分别在四路编码、拼接/OSD 和 DP 开启时重复以上步骤；关闭后 TYPE6 应仍可应答。
6. 保存应用日志和 `dmesg`。写失败日志现在记录原始 errno 和耗时，便于进一步定位驱动/FPGA 问题。

## DMA 看门狗计时竞态回归

看门狗必须在生命周期锁内读取当前时间，避免 DMA 开始时间晚于检查时间时，无符号相减下溢而误停采。
下面的测试确定性插入该时序，同时检查 999 ms 不超时、1000 ms 正常触发停流：

```sh
gcc -D_GNU_SOURCE -Wall -Wextra -Werror -pthread \
  -I rk_a818_prj/src/common \
  rk_a818_prj/tests/stream_watchdog_race_test.c -o /tmp/a818-watchdog-race-test
/tmp/a818-watchdog-race-test
```

## 视频 VFIFO 写阻塞诊断

`pcie_engine.c` 定义 FPGA2RK 数量寄存器偏移 `0x100` 和 RK2FPGA 数量寄存器偏移 `0x104`，相对于 `pcie_reg`（映射偏移 `0x10000`）。用户已确认 `0x104` 为已用量，单位 32 字节，总容量 268435456 字节（256 MiB）。当前依据此定义控制 H2C 发送；仓库仍无 FPGA RTL 和 XDMA 驱动源码。

每次 H2C 写入前缓存两个寄存器的原始值。仅在 `dma_watchdog`、`stop_pending`、`write_failed` 时打印 `[VFIFO]`，包含写序号、写入字节数、写前和当前寄存器值以及单调时钟毫秒。`write_active` 表示应用仍在跟踪本次调用（失败打印发生在清除此标记之前），不是硬件忙状态；未发生写入时缓存字段为零。看门狗也监测读取方向，因此应结合 H2C 错误日志判断故障方向。寄存器分别读取，并非硬件原子快照。

板端复测时，在另一终端提前执行 `dmesg -w > /tmp/a818-kernel.log`，然后运行诊断版本并保存完整应用输出，例如 `./rk_a818_prj > /tmp/a818-app.log 2>&1`。若板端不支持 `dmesg -w`，在故障后执行 `dmesg > /tmp/a818-kernel.log`。不要清空内核日志。保留完整日志，利用 `mono_ms` 与默认内核启动后时间戳大致关联，关注 XDMA/H2C、超时、传输完成、中断和 PCIe 错误。诊断采样会增加 MMIO 访问；若 BAR 读取本身阻塞，用户态诊断无法保证按时返回。

`0x104` 已用量按用户提供的定义换算；寄存器更新时间、其他写入源、读取副作用及 FIFO 满时 AXI 背压行为仍需硬件核对。容量预检查不能中断已经阻塞的 DMA，也不能代替驱动超时处理。

## 屏蔽 DMA 看门狗

当前默认关闭 1 秒超时自动停流。驱动返回错误或短传输时仍请求停流；手动 TYPE7 仍执行原有停流流程。若驱动一直不返回，发送线程可能一直等待。若阻塞期间发送 TYPE7，等待超时返回 74 并保持停止中，后台继续重试；本开关不修改驱动超时。上述超时停流及 `dma_watchdog` 板端验证需启用开关后执行；`write_failed`、`stop_pending` 诊断仍有效。

## 停止恢复复测（当前流程）

1. 运行中让 H2C write 阻塞，发送 TYPE7：应回复 74，打印 `stream stop pending: stage=1`；不得复位或释放资源。继续 TYPE6 应可回复，TYPE4/TYPE5 应返回 74。
2. 让 DMA 迟到返回：无需新的停止指令，后台应完成清理，打印 `stream stop complete: STOPPED`。stage 1/2/3/4 分别是任务计数、编码 join、OSD join、拼接 join；相同阶段的后台等待不反复刷日志。
3. 停止成功后重复 TYPE7 应直接成功，不重复清理。TYPE5 在没有重新配置 RK/PC 使能时拒绝启动。按原有配置流程配置后 TYPE5 启动，检查新帧和图像。
4. 让一个线程延迟退出：已 join 的线程不重复 join，其余线程可在后续轮询回收；全部结束前不执行公共队列/通道资源清理。
5. DMA 自行返回错误（不发 TYPE7）也应自动进入相同流程。重复以上过程至少 100 次，保存应用和内核日志。

没有新增心跳命令或驱动取消 ioctl；需要上位机心跳协议和驱动实现才能覆盖“上位机消失但 DMA 没报错”或“DMA 永远不返回”。仍使用现有 `pcie_fpga_reg_reset()` 的复位寄存器序列；它是否确实清空全部硬件 VFIFO，必须结合 FPGA RTL/板端验证。DMA 返回代表驱动已放弃缓冲区的假设也需驱动核对。

## RK → FPGA 容量限流

在唯一的编码 DMA 发送线程中，每个固定 32832 字节包出队前读取 `0x104`。使用 64 位运算计算 `used=count*32`、`free=268435456-used`；已用量大于总容量时按剩余 0 处理，防止减法下溢。仅当本包发送后剩余量不少于 33554432 字节（32 MiB）才出队并发送。

空间不足时不出队、不调用 DMA、不请求停止，退出任务 guard 后等待 1 ms 再查；空间恢复自动发送。容量日志每秒至多一条。此限流只约束 H2C，未增加编码端背压，长时间暂停仍可能导致既有软件队列满/丢帧，不能保证无限期保留所有视频数据。

边界测试：

```sh
gcc -Wall -Wextra -Werror -fsanitize=address,undefined -I rk_a818_prj/src/common rk_a818_prj/tests/vfifo_capacity_test.c -o /tmp/a818-vfifo-capacity-test
ASAN_OPTIONS=detect_leaks=0 /tmp/a818-vfifo-capacity-test
```

板端验证剩余空间达到阈值时出现 `[VFIFO] pause DMA write`，H2C 不再提交；消费数据恢复空间后继续发送。在暂停期间发送 TYPE7，应能完成停止。还需验证计数更新及时性及是否存在其他并发写入源；预检查不是硬件空间预留操作。
