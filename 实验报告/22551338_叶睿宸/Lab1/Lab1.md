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
| 96 字节   | 128字节  |  gdb调试，发现内存分配走salloc，salloc最小页大小为128B，由于前METADATA_IFPAGES_SIZE_POW2（32B）保存内存分配元数据信息，因此总共需要128B，恰好是slab分配方式一页大小，因此实际分配内存为128B  |
| 128 字节  | 256字节  |  内存分配走salloc，元数据+用户请求共占用160B，因此需要两页内存，实际分配内存为256B  |
| 256 字节  | 384字节  |  内存分配走salloc，元数据+用户请求共占用288B，因此需要三页内存，实际分配内存为384B  |
| 4064 字节 | 4096字节 |  gdb调试，发现内存分配走palloc，palloc最小页大小为4KB，由于前METADATA_IFPAGES_SIZE_POW2（32B）保存内存分配元数据信息，因此总共需要4KB，恰好是buddy分配方式一页大小，因此实际分配内存为4096B  |
| 4096 字节 | 8192字节 |  内存分配走palloc，元数据+用户请求共占用4128B，因此需要两页内存，实际分配内存为8192B  |

### 2. 核心问题

请在报告中回答以下问题：

1.  **最小分配单元**: Unikraft 两种内存分配策略的最小单元是多少？它是如何定义的？
* Buddy分配策略：
  * 该分配策略的最小分配单元是页，而页的大小是由系统架构决定的。
  * 该实验采用arm64架构，因此其定义的页大小在**unikraft/arch/arm/arm64/include/uk/asm/paging.h**下定义
    * ` #define PAGE_SIZE			0x1000UL `
  * 0x1000UL表示十六进制无符号长整型，十进制为4096，即4KB，因此Buddy在arm64架构下的最小分配大小是4KB
* Slab分配策略：
  * 该分配策略的最小分配单元是对象，这个大小​​可以远小于一页​​（例如几十或几百字节），并且是​​按需创建​​的，旨在减少内部碎片和提高分配旨在减少内部碎片和提高分配效率。
  * 实验所用Unikraft版本slab最小分配大小宏定义在**unikraft/lib/ukalloc/alloc.c**下定义
    * ` #define __S_PAGE_SHIFT 7`
    * ` #define __S_PAGE_SIZE (1ULL << __S_PAGE_SHIFT) `
  * 1ULL左移7位即表示2^7，因此slab分配器的最小分配单元为128B，尽管宏名用了PAGE，但这并不表示传统意义上的“页”，而是作为slab分配器的最小分配粒度，以此为单位为对象分配内存
2.  **分配器选择**: `uk_malloc()` 函数在何种条件下会选择 `palloc`，又在何种条件下会选择`salloc`？
```c 
#define __PAGE_SHIFT		12
#define __PAGE_SIZE		(1ULL << __PAGE_SHIFT)
#define IS_SMALL(size) ((size) < (__PAGE_SIZE / 5))
...
if (IS_SMALL(realsize)) {
num_pages = size_to_s_num_pages(realsize);
intptr = (__uptr)uk_salloc(a, num_pages);
uk_pr_err("alloc size => %llu, num_pages => %llu, intpter => %p\n",
            realsize, num_pages, intptr);
} else {
num_pages = size_to_num_pages(realsize);
intptr = (__uptr)uk_palloc(a, num_pages);
}
```
  * 结合宏和函数`uk_malloc_ifpages()`内部逻辑可以判断，当实际分配大小小于页大小1/5时，选择 salloc（slab 分配器）；否则选择 palloc（buddy 分配器）。
3.  **大内存分配问题**: 当前 `palloc` 在处理大内存（例如，一次性分配多个页面）的分配与回收时，存在一个已知的设计问题。请定位该问题，并尝试在 GDB 中通过 `set` 命令修改相关变量，模拟正确的 `free` 过程，并截图记录结果。
  * 这里以`malloc(4096)`举例，根据前文的实验数据可知这个malloc过程会走palloc方式分配内存且一次性分配多个（2个）页面，具体的gdb调试内容如下所示：
```c
(gdb) b main
Breakpoint 1 at 0x4010774c: main. (2 locations)
(gdb) c
Continuing.

Breakpoint 1, main (argc=1, argv=0x4016f138 <arg_vect>)
    at /OS-2026/app-helloworld/main.c:31
31        void* p = malloc(4096);
(gdb) call uk_alloc_pavailmem_total()
$1 = 32247      // 当前可用物理页总数
(gdb) n
33        free(p);
(gdb) call uk_alloc_pavailmem_total()
$2 = 32245      // 分配内存走palloc占用两个页，因此当前可用物理页总数-2
(gdb) s
...
(gdb) n
221       if (small) {
(gdb) p small   // 此时进入内存释放逻辑，结果发现small非空非0，进入slab释放内存逻辑
$3 = (const void *) 0x4011ad90 <uk_free_ifpages>
(gdb) n
222         uk_sfree(a, metadata->base, metadata->num_pages);
(gdb) n
230     }
(gdb) call uk_alloc_pavailmem_total()
$4 = 32245      // 由于执行的slab释放内存逻辑，导致物理页没有正确归还，当前可分配物理页依旧是32245
```
  * 从上述的gdb调试内容可以发现，由于small没有正确被赋值，导致代码进入了错误的内存释放分支，最终导致内存没有被正常释放。接下来通过set修复small值展示正常释放逻辑的gdb输出：
```c
(gdb) b main
Breakpoint 1 at 0x4010774c: main. (2 locations)
(gdb) c
Continuing.

Breakpoint 1, main (argc=1, argv=0x4016f138 <arg_vect>)
    at /OS-2026/app-helloworld/main.c:31
31        void* p = malloc(4096);
(gdb) call uk_alloc_pavailmem_total()
$1 = 32247
(gdb) n
33        free(p);
(gdb) call uk_alloc_pavailmem_total()
$2 = 32245
(gdb) s
...
(gdb) n
221       if (small) {
(gdb) p small
$3 = (const void *) 0x4011ad90 <uk_free_ifpages>
(gdb) set small=0   // 此处强制将small设为0，引导其走正确的释放分支
(gdb) n
224         uk_pfree(a, metadata->base, metadata->num_pages);
(gdb) n
230     }
(gdb) call uk_alloc_pavailmem_total()
$4 = 32247          // 最终可以发现当前可分配物理页恢复到32247，内存页被正常释放
```
  * 错误原因：
    * 在**unikraft/lib/ukalloc/include/uk/alloc.h**文件下定义的函数指针`typedef void (*uk_alloc_free_func_t)(struct uk_alloc *a, void *ptr);`只包含两个参数，函数指针变量在赋值时却为其分配了一个包含三个参数的函数指针`(a)->free = uk_free_ifpages;`，`void uk_free_ifpages(struct uk_alloc *a, void *ptr, const void *small)`
    * 在aarch64 ABI下，调用只传入两参，第三个参数未得到正常的输入，编译器会默认将“被调用的函数地址”作为第三个参数的输入，gdb可以看到`uk_free_ifpages (a=0x40010000, ptr=0x40192020, small=0x4011ad90 <uk_free_ifpages>)`
    * 因此`uk_free_ifpages`内部的`small`值始终为True，导致内存释放只会走slab释放逻辑，无法正常释放buddy方式分配的内存。