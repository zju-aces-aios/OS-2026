# Lab4 实验报告

[TOC]



## 1 实验数据记录表

| 实验编号 | 实现方式           | 设备/模拟器 | 矩阵尺寸 (M×K×N) | 计算耗时 (ms) | 备注 |
| -------- | ------------------ | ----------- | ---------------- | ------------- | ---- |
| 1        | 朴素 baseline      | 设备        | 64×64×64         | 46.418  |      |
| 2        | HVX 内积 (A * B^T) | 设备        | 64×64×64         | **29.930** |      |
| 3        | HVX 外积 (A * B)   | 设备        | 64×64×64         | 42.485  |      |
| 4        | 朴素 baseline      | 设备        | 256×256×256      | 608.166 |      |
| 5        | HVX 内积 (A * B^T) | 设备        | 256×256×256      | **54.864** |      |
| 6        | HVX 外积 (A * B)   | 设备        | 256×256×256      | 70.372 |      |
| 7        | 朴素 baseline      | 设备        | 512×512×512      | 6300.792      |      |
| 8        | HVX 内积 (A * B^T) | 设备        | 512×512×512      | **180.729** |      |
| 9        | HVX 外积 (A * B)   | 设备        | 512×512×512      | 283.632 |      |
| 10       | 朴素 baseline      | 设备        | 88×99×66         | 57.689  |      |
| 11       | HVX 内积 (A * B^T) | 设备        | 88×99×66         | **34.886** |      |
| 12       | HVX 外积 (A * B)   | 设备        | 88×99×66         | 47.378       |      |



额外的，1024x1024x1024：

- 166492.837 ms
- 外积：941.847 ms
- 内积：1915.807 ms



## 2 实现分析

### 2.1 方法一：内积

**初始实现**中，每次读取 A 和 B^T 中连续的 32 个 float 数据，作为两个 HVX 向量进行乘法并累加至一个向量寄存器 vacc 中：

```c
for (uint32_t i = 0; i < m; ++i) {
    for (uint32_t j = 0; j < n; ++j) {
        HVX_Vector vacc = Q6_V_vzero();

        uint32_t kk = 0;
        for (; kk + VEC <= k; kk += VEC) {
            HVX_Vector a_vec, b_vec;

            memcpy(&a_vec, &A[i * k + kk], sizeof(HVX_Vector));
            memcpy(&b_vec, &BT[j * k + kk], sizeof(HVX_Vector));

            HVX_Vector prod = Q6_Vsf_vmpy_VsfVsf(a_vec, b_vec);
            vacc = Q6_Vsf_vadd_VsfVsf(vacc, prod);
        }
```

此实现能够有效利用 HVX 的并行能力，但在每轮循环中 memcpy 拷贝内存至向量寄存器，成为性能瓶颈。此外，结果仍存储在向量中，需要进一步规约求和。



**水平规约优化**

为了将 vacc 中 32 个 float 值规约为标量，初始做法是手动提取并累加，这带来了寄存器间数据搬运和延迟。改进后使用 HVX 的循环右移指令配合加法实现快速规约：

```c
static inline float hvx_hsum_f32(HVX_Vector v) {
    for (int shift = sizeof(HVX_Vector) / 2; shift >= 4; shift >>= 1) {
        v = Q6_Vsf_vadd_VsfVsf(v, Q6_V_vror_VR(v, shift));
    }
    float s;
    memcpy(&s, &v, sizeof(float));
    return s;
}
```

该函数能在线程内部以 log(N) 步完成加和。



**尾块处理：对齐补零避免分支**

当 k 不是 32 的倍数时，尾部数据不足一个 HVX 向量。为保持 SIMD 处理路径统一，采用补零的方式，将不足的数据填入一个全 0 的寄存器中对应位置，从而无需切换到标量路径：

```c
if (kk < k) {
    uint32_t r = k - kk;
    HVX_Vector a_vec = Q6_V_vzero();
    HVX_Vector b_vec = Q6_V_vzero();

    memcpy(&a_vec, &A[i * k + kk], sizeof(float) * r);
    memcpy(&b_vec, &BT[j * k + kk], sizeof(float) * r);

    HVX_Vector prod = Q6_Vsf_vmpy_VsfVsf(a_vec, b_vec);
    vacc = Q6_Vsf_vadd_VsfVsf(vacc, prod);
}
```

