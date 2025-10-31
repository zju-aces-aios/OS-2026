# Lab2 实验报告



## 代码问题

### 1 地址未对齐

```c
obj = (char *)freed_ct + 1;
```

`freed_ct` 是页尾处的 `chunk_tail_t*`，`+1` 只前进 **1 字节**，不是一页。导致日志里出现非4KB对齐的奇怪地址。



### 2 只拆成0阶页

循环里每次都：

```
freed_ct->level = 0;
b->free_head[0] = freed_ch;
```

把本来应该回收到高阶的块拆成一堆无法合并的0阶页。





## 代码

```c
static void bbuddy_pfree(struct uk_alloc *a, void *obj, unsigned long num_pages)
{
	struct uk_bbpalloc *b;
	chunk_head_t *freed_ch, *to_merge_ch;
	chunk_tail_t *freed_ct;
	unsigned long mask;
  
	UK_ASSERT(a != NULL);

	uk_alloc_stats_count_pfree(a, obj, num_pages);
	b = (struct uk_bbpalloc *)&a->priv;

	freelist_sanitycheck(b->free_head);

	size_t order = (size_t)num_pages_to_order(num_pages);

	/* if the object is not page aligned it was clearly not from us */
	UK_ASSERT((((uintptr_t)obj) & (__PAGE_SIZE - 1)) == 0);

	/* First free the chunk */
	map_free(b, (uintptr_t)obj, 1UL << order);
  
#if 1
	int nr_page_left = 1; 
	while (nr_page_left) {
		freed_ch = (chunk_head_t *)obj;
		freed_ct = (chunk_tail_t *)((char *)obj
			    + (1UL << (order + __PAGE_SHIFT))) - 1;

		freed_ch->level = order;
		freed_ch->next  = b->free_head[order];
		freed_ch->pprev = &b->free_head[order];
		freed_ct->level = order;

        freed_ch->next->pprev = &freed_ch->next;
		b->free_head[order] = freed_ch;

		nr_page_left--;
		obj = (char *)obj + (1UL << (order + __PAGE_SHIFT));
	}
#endif
	freelist_sanitycheck(b->free_head);

}
```



## 调试思路

分配 32KB 内存，打印日志观察。很明显发现0阶存在很多不连续的地址。



原版：

```
==========【bbuddy_pfree start】===========
Dumping current state of the free list:
Free list 0 (Order 0):
  Entry at address: 0x44103000, end: 0x44104000, level: 0
Free list 1 (Order 1):
  Entry at address: 0x40192000, end: 0x40194000, level: 1
Free list 2 (Order 2):
  Entry at address: 0x44104000, end: 0x44108000, level: 2
  Entry at address: 0x40194000, end: 0x40198000, level: 2
  Entry at address: 0x40014000, end: 0x40018000, level: 2
Free list 3 (Order 3):
  Entry at address: 0x44108000, end: 0x44110000, level: 3
  Entry at address: 0x40198000, end: 0x401a0000, level: 3
  Entry at address: 0x40018000, end: 0x40020000, level: 3
Free list 4 is empty.
Free list 5 is empty.
Free list 6 (Order 6):
  Entry at address: 0x44140000, end: 0x44180000, level: 6
  Entry at address: 0x401c0000, end: 0x40200000, level: 6
  Entry at address: 0x40040000, end: 0x40080000, level: 6
Free list 7 (Order 7):
  Entry at address: 0x44180000, end: 0x44200000, level: 7
  Entry at address: 0x40080000, end: 0x40100000, level: 7
Free list 8 is empty.
Free list 9 (Order 9):
  Entry at address: 0x44200000, end: 0x44400000, level: 9
  Entry at address: 0x40200000, end: 0x40400000, level: 9
==========【bbuddy_pfree end】===========
Dumping current state of the free list:
Free list 0 (Order 0):
  Entry at address: 0x44103000, end: 0x44104000, level: 0
Free list 1 (Order 1):
  Entry at address: 0x40192000, end: 0x40194000, level: 1
Free list 2 (Order 2):
  Entry at address: 0x44104000, end: 0x44108000, level: 2
  Entry at address: 0x40194000, end: 0x40198000, level: 2
  Entry at address: 0x40014000, end: 0x40018000, level: 2
Free list 3 (Order 3):
  Entry at address: 0x44108000, end: 0x44110000, level: 3
  Entry at address: 0x40198000, end: 0x401a0000, level: 3
  Entry at address: 0x40018000, end: 0x40020000, level: 3
Free list 4 (Order 4):
  Entry at address: 0x44110000, end: 0x44120000, level: 4
Free list 5 is empty.
Free list 6 (Order 6):
  Entry at address: 0x44140000, end: 0x44180000, level: 6
  Entry at address: 0x401c0000, end: 0x40200000, level: 6
  Entry at address: 0x40040000, end: 0x40080000, level: 6
Free list 7 (Order 7):
  Entry at address: 0x44180000, end: 0x44200000, level: 7
  Entry at address: 0x40080000, end: 0x40100000, level: 7
Free list 8 is empty.
Free list 9 (Order 9):
  Entry at address: 0x44200000, end: 0x44400000, level: 9
  Entry at address: 0x40200000, end: 0x40400000, level: 9
```

