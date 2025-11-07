# Lab4: Hexagon NPU GEMM

## 任务目标

要求在 `Lab4/dsp/calculator_imp.c` 中实现并比较多种矩阵乘法实现：

- 朴素标量实现（baseline）：直接三重循环实现的 C 语言矩阵乘法，用作基线性能对比；
- 基于 HVX 的内积实现（A * B^T）：对 B 做转置，使得点积（dot-product）可以用向量化内积（内积法）计算；
- 基于 HVX 的外积实现（A * B）：采用外积法，利用标量广播将 A 的单个元素与 B 的一整段向量相乘并累加到 C 的子向量。

实现要求与验收准则：

1. 功能等价：对任意合法输入（浮点矩阵）都应输出误差在浮点容差内的结果；
2. 向量化与对齐：HVX 代码应处理 128 字节（32 float）对齐，说明如何处理尾部不对齐或非 32 倍长度的情形；
3. 性能测量：对不同矩阵尺寸执行并记录；
4. 结果分析：比较三种实现的运行时间与计算效率，并说明在实现中使用到的主要 HVX 指令与它们的作用（例如 vsplat、vmpy、vadd、vror 等）。

实验数据记录表：

- 无论是实体设备还是使用模拟器，启动参数的最后三位分别是矩阵尺寸的M、K、N

| 实验编号 | 实现方式 | 设备/模拟器 | 矩阵尺寸 (M×K×N) | 计算耗时 (ms) | 备注 |
|---:|---|---|---:|---:|---:|
| 1 | 朴素 baseline |  | 64×64×64 | 42.493   50.347  |  |
| 2 | HVX 内积 (A * B^T) |  | 64×64×64 | 44.871  |  |
| 3 | HVX 外积 (A * B) |  | 64×64×64 |  38.353 |  |
| 4 | 朴素 baseline |  | 256×256×256 |  614.380 472.621 |  |
| 5 | HVX 内积 (A * B^T) |  | 256×256×256 | 187.06  |  |
| 6 | HVX 外积 (A * B) |  | 256×256×256 |  143.410 |  |
| 7 | 朴素 baseline |  | 512×512×512 | 6254.155 3590.242  |  |
| 8 | HVX 内积 (A * B^T) |  | 512×512×512 | 961.447  |  |
| 9 | HVX 外积 (A * B) |  | 512×512×512 |  693.468 |  |
| 10 | 朴素 baseline |  | 88×99×66 |  57.371  61.493 |  |
| 11 | HVX 内积 (A * B^T) |  | 88×99×66 |  46.951 |  |
| 12 | HVX 外积 (A * B) |  | 88×99×66 |  57.664 |  |

分析要点：

1. 对比内积与外积在数据复用、内存访问模式与向量指令使用上的差异；
2. 关键 HVX 指令详解：指出在代码中使用到的每种 HVX 指令（例如 Q6_V_vsplat_R、Q6_Vqf32_vmpy_VsfVsf、Q6_Vqf32_vadd_Vqf32Vqf32、Q6_V_vror_VR 等）并解释它们在你的实现中如何改善性能；
3. 针对尾部、对齐、缓存与内存带宽瓶颈提出优化建议。

延伸讨论（可选）：

- 翻阅硬件手册，查看l2fetch函数的作用，并尝试用在代码实现中，观察变化
- 如果采用了其他的优化手段，请在提交文档中标明

## 提交内容

- 修改后的`calculator_imp.c`
- 实验报告



分析要点：

1、对比内积与外积在数据复用、内存访问模式与向量指令使用上的差异

1.1内积