实测显示，对于尾块长度较小时（如 < 4），该做法性能劣于手动展开，但为保持路径一致性与实现简洁性，最终未引入分支优化。



**内存对齐检测：减少拷贝**

高通平台上，若内存地址及跨度均满足 128 字节对齐，HVX 支持直接将内存作为向量操作数加载，避免显式 memcpy。因此，在入口添加判断逻辑：

```c
uintptr_t addrA  = (uintptr_t)A;
uintptr_t addrBT = (uintptr_t)BT;
size_t    stride = (size_t)k * sizeof(float);

int all_aligned = ((addrA  & 127) == 0)
                && ((addrBT & 127) == 0)
                && ((stride & 127) == 0);
```

若满足条件，切换至零拷贝路径：

```c
HVX_Vector a_vec = *(HVX_Vector const *)(rowA + kk);
HVX_Vector b_vec = *(HVX_Vector const *)(rowB + kk);
```



**L2 预取：提前加载数据**

在 HVX 中可通过 l2fetch 指令实现手动预取，降低缓存未命中带来的 stalls。为配合 128B 对齐访问，使用如下宏封装：

```c
static inline void L2fetch(const void *base, unsigned rt) {
    __asm__ __volatile__("l2fetch(%0,%1)" :: "r"(base), "r"(rt));
}
#define L2PACK(width, stride, height) \
    (((unsigned)(stride) << 16) | ((unsigned)(width) << 8) | (unsigned)(height))
```

预取配置如下：

```c
const unsigned PF128 = L2PACK(128, 128, 1);
const uint32_t PF_DIST_CHUNKS = 8;
```

实际应用中，每轮迭代提取远端数据：

```c
uint32_t kk_pf = kk + PF_DIST_CHUNKS * VEC;
if (kk_pf < k) {
    L2fetch(rowA + kk_pf, PF128);
    L2fetch(rowB + kk_pf, PF128);
}
```

在内存访问密集场景下，预取略有提升。



**多列并发**

在矩阵乘法中，若矩阵 A 行不变、B^T 多列参与点积，实际上存在数据重用的空间。于是我们将每次计算从单列扩展为 4 列并行：

```c
const uint32_t JBLK = 4;
for (uint32_t j = 0; j + JBLK <= n; j += JBLK) {
    HVX_Vector vacc0 = Q6_V_vzero();
    HVX_Vector vacc1 = Q6_V_vzero();
    HVX_Vector vacc2 = Q6_V_vzero();
    HVX_Vector vacc3 = Q6_V_vzero();

    const float *rowB0 = BT + (size_t)(j + 0) * k;
    const float *rowB1 = BT + (size_t)(j + 1) * k;
    const float *rowB2 = BT + (size_t)(j + 2) * k;
    const float *rowB3 = BT + (size_t)(j + 3) * k;

    for (uint32_t kk = 0; kk < k; kk += VEC) {
        uint32_t kk_pf = kk + PF_DIST_CHUNKS * VEC;
        if (kk_pf < k) {
            L2fetch(rowA  + kk_pf, PF128);
            L2fetch(rowB0 + kk_pf, PF128);
            L2fetch(rowB1 + kk_pf, PF128);
            L2fetch(rowB2 + kk_pf, PF128);
            L2fetch(rowB3 + kk_pf, PF128);
        }

        HVX_Vector a_vec = *(HVX_Vector const *)(rowA + kk);
        HVX_Vector b0    = *(HVX_Vector const *)(rowB0 + kk);
        HVX_Vector b1    = *(HVX_Vector const *)(rowB1 + kk);
        HVX_Vector b2    = *(HVX_Vector const *)(rowB2 + kk);
        HVX_Vector b3    = *(HVX_Vector const *)(rowB3 + kk);

        vacc0 = Q6_Vsf_vadd_VsfVsf(vacc0, Q6_Vsf_vmpy_VsfVsf(a_vec, b0));
        vacc1 = Q6_Vsf_vadd_VsfVsf(vacc1, Q6_Vsf_vmpy_VsfVsf(a_vec, b1));
        vacc2 = Q6_Vsf_vadd_VsfVsf(vacc2, Q6_Vsf_vmpy_VsfVsf(a_vec, b2));
        vacc3 = Q6_Vsf_vadd_VsfVsf(vacc3, Q6_Vsf_vmpy_VsfVsf(a_vec, b3));
    }

    C[i * n + j + 0] = hvx_hsum_f32(vacc0);
    C[i * n + j + 1] = hvx_hsum_f32(vacc1);
    C[i * n + j + 2] = hvx_hsum_f32(vacc2);
    C[i * n + j + 3] = hvx_hsum_f32(vacc3);
}
```



