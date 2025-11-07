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

| 请求分配大小 | 实际分配大小 | 分析与说明 |
| :------- | :------ | :--------- |
| 96 字节   | 1页/128字节 | 小页面分配的最小单位就是128字节，在alloc.c中，会给内存加上32字节的metadata，得到真实大小；<br />真实大小为96+32=128字节，正好一页。<br />在alloc.c的头上define中，定义了：  <br />`#define __S_PAGE_SHIFT 7`<br /> `#define __S_PAGE_SIZE (1ULL << __S_PAGE_SHIFT)`<br />这里决定了最小的s_page_size。输出：<br /> |
| 128 字节  | 2页/256字节 | 同上，加上32字节的metadata之后，就有160字节了，一页放不下。输出：[    0.974567] ERR:  [libukalloc] <alloc.c @  181> alloc size => 160, num_pages => 2, intpter => 0x44101980 |
| 256 字节  | 3页/384字节 | 输出：[    0.931424] ERR:  [libukalloc] <alloc.c @  181> alloc size => 288, num_pages => 3, intpter => 0x44101900<br />分析：288字节，两页放不下 |
| 4064 字节 | 1页/4096字节 | 通过GDB得到：PAGE_SIZE 是4096字节，只要请求的内存大于等于4096/5也就是820，那就会进入大内存分配的流程，调用uk_palloc. <br />现在至少有1个4096字节的块了。具体是否如是，需要再确认。<br />在代码里加一行：<br />printf("\n p1 here _______ ------- p1 :%p\n",p1);<br />可以偷懒地确认分配的地址。<br />我注意到，分配的地址不是边界对齐的。<br />gdb了半天，找到：return (void *)(intptr + METADATA_IFPAGES_SIZE_POW2);，所以返回的地址是元数据之后的，它不对齐很合理。<br /> |
| 4096 字节 | 2页/8192字节 | 一样到了大内存分配。但是4096+32已经超过了4096，所以用两个块填上。 |

### 2. 核心问题

请在报告中回答以下问题：

1. **最小分配单元**: Unikraft 两种内存分配策略的最小单元是多少？它是如何定义的？
   在alloc.h里：

   `#define __S_PAGE_SHIFT 7`<br /> `#define __S_PAGE_SIZE (1ULL << __S_PAGE_SHIFT)`
   小内存是128字节。

   在limits.h里定义了：

   ` #define __PAGE_SHIFT 12`

   ` #define __PAGE_SIZE (1 << __PAGE_SHIFT)`

   也就是4096字节。

2. **分配器选择**: `uk_malloc()` 函数在何种条件下会选择 `palloc`，又在何种条件下会选择`salloc`？
   __PAGE_SIZE 是4096字节，只要请求的内存大于等于4096/5也就是819.2字节，那就会进入大内存分配的流程，调用uk_palloc

3. **大内存分配问题**: 当前 `palloc` 在处理大内存（例如，一次性分配多个页面）的分配与回收时，存在一个已知的设计问题。请定位该问题，并尝试在 GDB 中通过 `set` 命令修改相关变量，模拟正确的 `free` 过程，并截图记录结果。

   ``` shell
   0x0000000040000000 in ?? ()
   (gdb) b uk_free_ifpages
   Breakpoint 1 at 0x4011ace8: file /workspaces/OS-2026/app-helloworld/workdir/unikraft/lib/ukalloc/alloc.c, line 209.
   (gdb) c
   Continuing.
   
   Breakpoint 1, uk_free_ifpages (a=0x40010000, ptr=0x44101aa0, 
       small=0x4011acd4 <uk_free_ifpages>)
       at /workspaces/OS-2026/app-helloworld/workdir/unikraft/lib/ukalloc/alloc.c:209
   209       UK_ASSERT(a);
   (gdb) info args
   a = 0x40010000
   ptr = 0x44101aa0
   small = 0x4011acd4 <uk_free_ifpages>
   (gdb) set small = 0
   (gdb) c
   Continuing.
   [Inferior 1 (process 1) exited normally]
   ```

   

​	trace一下：

```shell
(gdb) bt
#0  uk_free_ifpages (a=0x40010000, ptr=0x44101aa0, 
    small=0x4011acd4 <uk_free_ifpages>)
    at /workspaces/OS-2026/app-helloworld/workdir/unikraft/lib/ukalloc/alloc.c:209
#1  0x000000004012f460 in uk_do_free (a=0x40010000, ptr=0x44101aa0)
    at /workspaces/OS-2026/app-helloworld/workdir/unikraft/lib/ukalloc/include/uk/alloc.h:210
#2  0x000000004012f488 in uk_free (a=0x40010000, ptr=0x44101aa0)
    at /workspaces/OS-2026/app-helloworld/workdir/unikraft/lib/ukalloc/include/uk/alloc.h:214
#3  0x000000004012f958 in pf_probe_fdt ()
    at /workspaces/OS-2026/app-helloworld/workdir/unikraft/drivers/ukbus/platform/platform_bus.c:177
#4  0x000000004012f9fc in pf_probe ()
    at /workspaces/OS-2026/app-helloworld/workdir/unikraft/drivers/ukbus/platform/platform_bus.c:204
#5  0x0000000040120128 in uk_bus_probe (b=0x4014c478 <pfh>)
--Type <RET> for more, q to quit, c to continue without paging--
    at /workspaces/OS-2026/app-helloworld/workdir/unikraft/lib/ukbus/bus.c:88
#6  0x000000004012026c in uk_bus_probe_all ()
    at /workspaces/OS-2026/app-helloworld/workdir/unikraft/lib/ukbus/bus.c:125
#7  0x00000000401202cc in uk_bus_lib_init (ictx=0x4000ff88)
    at /workspaces/OS-2026/app-helloworld/workdir/unikraft/lib/ukbus/bus.c:136
#8  0x000000004011f558 in uk_boot_entry ()
    at /workspaces/OS-2026/app-helloworld/workdir/unikraft/lib/ukboot/boot.c:383
#9  0x0000000000000000 in ?? ()
Backtrace stopped: previous frame identical to this frame (corrupt stack?)
```

找到：在alloc.h中，

```c
static inline void uk_do_free(struct uk_alloc *a, void *ptr) {
  UK_ASSERT(a);
  a->free(a, ptr);
}
```

但是，

```c
void uk_free_ifpages(struct uk_alloc *a, void *ptr, const void *small);
```

有三个参数，这就不对了

修改：

```c
/* 根据分配时的 size 重新判断是否为 small */
struct metadata_ifpages *metadata = uk_get_metadata(ptr, 0);
int is_small = IS_SMALL(metadata->size);
```