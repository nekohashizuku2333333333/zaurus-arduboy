# Zaurus SL-C750 Arduboy 模拟器加速研究

> 环境基线取自 `n.7z`（murphytalk-pinyin-fix）的 `doc/env.md`：GCC 2.95.3 + Qt/E 2.3.2（C++ 前端 ABI）、GCC 3.4.6-xscale（C 核心）、`-mcpu=xscale -mhard-float`、原厂 Sharp ROM（内核 2.4.18-rmk7-pxa3）。
> 代码基线取自 `arduboyemuforzaurus.zip` 的 HEAD 提交 `9d45e3c "CPU bottleneck"`。

## 0. 结论先行

到目前为止（`stage2.md` + `CPU bottleneck` 提交）所有优化都集中在**前端与显示侧**：
RGB565 缓存 QImage、`QDirectPainter` 直写、脏帧跳过、30fps 上限、按实时经过时间驱动周期数、按目标 `avr->cycle` 停步。这些已经把「画」的成本压到很低。

**但真正的瓶颈在 AVR 解释器核心本身——`third_party/simavr` 至今是未改动的原版**。
它以「一次 `avr_run()` 执行一条指令」的方式运行，且**每执行一条指令就调用一次 `avr_cycle_timer_process()`**，并对每次寄存器/内存访问做多重分支与 IRQ 派发。这就是为什么必须保留 8MHz「Light」模式：C750 跑不满 16MHz 的原版 simavr。

下面的加速手段按「投入产出比」从高到低排序。Tier 1/2 是在不换架构、保持 simavr 精度的前提下能拿到的最大收益；Tier 5 才是换架构。

---

## 1. 硬件预算：为什么现在跑不满

SL-C750 = Intel PXA255（XScale，ARMv5TE），约 400MHz，**顺序单发射、无 VFP/FPU**（浮点走内核 FPA 仿真，极慢），32KB I-Cache + 32KB D-Cache，**无 L2**。

Arduboy = ATmega32u4 @ 16MHz。要实时全速，需要每秒模拟约 1600 万 AVR 周期。原版 simavr 走大 `switch` 解释器，一条被模拟指令在宿主上的开销粗估在数十条 ARM 指令量级（取指、解码、取操作数、执行、算标志、周期定时器处理）。

粗略量级：`16M 指令/秒 × ~40–80 宿主指令/条 ≈ 6–13 亿宿主指令/秒`。
一颗 400MHz 顺序核在有 Cache miss 的现实条件下达不到这个数。所以：

- 全速 16MHz 在原版 simavr 上是「勉强/达不到」——与现状（需要 Light 8MHz）吻合。
- 目标应是**把每条被模拟指令的宿主开销砍掉 2–4 倍**，让 16MHz 落进预算，或至少让 12MHz 稳定。

---

## 2. Tier 1 — 解释器主循环开销（最高收益、最低风险）

### 2.1 把周期定时器处理移出「每指令」路径 ★最优先

现状 `sim/sim_avr.c: avr_callback_run_raw()`：

```c
if (avr->state == cpu_Running)
    avr->pc = avr_run_one(avr);      // 执行 1 条
avr_cycle_count_t sleep = avr_cycle_timer_process(avr);   // 每条都调用！
```

而前端 `arduboy_core.c: zaurus_arduboy_run_cycles()` 又通过 `avr_run()`→`avr->run()`（**函数指针间接调用**）反复进入。于是每一条被模拟指令都付出：

1. 一次经 `avr->run` 的间接调用；
2. 一次 `avr_cycle_timer_process()` 调用 + 定时器池遍历（`sim/sim_cycle_timers.c`，即使没有到期定时器也要比较 `when > avr->cycle`）。

`avr_cycle_timer_process()` 本身返回的是「距下一个定时器还有多少周期」。可以据此**成批执行**：在 `arduboy_core` 里写一个自定义运行函数，直接循环 `avr_run_one()`，只有到达下一个定时器到期点时才处理定时器：