### 2.2 方法二：外积

在最初的实现中，遍历矩阵 A 的每一行、矩阵 B 的每一列向量。将单个标量 aik 广播为 HVX 向量 a_brd，再与 B 的一个列块 b_vec 执行并行乘加，写入结果矩阵：

```c
for (uint32_t i = 0; i < m; ++i) {
    uint32_t j = 0;
    for (; j + VEC <= n; j += VEC) {
        HVX_Vector vacc = Q6_V_vzero();
        for (uint32_t kk = 0; kk < k; ++kk) {
            float aik = A[i * k + kk];
            HVX_Vector a_brd = Q6_V_vsplat_R(float_to_bits(aik));
            HVX_Vector b_vec;
            memcpy(&b_vec, &B[kk * n + j], sizeof(HVX_Vector));
            vacc = Q6_Vsf_vadd_VsfVsf(vacc, Q6_Vsf_vmpy_VsfVsf(a_brd, b_vec));
        }
        memcpy(&C[i * n + j], &vacc, sizeof(HVX_Vector));
    }
    // 尾块处理略（与内积方案一致）
}
```



**对齐路径优化（省略）**

与内积法类似，本实现同样在运行前检查输入地址与步幅是否满足 128 字节对齐要求。在满足条件时，使用直接指针方式进行向量加载，避免 memcpy 带来的额外开销。鉴于原理一致，此处不再赘述。



**线性预取尝试**

起初在 `matmul_ijk` 的对齐分支里，把外积循环改成边算边线性预取：先对首批 k 行做一次 L2fetch 暖场，再在每次迭代里用固定的行距偏移预取下一块，确保 B 的列向量在访问前已经进 L2。虽然现在的代码不再保留那段逻辑，但核心做法是利用 `b_ptr += n` 的自然行距来构造线性 prefetch 队列。

**改成 box 预取**

为了更贴合外积访问模式，把线性预取替换成 box 形式：用 `L2PACK(width,stride,height)` 打出 128B×8 行的预取框，行跨度直接取 `n*4` 字节。代码里 `PF_WIDTH_BYTES`、`PF_HEIGHT_ROWS` 与 `use_box_prefetch` 的组合就完成了这个切换，并在每处理完 8 行时提交下一块。

**四列并发**
在完成基本预取机制之后，为进一步提高向量利用率，参考内积法中的方案，本实验将列向量扩展为四列并发

```
const float *colB0 = B + (j + 0) + kk * n;
const float *colB1 = B + (j + 1) + kk * n;
const float *colB2 = B + (j + 2) + kk * n;
const float *colB3 = B + (j + 3) + kk * n;
```

在每轮中：

- 读取一个 a_{ik}，广播为 a_brd；
- 对四列 B 依次加载为四个向量；
- 分别与 a_brd 相乘并累加进 vacc0 ~ vacc3；
- 最终写回 `C[i][j:j+4]`。

与之配套，Box 预取也在 4 条列地址上并行发起，形成 **128B × 8 × 4列** 的面状预取区域，显著降低加载延迟，提升整体带宽利用率。







### 2.3 数据复用、内存访问模式

**数据复用**：