```c
static inline void matmul_ikj_transposed_b(float *restrict input_matrix1,
                                     float *restrict input_matrix2,
                                     float *restrict output,
                                     uint32_t m,
                                     uint32_t k,
                                     uint32_t n) {
	for (int i = 0; i < m; i++) {
		for (int j = 0; j < n; j++) {
            int u=0;
            HVX_Vector acc = Q6_V_vzero();
            for(u=0;u<k/32;u++){
                HVX_Vector vector1,vector2;
                memcpy(&vector1, &input_matrix1[i * k + u * 32], 32 * sizeof(float));
                memcpy(&vector2, &input_matrix2[j * k + u * 32], 32 * sizeof(float));
                HVX_Vector mul = Q6_Vqf32_vmpy_VsfVsf(vector1, vector2);
                acc = Q6_Vqf32_vadd_Vqf32Vqf32(acc, mul);
            }
            int remainder = k % 32;
            if(remainder>0){
                HVX_Vector vector1=Q6_V_vzero();
                HVX_Vector vector2=Q6_V_vzero();
                memcpy(&vector1, &input_matrix1[i * k + u * 32], remainder * sizeof(float));
                memcpy(&vector2, &input_matrix2[j * k + u * 32], remainder * sizeof(float));
                HVX_Vector mul = Q6_Vqf32_vmpy_VsfVsf(vector1, vector2);
                acc = Q6_Vqf32_vadd_Vqf32Vqf32(acc, mul);
            }
            for(int q=16;q>=1;q>>=1){
                HVX_Vector vector = Q6_V_vror_VR(acc, q * sizeof(float));
                acc= Q6_Vqf32_vadd_Vqf32Vqf32(acc, vector);
            }
            HVX_Vector acc_sf = Q6_Vsf_equals_Vqf32(acc);
            float result;
            memcpy(&result, &acc_sf, sizeof(float));
            output[i * n + j]+=result;
		}
	}
	return;
}
```

1.2外积

```c
static inline void matmul_ijk(float *restrict input_matrix1,
                 float *restrict input_matrix2,
                 float *restrict output,
                 uint32_t m,
                 uint32_t k,
                 uint32_t n) {
    int num=(k-1)/32+1;
    HVX_Vector acc[num];
    for(int i=0;i<m;i++){
        for (int nums = 0; nums < num; nums++) {
            acc[nums] = Q6_V_vzero();
        }
        for(int j=0;j<k;j++){
            int z=0;
            for(z=0;z<n/32;z++){
                HVX_Vector vec_1=Q6_V_vsplat_R(float_to_bits(input_matrix1[i * k + j]));
                HVX_Vector vec_2;
                memcpy(&vec_2, &input_matrix2[j * n + z * 32], 32 * sizeof(float));
                HVX_Vector mul = Q6_Vqf32_vmpy_VsfVsf(vec_1, vec_2);
                acc[z] = Q6_Vqf32_vadd_Vqf32Vqf32(acc[z], mul);
            }
            int remainder = n % 32;
            if(remainder>0){
                HVX_Vector vec_1=Q6_V_vsplat_R(float_to_bits(input_matrix1[i * k + j]));
                HVX_Vector vec_2=Q6_V_vzero();
                memcpy(&vec_2, &input_matrix2[j * n + z * 32], remainder * sizeof(float));
                HVX_Vector mul = Q6_Vqf32_vmpy_VsfVsf(vec_1, vec_2);
                acc[z] = Q6_Vqf32_vadd_Vqf32Vqf32(acc[z], mul);  
            }

        }
        int z=0;
        for(z=0;z<n/32;z++){
            HVX_Vector res = Q6_Vsf_equals_Vqf32(acc[z]);
            memcpy(&output[i * n + z * 32], &res, 32 * sizeof(float));
        }
        int remainder = n % 32;
        if(remainder>0){
            HVX_Vector res = Q6_Vsf_equals_Vqf32(acc[z]);
            memcpy(&output[i * n + z * 32], &res, remainder * sizeof(float));
        }
        
    }

	return;
}

```

数据复用：在外积采用将input_matrix1的每行进行复用来加上与input_matrix1上做外积的值，避免重复加载，内积则是采取扩展标量的方法实现复用。