```c
// 伪代码：批处理内核，绕开 per-instruction 的 avr->run 与 timer 处理
int zaurus_arduboy_run_cycles(zaurus_arduboy_t *emu, unsigned cycles) {
    avr_t *avr = emu->avr;
    avr_cycle_count_t until = avr->cycle + cycles;
    while (avr->cycle < until && avr->state == cpu_Running) {
        // 下一个定时器到期点（没有则用 until）
        avr_cycle_count_t next = avr->cycle_timers.timer
                               ? avr->cycle_timers.timer->when : until;
        if (next > until) next = until;
        // 紧内层循环：不再每条调用 timer 处理，也不走函数指针
        while (avr->cycle < next && avr->state == cpu_Running)
            avr->pc = avr_run_one(avr);
        avr_cycle_timer_process(avr);            // 只在批边界处理
        if (avr->interrupt_state)
            avr_service_interrupts(avr);         // 中断也在批边界服务
    }
    // …保持原有 ssd1306 dirty → copy_frame 逻辑…
}
```

要点与风险：
- **精度**：中断与定时器只在「到下一个到期点」时服务，而到期点本来就是定时器精度边界，所以对 Timer/SPI 事件仍是周期精确的；唯一放松的是「外部异步中断在批中途注入」，对 Arduboy（按键经 IOPORT，SPI 经周期定时器）没有可感知影响。
- SPI 在 `/4` 预分频下每字节 32 周期就有一个到期点，天然把批切得很短，因此显示上传阶段批很小、其余纯计算阶段批很大——正好。
- 这一步**不改 simavr 源码**（只在 `arduboy_core.c` 里换一个运行函数 + 少量 `extern`），风险最低。预期收益：省掉每指令一次间接调用 + 一次定时器池遍历，**约 1.2–1.6×**。

### 2.2 用 `__builtin_expect` 标注热分支

`sim_core.c` 里 `if (unlikely(avr->pc >= avr->flashend))`、`state == cpu_Running`、`addr < 32` 等分支方向高度可预测。XScale 分支预测弱，给编译器提示能减少流水线气泡。低成本、小收益，随手做。

---

## 3. Tier 2 — 解码成本（switch 解释器 → 预解码 / 直接线程化）

### 3.1 预解码缓存（predecode）★核心级最大单项收益

`avr_run_one()`（`sim/sim_core.c`，1656 行）每次执行都重新走
`switch (opcode & 0xf000) → switch(...) → …` 的多层嵌套来还原指令并抽取操作数。

关键事实：**ATmega32u4 flash 仅 32KB = 16K 个指令字，加载后基本静态**。可以在 `load_hex` 之后做一遍**预解码**，把每个 flash 字翻译成一张紧凑表：

```c
typedef struct {
    uint8_t  handler;   // 指令种类编号（枚举）
    uint8_t  d, r;      // 已抽取的寄存器号
    uint16_t k;         // 已抽取的立即数/偏移
} decoded_t;
decoded_t dec[16*1024];   // 每条 6 字节 → 96KB；压到 4 字节 → 64KB
```

热循环变成：`d = dec[pc>>1]; switch(d.handler){…}`（或配合 3.2 用计算 goto）。
省掉了每条指令的操作数位抽取与多层分支判定，这是 switch 解释器→预解码的经典 **1.5–3×** 收益。

注意：
- **Cache**：XScale 只有 32KB D-Cache 无 L2，64KB 的解码表放不进 L1。要把每条压到 **4 字节**（handler+两个寄存器打包+16 位 k），并保证访问顺序性（PC 多为顺序），XScale 的 `PLD` 预取下一条可进一步遮掩延迟。
- **自修改代码 / SPM**：Arduboy 游戏几乎不自写 flash；只要在 `avr_flash` 的 SPM 写入路径上把对应 `dec[]` 项标记失效（或整表重解码）即可，触发极少。
- 工作量中等，GCC 3.4.6 完全支持。

### 3.2 计算 goto（labels-as-values）直接线程化

把大 `switch` 改成 `goto *handlers[d.handler];`（GCC 扩展，2.95/3.4 均支持）。
省掉 switch 的范围检查与跳表间接，并让每个 handler 结尾直接跳到下一条的分发，改善取指局部性。**与 3.1 叠加**即为「直接线程化解释器」，是不上 JIT 的性能天花板。中等工作量，建议在 3.1 稳定后再叠。

### 3.3 热点 opcode 排序

按 Arduboy 实际指令频率把 `LD/ST/MOV/ADD/SUB/CP/分支/RJMP/RCALL` 放到分发最前。现状已按 `opcode & 0xf000` 先分，改动小、收益小，顺手做。