- 内积：对每个 $C[i][j]$ 只复用一次同一位置的 $A$ 与 $B^T$ 子向量；跨 j 或 i 的复用有限，需要针对每个输出重新加载。
- 外积：每个 $A[i][kk]$ 标量在整行输出上复用，对 $B$ 的同一列向量复用度高；能将一个标量广播后多次乘加，减少对 $A$ 的访问。

由此可见外积对内存的访问次数更少（取而代之的是多了一个广播运算）。

实验也证明了这一点。在没有经过过多优化时，外积的性能明显由于内积。



**访问模式**：

- 内积：访问 $A$ 时是按行块；访问 $B^T$ 也是32元素连续访问。
- 外积：访问 $B$ 时是按列跨行扫描，每次读取整行向量；在列数较大时易出现跨行跳跃。

实验中也发现，l2fetch应用在外积中时，对性能的提升更加明显。





### 2.4 向量指令

|                    | **类型**             | **功能简述**                  | **在实现中的作用**                                           |
| ------------------ | -------------------- | ----------------------------- | ------------------------------------------------------------ |
| Q6_Vsf_vmpy_VsfVsf | 向量乘法             | 对两个 HVX 浮点向量逐元素相乘 | 是内积与外积乘加核心，每次执行 32 次并行乘法。               |
| Q6_Vsf_vadd_VsfVsf | 向量加法             | 对两个 HVX 浮点向量逐元素相加 | 用于累加结果，形成逐步累加向量 vacc。                        |
| Q6_V_vzero         | 向量生成             | 生成全 0 向量寄存器           | 初始化累加器 vacc，也用于尾块补零。                          |
| Q6_V_vsplat_R      | 标量广播             | 将标量寄存器广播为整个向量    | 外积中，将 `A[i][k]` 广播为 a_brd，实现标量×向量乘法。       |
| Q6_V_vror_VR       | 向量循环右移         | 将一个向量右移若干字节        | 用于归约求和，逐步加和向量内的 32 个 float。                 |
| l2fetch(base, rt)  | 汇编指令（手动预取） | 向 L2 Cache 提前加载一段内存  | 提前加载 A 或 B/B^T 向量块，减少 cache miss；配合 L2PACK 生成加载描述符。 |





### 2.5 优化建议

**尾部**

向量化操作往往依赖固定宽度（如 128B），当数据量不足一整个向量时需进行“尾块处理”。不合理的尾部处理方式可能导致分支开销、冗余计算或内存访问异常。

建议：

- 尽量采用统一的向量路径，通过补零避免切换到标量代码；
- 对极小尾部（< 8 float）可考虑展开计算。



**内存对齐增强**

HVX 指令通常要求操作数具备 128B 对齐，若不满足将无法使用零拷贝方式加载，从而引入 memcpy 或寄存器搬运开销。

建议：

- 构造数据是就劲量保证矩阵对齐，或提前将矩阵填充成128B对齐；
- 在内核入口执行对齐检查，根据地址和步长决定加载路径。



**缓存与内存带宽瓶颈优化**

HVX 执行速度远快于内存访问速度，因此 L2 Cache miss 是主要瓶颈之一。合理的预取机制可以显著减少 stall，提升带宽利用率。

**建议：**

- 充分利用平台支持的手动预取指令（如 l2fetch）；
- 根据访问模式选择适当的预取策略：
    - 对连续访问采用**线性预取**；
    - 对跨行访问使用**Box 预取**；
- 结合矩阵尺寸、行列主序结构、stride 大小等设计**动态预取策略**；
- 复杂场景可考虑**双缓冲（Double Buffering）**等高级机制，类似昇腾NPU。



## 3 延伸讨论

### 3.1 l2fetch

在实现分析和优化建议中已经详细说明了。



### 3.2 其他的优化手段

#### 多列并发

在本实验中，除了常规的向量化与预取优化外，还额外引入了**多列并发计算策略**，即在内积与外积路径中，每次同时计算结果矩阵中 C 的 4x32 个相邻列元素（如 $C[i][j] \sim C[i][j+3]$）。

虽然 HVX 本身不支持指令层面的 4 路并行执行，该策略的优化效果主要来源于**数据访问优化**：