内存访问模式：两者都是采用连续内存访问。

向量指令：内积相比于外积多用Q6_V_vror_VR，来实现HVX_Vector中的数据移动，来实现数据求和。

2.关键 HVX 指令详解：指出在代码中使用到的每种 HVX 指令（例如 Q6_V_vsplat_R、Q6_Vqf32_vmpy_VsfVsf、Q6_Vqf32_vadd_Vqf32Vqf32、Q6_V_vror_VR 等）并解释它们在你的实现中如何改善性能；

2.1 Q6_V_vzero()

功能: 创建一个所有位都为 0 的 HVX 向量。
在你的代码中: 用于在开始累加前初始化累加器向量 acc。这是保证计算结果正确的关键第一步。若无此步，计算有可能将基于内存垃圾值进行。
2.2 Q6_V_vsplat_R(float_to_bits(float_val))

功能: "Splat" (广播) 指令。它将一个 32 位的标量值（这里是你转换后的 float）复制 32 次，填满整个 128 字节的 HVX 向量。
是外积法 matmul_ijk 的灵魂。通过将 需要的数值广播成一个向量，你能够用一次向量乘法 vmpy 就完成乘法，极大地提升了性能。
2.3 Q6_Vqf32_vmpy_VsfVsf(vec_a, vec_b)

功能: 向量浮点乘法。它将两个输入向量 vec_a 和 vec_b 的对应元素（32对 float）逐个相乘，生成一个包含 32 个乘积的结果向量。
在你的代码中: 这是两种算法进行核心计算的指令。在内积法中，它计算点积的部分乘积；在外积法中，它执行标量-向量乘法。这是并行计算能力的主要体现。
2.4 Q6_Vqf32_vadd_Vqf32Vqf32(acc, mul)

功能: 向量浮点加法。将两个输入向量的对应元素相加。
在你的代码中: 用于累加结果。无论是内积法累加点积的部分和，还是外积法累加外积的部分和，vadd 都以并行方式高效地更新累加器 acc，避免了逐元素操作的巨大开销。
2.5 Q6_V_vror_VR(vec, bytes)

功能: 向量循环右移。将整个 1024 位的向量 vec 向右循环移动指定的 bits 数。

在你的代码中: 这是内积法 matmul_ikj_transposed_b 中水平求和的关键。通过巧妙地设置移位距离（例如，向量长度的一半、1/4、1/8...），并与 vadd 配合，可以高效地将向量中的所有元素求和到第一个元素位置。

2.6 Q6_Vsf_equals_Vqf32(vec)

功能: 类型/格式转换。确保向量 vec 的内部表示对于后续的内存拷贝或标量提取是正确的。在将向量 acc 的内容写回内存或提取单个 float 之前，使用此指令来保证数据格式的正确性。

3.针对尾部、对齐、缓存与内存带宽瓶颈提出优化建议。

3.1尾部处理

当前我代码中主要还是采用较为粗糙的判断是否能整除32来判断尾部是否需要特殊处理，如果需要，则将其扩充到32进行处理，处理流程和之前一样。

优化建议：对于非常短的尾部（例如少于4个元素），直接用 C 语言的 for 循环进行标量计算，可能比设置和使用向量指令更快，因为它避免了向量操作的固定开销。

3.2对齐
优化建议：使用对齐的内存分配: 在你的主 C/C++ 代码中，使用 memalign 或 posix_memalign 来分配 input_matrix1、input_matrix2 和 output，确保它们的基地址是 128 字节对齐的。

3.3内存带宽瓶颈

优化建议：

分块： 这是最重要的矩阵乘法优化。不要一次处理整个矩阵，而是将 A, B, C 矩阵切分成小的子块 (sub-matrices)，大小以能完全放入 L2 甚至 L1 缓存为宜（例如 32x32 或 64x64）。然后对这些小块进行矩阵乘法。这可以最大化数据复用，将对主内存的访问降到最低。