---

## 4. Tier 3 — 内存访问与 I/O 派发开销

### 4.1 寄存器/SRAM 快速路径

每次内存访问经 `_avr_get_ram`/`_avr_set_ram` → `avr_core_watch_read/write`（`sim_core.c`）。后者对**每次访问**都做：

- `addr > avr->ramend` 的回绕检查；
- 写路径还调用 `_call_register_irqs` + `_call_sram_irqs`（遍历已注册的 SRAM IRQ）。

而绝大多数访问是 r0–r31 通用寄存器。建议在热路径先加一条最短分支：

```c
if (likely(r < 32)) { avr->data[r] = v; return; }   // 直接命中，绕开全部 I/O/IRQ/watch 机制
```

读同理。`avr->gdb` 的 watchpoint 已被 `if (avr->gdb)` 守卫（设备上 `gdb==NULL`），所以它只剩一次可预测分支，不是调用——**不必为它专门编译裁剪**，但把它移出寄存器快速路径仍有微益。

### 4.2 削减 I/O 写的「每位 IRQ 风暴」

`_avr_set_r()` 对带 IRQ 的 I/O 寄存器写，会 `AVR_IOMEM_IRQ_ALL` + **循环 8 次** `avr_raise_irq` 逐位派发：

```c
if (avr->io[io].irq) {
    avr_raise_irq(avr->io[io].irq + AVR_IOMEM_IRQ_ALL, v);
    for (int i = 0; i < 8; i++)
        avr_raise_irq(avr->io[io].irq + i, (v >> i) & 1);
}
```

Arduboy 对 PORTB/PORTD/PORTF 的写很频繁（SPI 的 SCK/MOSI、OLED 的 CS/DC/RST、按键）。而消费者主要是 SSD1306/SPI 的**字节级** SPI IRQ，并不订阅端口的逐位 IRQ。可以：

- 审计哪些 IRQ 真有 hook（`avr_irq_register_notify` 的注册点）；
- 对没有消费者的位，跳过逐位 `avr_raise_irq`（或仅在有 notify 链时才进循环）。

这是显示密集代码里的实打实开销，中等工作量、收益可观。

### 4.3 编译期裁剪未用外设

Makefile.zaurus 目前把整套外设都编进核心（`avr_usb.c avr_uart.c avr_twi.c avr_adc.c avr_usi(?)…`）。Arduboy 实际用到的是：**Timer0/1/3/4、SPI、IOPort、EEPROM、（可能 ADC 取随机种子）**。
`avr_usb`（ATmega32u4 的 USB）尤其值得确认：若游戏运行期不用 USB，其模型仍可能注册周期定时器空转。把未用外设从 `avr_init` 的挂载/IRQ 订阅里去掉，可减少 4.2 的 IRQ 开销与定时器池条目。低—中工作量、安全。

---

## 5. Tier 4 — 工具链与构建标志

现状（来自 `stage2.md` / `build_zaurus.sh`）：C 核心 `-O3 -fomit-frame-pointer -fno-strict-aliasing -mcpu=xscale -mtune=xscale -mhard-float`。补充建议：

- **确保 ARM 模式**（`-marm`，非 Thumb）。Thumb 省 Icache 但 XScale 下每指令更慢；热解释器优先 ARM。
- **`-mhard-float` 只影响浮点**。已确认热路径无浮点：`grep` 显示 `float/double` 仅出现在 `avr_timer.c` 的**定时器配置期**（写 TCCR 等寄存器时算频率），不在每指令路径。所以 FPA 仿真慢**不影响热循环**，保留 `-mhard-float` 以链接 FPA 版 Qt 库是正确的。但要确认解释器 TU 没有被意外带进软/硬浮点 helper。
- **不要 `-funroll-loops`**：会撑大热循环、压垮 32KB Icache。
- **保持热循环紧凑**：predecode + 计算 goto 的分发核心应尽量小以常驻 Icache。
- **XScale `PLD` 预取**：在解释器里预取「下一条 flash 字 / 下一条 dec[] 项」，遮掩无 L2 的内存延迟。进阶、小收益。
- GCC 3.4.6 无实用 PGO；不追。

---

## 6. Tier 5 — 换架构（高工作量 / 高天花板）