- **共享** A\[i][:] **向量**：在处理 4 列时，所需的 A 行向量可复用一次加载，避免对相同行重复读取；
- **向量重用提升缓存效率**：由于多个列结果依赖同一行向量，缓存命中率与数据局部性显著提高；
- **减少访存带宽压力**：每次处理 4 个输出，单位访存带来的计算量提高



## 附录：完整代码

```c
// 拿到 float 的二进制表示
static inline int32_t float_to_bits(float input)
{
    union {
        float f;
        int32_t i;
    } fp32 = {.f = input};
    return fp32.i;
}

static inline void matmul_ijk_baseline(float *restrict input_matrix1,
                 float *restrict input_matrix2,
                 float *restrict output,
                 uint32_t m,
                 uint32_t k,
                 uint32_t n) {
    for (int i = 0;i < m; i++) {
		for (int j = 0; j < n; j++) {
			float sum = 0.0f;
			for (int l = 0; l < k; l++) {
				sum += input_matrix1[i * k + l] * input_matrix2[l * n + j];
			}
			output[i * n + j] = sum;
		}
	}
	return;
}

static inline void L2fetch(const void *base, unsigned rt) {
    __asm__ __volatile__("l2fetch(%0,%1)" :: "r"(base), "r"(rt));
}

// 打包 Rt: [31:16]=stride(bytes), [15:8]=width(bytes), [7:0]=height(lines)
#define L2PACK(width, stride, height) ( ((unsigned)(stride) << 16) | ((unsigned)(width) << 8) | ((unsigned)(height)) )

static inline void matmul_ijk(float *restrict A,
                              float *restrict B,
                              float *restrict C,
                              uint32_t m, uint32_t k, uint32_t n)
{
    const uint32_t VEC = 32;                                // 32*f32 = 128B

    uintptr_t addrB = (uintptr_t)B;
    uintptr_t addrC = (uintptr_t)C;
    size_t stride = (size_t)n * sizeof(float);
    int all_aligned = ((addrB & 127) == 0)
                   && ((addrC & 127) == 0)
                   && ((stride & 127) == 0);

    if (all_aligned) {
        const uint32_t PF_WIDTH_BYTES = 128;       // 32 floats per HVX vector
        const uint32_t PF_HEIGHT_ROWS = 8;         // fetch an 8x32 tile from B per request
        const uint32_t JBLK = 4;                   
        const size_t stride_floats = (size_t)n;
        const size_t stride_bytes_sz = stride_floats * sizeof(float);
        const int use_box_prefetch = (stride_bytes_sz <= 0xFFFF);
        const uint32_t stride_bytes = (uint32_t)stride_bytes_sz;
        for (uint32_t i = 0; i < m; ++i) {
            const float *rowA = A + (size_t)i * k;
            float *rowC = C + (size_t)i * n;
            uint32_t j = 0;

            for (; j + JBLK * VEC <= n; j += JBLK * VEC) {
                const float *b_base0 = B + j;
                const float *b_base1 = b_base0 + VEC;
                const float *b_base2 = b_base1 + VEC;
                const float *b_base3 = b_base2 + VEC;

                if (use_box_prefetch && k != 0) {
                    uint32_t init_rows = (k < PF_HEIGHT_ROWS) ? k : PF_HEIGHT_ROWS;
                    unsigned pf_cmd = L2PACK(PF_WIDTH_BYTES, stride_bytes, init_rows);
                    L2fetch(b_base0, pf_cmd);
                    L2fetch(b_base1, pf_cmd);
                    L2fetch(b_base2, pf_cmd);
                    L2fetch(b_base3, pf_cmd);
                }

                HVX_Vector vacc0 = Q6_V_vzero();
                HVX_Vector vacc1 = Q6_V_vzero();
                HVX_Vector vacc2 = Q6_V_vzero();
                HVX_Vector vacc3 = Q6_V_vzero();
                const float *b_ptr0 = b_base0;
                const float *b_ptr1 = b_base1;
                const float *b_ptr2 = b_base2;
                const float *b_ptr3 = b_base3;

                for (uint32_t kk = 0; kk < k; ++kk) {
                    if (use_box_prefetch && (kk % PF_HEIGHT_ROWS) == 0) {
                        uint32_t next = kk + PF_HEIGHT_ROWS;
                        if (next < k) {
                            uint32_t rem_rows = k - next;
                            uint32_t fetch_rows = (rem_rows < PF_HEIGHT_ROWS) ? rem_rows : PF_HEIGHT_ROWS;
                            unsigned pf_cmd = L2PACK(PF_WIDTH_BYTES, stride_bytes, fetch_rows);
                            const float *next0 = b_base0 + (size_t)next * stride_floats;
                            const float *next1 = b_base1 + (size_t)next * stride_floats;
                            const float *next2 = b_base2 + (size_t)next * stride_floats;
                            const float *next3 = b_base3 + (size_t)next * stride_floats;
                            L2fetch(next0, pf_cmd);
                            L2fetch(next1, pf_cmd);
                            L2fetch(next2, pf_cmd);
                            L2fetch(next3, pf_cmd);
                        }
                    }

                    float aik = rowA[kk];
                    HVX_Vector a_brd = Q6_V_vsplat_R(float_to_bits(aik));
                    HVX_Vector b_vec0 = *(HVX_Vector const *)b_ptr0;
                    HVX_Vector b_vec1 = *(HVX_Vector const *)b_ptr1;
                    HVX_Vector b_vec2 = *(HVX_Vector const *)b_ptr2;
                    HVX_Vector b_vec3 = *(HVX_Vector const *)b_ptr3;
                    vacc0 = Q6_Vsf_vadd_VsfVsf(vacc0, Q6_Vsf_vmpy_VsfVsf(a_brd, b_vec0));
                    vacc1 = Q6_Vsf_vadd_VsfVsf(vacc1, Q6_Vsf_vmpy_VsfVsf(a_brd, b_vec1));
                    vacc2 = Q6_Vsf_vadd_VsfVsf(vacc2, Q6_Vsf_vmpy_VsfVsf(a_brd, b_vec2));
                    vacc3 = Q6_Vsf_vadd_VsfVsf(vacc3, Q6_Vsf_vmpy_VsfVsf(a_brd, b_vec3));
                    b_ptr0 += stride_floats;
                    b_ptr1 += stride_floats;
                    b_ptr2 += stride_floats;
                    b_ptr3 += stride_floats;
                }

                *(HVX_Vector *)(rowC + j + VEC * 0) = vacc0;
                *(HVX_Vector *)(rowC + j + VEC * 1) = vacc1;
                *(HVX_Vector *)(rowC + j + VEC * 2) = vacc2;
                *(HVX_Vector *)(rowC + j + VEC * 3) = vacc3;
            }

            for (; j + VEC <= n; j += VEC) {
                const float *b_col_base = B + j;
                if (use_box_prefetch && k != 0) {
                    uint32_t init_rows = (k < PF_HEIGHT_ROWS) ? k : PF_HEIGHT_ROWS;
                    unsigned pf_cmd = L2PACK(PF_WIDTH_BYTES, stride_bytes, init_rows);
                    L2fetch(b_col_base, pf_cmd);
                }

                HVX_Vector vacc = Q6_V_vzero();
                const float *b_ptr = b_col_base;
                for (uint32_t kk = 0; kk < k; ++kk) {
                    if (use_box_prefetch && (kk % PF_HEIGHT_ROWS) == 0) {
                        uint32_t next = kk + PF_HEIGHT_ROWS;
                        if (next < k) {
                            uint32_t rem_rows = k - next;
                            uint32_t fetch_rows = (rem_rows < PF_HEIGHT_ROWS) ? rem_rows : PF_HEIGHT_ROWS;
                            unsigned pf_cmd = L2PACK(PF_WIDTH_BYTES, stride_bytes, fetch_rows);
                            const float *next_ptr = b_col_base + (size_t)next * stride_floats;
                            L2fetch(next_ptr, pf_cmd);
                        }
                    }
                    float aik = rowA[kk];
                    HVX_Vector a_brd = Q6_V_vsplat_R(float_to_bits(aik));
                    HVX_Vector b_vec = *(HVX_Vector const *)b_ptr;
                    vacc = Q6_Vsf_vadd_VsfVsf(vacc, Q6_Vsf_vmpy_VsfVsf(a_brd, b_vec));
                    b_ptr += stride_floats;
                }
                *(HVX_Vector *)(rowC + j) = vacc;
            }
        }
        return;
    }

    for (uint32_t i = 0; i < m; ++i) {
        uint32_t j = 0;
        for (; j + VEC <= n; j += VEC) {
            HVX_Vector vacc = Q6_V_vzero();
            for (uint32_t kk = 0; kk < k; ++kk) {
                float aik = A[i * k + kk];
                HVX_Vector a_brd = Q6_V_vsplat_R(float_to_bits(aik));
                HVX_Vector b_vec;
                memcpy(&b_vec, &B[kk * n + j], sizeof(HVX_Vector));
                vacc = Q6_Vsf_vadd_VsfVsf(vacc, Q6_Vsf_vmpy_VsfVsf(a_brd, b_vec));
            }
            memcpy(&C[i * n + j], &vacc, sizeof(HVX_Vector));
        }
        
        for (; j < n; ++j) {
            float sum = 0.0f;
            for (uint32_t kk = 0; kk < k; ++kk) {
                sum += A[i * k + kk] * B[kk * n + j];
            }
            C[i * n + j] = sum;
        }
    }
}

static inline float hvx_hsum_f32(HVX_Vector v) {
    for (int shift = (int)sizeof(HVX_Vector) / 2; shift >= 4; shift >>= 1) {
        v = Q6_Vsf_vadd_VsfVsf(v, Q6_V_vror_VR(v, shift));
    }
    float s;
    memcpy(&s, &v, sizeof(float));  // 只拷 4B
    return s;
}


static inline void matmul_ikj_transposed_b( // A * B^T, 其中 B^T 的布局为 n×k
    float *restrict A,
    float *restrict BT,   // BT[j * k + kk] == B^T[j, kk]
    float *restrict C,
    uint32_t m, uint32_t k, uint32_t n)
{
    const uint32_t VEC = 32;
    
    // 在调用前或内核开头做一次判断：
    uintptr_t addrA  = (uintptr_t)A;
    uintptr_t addrBT = (uintptr_t)BT;
    size_t    stride = (size_t)k * sizeof(float);  // 行跨度，字节数
    const unsigned PF128 = L2PACK(128, 128, 1);  // 线性预取：width=128B, stride=128B, height=1
    const uint32_t PF_DIST_CHUNKS = 8;     // 提前 8 个 128B 块，即 8*128B = 1KB

    // 全局对齐条件：
    // 1) A、BT 的起始地址都是 128B 对齐；
    // 2) 行跨度也是 128B 的倍数（k*4 % 128 == 0）。
    int all_aligned = ((addrA  & 127) == 0)
                    && ((addrBT & 127) == 0)
                    && ((stride & 127) == 0);
    
    if (all_aligned) {
        const uint32_t JBLK = 4;  // 四列并行
        for (uint32_t i = 0; i < m; ++i) {
            const float *rowA = A  + (size_t)i * k;
            for (uint32_t j = 0; j + JBLK <= n; j += JBLK) {
                HVX_Vector vacc0 = Q6_V_vzero();
                HVX_Vector vacc1 = Q6_V_vzero();
                HVX_Vector vacc2 = Q6_V_vzero();
                HVX_Vector vacc3 = Q6_V_vzero();
                const float *rowB0 = BT + (size_t)(j + 0) * k;
                const float *rowB1 = BT + (size_t)(j + 1) * k;
                const float *rowB2 = BT + (size_t)(j + 2) * k;
                const float *rowB3 = BT + (size_t)(j + 3) * k;

                for (uint32_t kk = 0; kk < k; kk += VEC) {
                    // 预取
                    uint32_t kk_pf = kk + PF_DIST_CHUNKS * VEC;
                    if (kk_pf < k) {
                        L2fetch(rowA  + kk_pf, PF128);
                        L2fetch(rowB0 + kk_pf, PF128);
                        L2fetch(rowB1 + kk_pf, PF128);
                        L2fetch(rowB2 + kk_pf, PF128);
                        L2fetch(rowB3 + kk_pf, PF128);
                    }

                    // —— 正确的内积向量加载 —— 
                    HVX_Vector a_vec = *(HVX_Vector const *)(rowA  + kk);
                    HVX_Vector b0    = *(HVX_Vector const *)(rowB0 + kk);
                    HVX_Vector b1    = *(HVX_Vector const *)(rowB1 + kk);
                    HVX_Vector b2    = *(HVX_Vector const *)(rowB2 + kk);
                    HVX_Vector b3    = *(HVX_Vector const *)(rowB3 + kk);

                    // 四路乘加
                    vacc0 = Q6_Vsf_vadd_VsfVsf(vacc0, Q6_Vsf_vmpy_VsfVsf(a_vec, b0));
                    vacc1 = Q6_Vsf_vadd_VsfVsf(vacc1, Q6_Vsf_vmpy_VsfVsf(a_vec, b1));
                    vacc2 = Q6_Vsf_vadd_VsfVsf(vacc2, Q6_Vsf_vmpy_VsfVsf(a_vec, b2));
                    vacc3 = Q6_Vsf_vadd_VsfVsf(vacc3, Q6_Vsf_vmpy_VsfVsf(a_vec, b3));
                }
                // 写回四列
                C[i * n + j + 0] = hvx_hsum_f32(vacc0);
                C[i * n + j + 1] = hvx_hsum_f32(vacc1);
                C[i * n + j + 2] = hvx_hsum_f32(vacc2);
                C[i * n + j + 3] = hvx_hsum_f32(vacc3);
            }
            // 处理剩余列
            for (uint32_t j2 = (n / JBLK) * JBLK; j2 < n; ++j2) {
                HVX_Vector vacc = Q6_V_vzero();
                const float *rowB = BT + (size_t)j2 * k;
                for (uint32_t kk = 0; kk < k; kk += VEC) {
                    uint32_t kk_pf = kk + PF_DIST_CHUNKS * VEC;
                    if (kk_pf < k) {
                        L2fetch(rowA  + kk_pf, PF128);
                        L2fetch(rowB  + kk_pf, PF128);
                    }
                    HVX_Vector a_vec = *(HVX_Vector const *)(rowA + kk);
                    HVX_Vector b_vec = *(HVX_Vector const *)(rowB + kk);
                    vacc = Q6_Vsf_vadd_VsfVsf(vacc, Q6_Vsf_vmpy_VsfVsf(a_vec, b_vec));
                }
                C[i * n + j2] = hvx_hsum_f32(vacc);
            }
        }
        return;
    } 
    
    // 非全局对齐版本
    for (uint32_t i = 0; i < m; ++i) {
        for (uint32_t j = 0; j < n; ++j) {
            HVX_Vector vacc = Q6_V_vzero();

            uint32_t kk = 0;
            for (; kk + VEC <= k; kk += VEC) {
                HVX_Vector a_vec, b_vec;
                
                memcpy(&a_vec, &A[i * k + kk], sizeof(HVX_Vector));
                memcpy(&b_vec, &BT[j * k + kk], sizeof(HVX_Vector));

                HVX_Vector prod = Q6_Vsf_vmpy_VsfVsf(a_vec, b_vec);
                vacc = Q6_Vsf_vadd_VsfVsf(vacc, prod);
            }

            if (kk < k) {
                uint32_t r = k - kk;
                HVX_Vector a_vec = Q6_V_vzero();
                HVX_Vector b_vec = Q6_V_vzero();

                memcpy(&a_vec, &A[i * k + kk], sizeof(float) * r);
                memcpy(&b_vec, &BT[j * k + kk], sizeof(float) * r);

                HVX_Vector prod = Q6_Vsf_vmpy_VsfVsf(a_vec, b_vec);
                vacc = Q6_Vsf_vadd_VsfVsf(vacc, prod);
            }

            C[i * n + j] = hvx_hsum_f32(vacc);
        }
    }
}

```