改版：

```
==========【bbuddy_pfree start】===========
Dumping current state of the free list:
Free list 0 (Order 0):
  Entry at address: 0x44103000, end: 0x44104000, level: 0
Free list 1 (Order 1):
  Entry at address: 0x40192000, end: 0x40194000, level: 1
Free list 2 (Order 2):
  Entry at address: 0x44104000, end: 0x44108000, level: 2
  Entry at address: 0x40194000, end: 0x40198000, level: 2
  Entry at address: 0x40014000, end: 0x40018000, level: 2
Free list 3 (Order 3):
  Entry at address: 0x44108000, end: 0x44110000, level: 3
  Entry at address: 0x40198000, end: 0x401a0000, level: 3
  Entry at address: 0x40018000, end: 0x40020000, level: 3
Free list 4 is empty.
Free list 5 is empty.
Free list 6 (Order 6):
  Entry at address: 0x44140000, end: 0x44180000, level: 6
  Entry at address: 0x401c0000, end: 0x40200000, level: 6
  Entry at address: 0x40040000, end: 0x40080000, level: 6
Free list 7 (Order 7):
  Entry at address: 0x44180000, end: 0x44200000, level: 7
  Entry at address: 0x40080000, end: 0x40100000, level: 7
Free list 8 is empty.
Free list 9 (Order 9):
  Entry at address: 0x44200000, end: 0x44400000, level: 9
  Entry at address: 0x40200000, end: 0x40400000, level: 9
==========【bbuddy_pfree end】===========
Dumping current state of the free list:
Free list 0 (Order 0):
  Entry at address: 0x4411efd3, end: 0x4411ffd3, level: 0
  Entry at address: 0x4411dfd6, end: 0x4411efd6, level: 0
  Entry at address: 0x4411cfd9, end: 0x4411dfd9, level: 0
  Entry at address: 0x4411bfdc, end: 0x4411cfdc, level: 0
  Entry at address: 0x4411afdf, end: 0x4411bfdf, level: 0
  Entry at address: 0x44119fe2, end: 0x4411afe2, level: 0
  Entry at address: 0x44118fe5, end: 0x44119fe5, level: 0
  Entry at address: 0x44117fe8, end: 0x44118fe8, level: 0
  Entry at address: 0x44116feb, end: 0x44117feb, level: 0
  Entry at address: 0x44115fee, end: 0x44116fee, level: 0
  Entry at address: 0x44114ff1, end: 0x44115ff1, level: 0
  Entry at address: 0x44113ff4, end: 0x44114ff4, level: 0
  Entry at address: 0x44112ff7, end: 0x44113ff7, level: 0
  Entry at address: 0x44111ffa, end: 0x44112ffa, level: 0
  Entry at address: 0x44110ffd, end: 0x44111ffd, level: 0
  Entry at address: 0x44110000, end: 0x44111000, level: 0
  Entry at address: 0x44103000, end: 0x44104000, level: 0
Free list 1 (Order 1):
  Entry at address: 0x40192000, end: 0x40194000, level: 1
Free list 2 (Order 2):
  Entry at address: 0x44104000, end: 0x44108000, level: 2
  Entry at address: 0x40194000, end: 0x40198000, level: 2
  Entry at address: 0x40014000, end: 0x40018000, level: 2
Free list 3 (Order 3):
  Entry at address: 0x44108000, end: 0x44110000, level: 3
  Entry at address: 0x40198000, end: 0x401a0000, level: 3
  Entry at address: 0x40018000, end: 0x40020000, level: 3
Free list 4 is empty.
Free list 5 is empty.
Free list 6 (Order 6):
  Entry at address: 0x44140000, end: 0x44180000, level: 6
  Entry at address: 0x401c0000, end: 0x40200000, level: 6
  Entry at address: 0x40040000, end: 0x40080000, level: 6
Free list 7 (Order 7):
  Entry at address: 0x44180000, end: 0x44200000, level: 7
  Entry at address: 0x40080000, end: 0x40100000, level: 7
Free list 8 is empty.
Free list 9 (Order 9):
  Entry at address: 0x44200000, end: 0x44400000, level: 9
  Entry at address: 0x40200000, end: 0x40400000, level: 9
```