### 6.1 迷你 JIT / 动态重编译到 ARMv5（性能天花板）
把 AVR 基本块在运行时翻译成原生 ARMv5。由于 flash 仅 32KB 且静态，甚至可在加载后**一次性 AOT 翻译**整个程序。把 AVR 32 个 8 位寄存器映射到 ARM 寄存器/内存，AVR 标志尽量借用 ARM 标志。这是唯一能在 16MHz 之上留出**充裕余量**（甚至超频体验）的路。代价：数周工作量、调试复杂、需处理中断边界与自修改。作为「要全速 + 余量」的终极选项列出。

### 6.2 换用更精简的 ATmega32u4 解释器
放弃 simavr 的周期精确外设建模，换轻量核心，用精度换速度。风险：声音/时序精度下降，且要重做外设接线。simavr 的精确性本身是卖点，不建议轻易放弃。

### 6.3 保留 simavr 但砍精度档
提供「精确 / 快速」两档：快速档跳过部分周期定时器细节、降低 SPI/声音采样精度。介于 Tier 1–3 与 6.1 之间的折中。

---

## 7. Tier 6 — 感知层 / 已完成项

已完成且有效（继续保持）：30fps 显示上限、脏帧跳过、RGB565 缓存、`QDirectPainter` 直写、实时经过时间驱动周期。

可再加：
- `copy_frame` 只拷贝变化的 page/列（现在整帧 8×128 全拷）——收益小，Tier 1/3 之后再看。
- **自适应时钟**：实测达成的 IPS，自动在 8/12/16MHz 间选择，替代手动 Light/Boost。
- 若加声音：直接从 Timer 状态低成本合成，不要过采样。

---

## 8. 建议实施顺序（投入产出比）

1. **Tier 2.1 批处理运行内核**（改 `arduboy_core.c`，不动 simavr）— 最先做，风险最低。
2. **Tier 3.1 寄存器快速路径 + Tier 3.2 削减逐位 IRQ + Tier 4.3 裁剪未用外设** — 一组「安全的核心内改动」。
3. **Tier 3.1 预解码缓存** — 单项最大核心级收益；先 4 字节表 + PLD。
4. **Tier 3.2 计算 goto 直接线程化** — 在预解码稳定后叠加。
5. 评估是否仍不足 → 才考虑 **Tier 5.1 JIT**。

每步都应：真机测「Boost=16MHz 下的实际达成帧率/是否掉速」，并保留一个基准 `.hex`（建议用重计算的游戏，如带大量精灵/物理的）做回归。

## 9. 验证方法（把主观「变快了」变成数字）

现有 `tools/dump_frame` 可跑固定周期数并出 PBM。建议在核心加一个**吞吐基准**：
- 在 x86 与真机上都跑「模拟 N=1600 万 AVR 周期需要多少墙钟毫秒」，得到 `模拟MHz`。
- 目标：真机 `模拟MHz ≥ 16` 且有余量。
- 每次优化前后对比 `模拟MHz`，并用 `dump_frame` 出的 PBM 做**逐字节比对**确保没有改坏语义（尤其是 Tier 2.1 批处理、Tier 3 快速路径这些动语义的改动）。
- 回归集：至少 2–3 个真实游戏 `.hex`，比对若干固定周期点的帧 hash。

---

### 附：关键代码位置速查
| 关注点 | 文件:行为 |
|---|---|
| 每指令调用定时器处理 | `sim/sim_avr.c` `avr_callback_run_raw()` |
| 前端间接调用 `avr->run` | `arduboy_core.c` `zaurus_arduboy_run_cycles()` → `sim_avr.c` `avr_run()` |
| 大 switch 解码器 | `sim/sim_core.c` `avr_run_one()` (~765 行起) |
| 内存访问回绕检查 + SRAM IRQ | `sim/sim_core.c` `avr_core_watch_read/write()` |
| I/O 写逐位 IRQ 循环 | `sim/sim_core.c` `_avr_set_r()` |
| SPI 字节级完成 → SSD1306 | `sim/avr_spi.c` `avr_spi_write()`；`examples/parts/ssd1306_virt.c` |
| 定时器配置期浮点（非热路径） | `sim/avr_timer.c` |
| 已启用外设集合 | `Makefile.zaurus` `SIM_SRCS` |
