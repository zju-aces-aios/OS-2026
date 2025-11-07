# 实验一：Unikraft 内存分配策略分析

## 实验目标

本实验旨在通过 `qemu` 和 `gdb` 调试 Unikraft 内核，深入分析其内存分配过程。你需要：

1.  跟踪不同大小内存请求的分配流程。
2.  分析当前 Buddy 和 Slab 分配器的协作机制。
3.  找出当前内存分配策略 / 协作机制中可能存在的不合理之处，例如内存碎片或正确性问题。
4.  将你的分析和调试过程记录下来，形成一份完整的实验报告。

## 任务与思考题

### 1. 内存分配大小分析

在调试过程中，通过观察内存状态，填写下表，记录不同请求大小对应的实际分配大小。

| 请求分配大小 | 实际分配大小 | 分析与说明                                          |
| :----------- | :----------- | :-------------------------------------------------- |
| 96 字节      | 128字节      | 96+32=128，因此分配128                              |
| 128 字节     | 2*128字节    | 128+32->128+128（order0大小128），因此分配2*128     |
| 256 字节     | 3*128字节    | 256+32->128+128+128（order0大小128），因此分配3*128 |
| 4064 字节    | 4096字节     | 4064+32=4096(order0大小4096)，因此分配4096          |
| 4096 字节    | 8192字节     | 4096+32->8192(order1大小8192)，直接分配8192         |

### 2. 核心问题

请在报告中回答以下问题：

1. **最小分配单元**: Unikraft 两种内存分配策略的最小单元是多少？它是如何定义的？

   Slab ：128字节

   [app-helloworld/workdir/unikraft/lib/ukalloc/alloc.c](https://github.com/zju-aces-aios/OS-2026/blob/0221c1aeae90d2503e0c5d00a23fb8e77f0ac97e/app-helloworld/workdir/unikraft/lib/ukalloc/alloc.c#L51)：

   \#define __S_PAGE_SHIFT 7

   \#define __S_PAGE_SIZE (1ULL << __S_PAGE_SHIFT)

   Buddy ：4096字节

   [app-helloworld/workdir/unikraft/arch/arm/arm/include/uk/asm/limits.h](https://github.com/zju-aces-aios/OS-2026/blob/0221c1aeae90d2503e0c5d00a23fb8e77f0ac97e/app-helloworld/workdir/unikraft/arch/arm/arm/include/uk/asm/limits.h#L35)

   #define __PAGE_SHIFT		12

   #ifdef __ASSEMBLY__
   #define __PAGE_SIZE		(1 << __PAGE_SHIFT)

2. **分配器选择**: `uk_malloc()` 函数在何种条件下会选择 `palloc`，又在何种条件下会选择`salloc`？

   在请求分配的内存>=4096/5字节时选palloc，<4096/5字节时用salloc

3. **大内存分配问题**: 当前 `palloc` 在处理大内存（例如，一次性分配多个页面）的分配与回收时，存在一个已知的设计问题。请定位该问题，并尝试在 GDB 中通过 `set` 命令修改相关变量，模拟正确的 `free` 过程，并截图记录结果。

在调试 palloc处理大内存（多页连续分配）场景时，发现 free 阶段存在设计缺陷。通过 GDB 断点调试可以看到：

```
Breakpoint 1, uk_free_ifpages (a=0x40010000, ptr=0x44101aa0, 
    small=0x4011acd4 <uk_free_ifpages>)
    at /workspaces/OS-2026/app-helloworld/workdir/unikraft/lib/ukalloc/alloc.c:209
```

info args显示：

```
a = 0x40010000
ptr = 0x44101aa0
small = 0x4011acd4 <uk_free_ifpages>
```

small参数被误传成了一个函数地址，而不是布尔值，说明调用栈中传参存在错误。

继续查看回溯：

```
#0  uk_free_ifpages (a=0x40010000, ptr=0x44101aa0, 
    small=0x4011acd4 <uk_free_ifpages>)
    at /workspaces/OS-2026/app-helloworld/workdir/unikraft/lib/ukalloc/alloc.c:209
#1  0x000000004012f460 in uk_do_free (a=0x40010000, ptr=0x44101aa0)
    at /workspaces/OS-2026/app-helloworld/workdir/unikraft/lib/ukalloc/include/uk/alloc.h:210
#2  0x000000004012f488 in uk_free (a=0x40010000, ptr=0x44101aa0)
    at /workspaces/OS-2026/app-helloworld/workdir/unikraft/lib/ukalloc/include/uk/alloc.h:214
```

可以确认问题出在 uk_do_free() → uk_free_ifpages() 的参数不匹配。

在 alloc.h 中定义的：

```
static inline void uk_do_free(struct uk_alloc *a, void *ptr) {
    UK_ASSERT(a);
    a->free(a, ptr);
}
```

仅传入了两个参数。而实际实现的：

```
void uk_free_ifpages(struct uk_alloc *a, void *ptr, const void *small);
```

却需要三个参数。
 因此，当分配器注册了 free = uk_free_ifpages 时，uk_do_free() 调用时参数数量不符，第三个参数 small 会被错误填充为一个随机值或地址（在此为函数地址），从而导致释放逻辑异常。

为了验证问题，在 GDB 中手动修改变量，使 small 恢复为正确值：

```
(gdb) set small = 0
(gdb) c
Continuing.
[Inferior 1 (process 1) exited normally]
```

程序正常退出，说明问题确实与 small 参数错误有关。
