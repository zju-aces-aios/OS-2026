**# 实验一：Unikraft 内存分配策略分析**



**## 实验目标**



本实验旨在通过 `qemu` 和 `gdb` 调试 Unikraft 内核，深入分析其内存分配过程。你需要：

1. 跟踪不同大小内存请求的分配流程。

2. 分析当前 Buddy 和 Slab 分配器的协作机制。

3. 找出当前内存分配策略 / 协作机制中可能存在的不合理之处，例如内存碎片或正确性问题。

4. 将你的分析和调试过程记录下来，形成一份完整的实验报告。



**## 任务与思考题**



**### 1. 内存分配大小分析**



在调试过程中，通过观察内存状态，填写下表，记录不同请求大小对应的实际分配大小。



| 请求分配大小 | 实际分配大小 | 分析与说明 |

| :------- | :------ | :--------- |

| 96 字节  |   128B   |   因为size<50%页面大小，128B是最小的分配单元，且96+32<=128，所以分配128B    |

| 128 字节  |    256B  |  128+32=160<2*128，所以分配256B     |

| 256 字节  |    384B  |   256+32=288<3*128，所以分配384B    |

| 4064 字节 |   4KB   |    因为size>=50%页面大小 ，采用页为分配单位，4064+32=4096，所以分配4KB   |

| 4096 字节 |   8KB   |     4096+32<2*4096，分配8KB  |

![image-20251101131338680](C:\Users\qq117\AppData\Roaming\Typora\typora-user-images\image-20251101131338680.png)

根据代码中realsize（真实分配的空间）的数值来源来看由我们需要分配的空间加上metadata的数据（32B），然后根据realsize是否超过50%的页面大小来据欸的那个采取salloc还是palloc

**### 2. 核心问题**



请在报告中回答以下问题：



1. ***\*最小分配单元\****: Unikraft 两种内存分配策略的最小单元是多少？它是如何定义的？  128B   

2. ***\*分配器选择\****: `uk_malloc()` 函数在何种条件下会选择 `palloc`，又在何种条件下会选择`salloc`？

3. ***\*大内存分配问题\****: 当前 `palloc` 在处理大内存（例如，一次性分配多个页面）的分配与回收时，存在一个已知的设计问题。请定位该问题，并尝试在 GDB 中通过 `set` 命令修改相关变量，模拟正确的 `free` 过程，并截图记录结果。



问题1：存在Buddy 和 Slab两种内存分配策略

Slab的内存分配策略(salloc)如下，最小单位为128B，

![image-20250922171457039](C:\Users\qq117\AppData\Roaming\Typora\typora-user-images\image-20250922171457039.png)

Buddy内存分配策略(palloc)如下，最小单位为4KB

![image-20251101132714913](C:\Users\qq117\AppData\Roaming\Typora\typora-user-images\image-20251101132714913.png)



问题二：

根据代码中的定义，小于20%采用salloc，大于则采用palloc进行分配

![image-20251101133144716](C:\Users\qq117\AppData\Roaming\Typora\typora-user-images\image-20251101133144716.png)

![image-20250922171526926](C:\Users\qq117\AppData\Roaming\Typora\typora-user-images\image-20250922171526926.png)



问题三：

该问题通过函数调用栈来看存在于下图中的free函数传参中，未传入small的值

![image-20250922171815192](C:\Users\qq117\AppData\Roaming\Typora\typora-user-images\image-20250922171815192.png)

最后在下图函数中将small  set 为0即可转向pfree
![image-20250922171918273](C:\Users\qq117\AppData\Roaming\Typora\typora-user-images\image-20250922171918273.png)

转向如下图所示

![image-20250922172000208](C:\Users\qq117\AppData\Roaming\Typora\typora-user-images\image-20250922172000208.png)