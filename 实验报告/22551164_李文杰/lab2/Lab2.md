# 实验二：代码纠错

## 问题与修正

1. **地址未按页大小推进**
    原代码用 `obj = (char *)freed_ct + 1;` 来移动到下一页。`freed_ct` 是 `chunk_tail_t *`，`+1` 只移动了 `sizeof(chunk_tail_t)` 字节，而不是一整页（通常 4KB），因此会出现非页对齐的地址并触发断言或日志中出现奇怪地址。
    **修复**：改为按页或按块字节数推进：`cur += (1UL << (k + __PAGE_SHIFT));`（在单页情况下就是 `cur += (1UL << __PAGE_SHIFT)`）。
2. **总是拆成 level 0**
    原循环每页都把块标为 `level = 0` 并插入 `free_head[0]`，这会把应当作为更高阶（更大）的连续块插入的空间拆成大量无法合并的小块，破坏 buddy 算法的效率与合并能力。
    **修复**：在拆分剩余页时，选择“最大且对齐的”幂次（即找最大的 `k` 使得 `1<<k <= nr_page_left` 并且当前地址 `cur` 在 `1<< (k+PAGE_SHIFT)` 字节边界上对齐），然后将该整块以 `level = k` 插入相应的 `free_head[k]`。这样保留了更大块的可能性，方便后续合并。
3. **安全地更新链表指针**
    原代码直接执行 `freed_ch->next->pprev = &freed_ch->next;`，未检查 `freed_ch->next` 是否为 `NULL`，在 `next==NULL` 时会空指针解引用。修复时加入了 `if (freed_ch->next) ...` 判断。

## code

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

#if 0
	/* Create free chunk */
	freed_ch = (chunk_head_t *)obj;
	freed_ct = (chunk_tail_t *)((char *)obj
				    + (1UL << (order + __PAGE_SHIFT))) - 1;

	/* Now, possibly we can conseal chunks together */
	while (order < FREELIST_SIZE) {
		mask = 1UL << (order + __PAGE_SHIFT);
		if ((unsigned long)freed_ch & mask) {
			to_merge_ch = (chunk_head_t *)((char *)freed_ch - mask);
			if (allocated_in_map(b, (uintptr_t)to_merge_ch)
			    || to_merge_ch->level != order)
				break;

			/* Merge with predecessor */
			freed_ch = to_merge_ch;
		} else {
			to_merge_ch = (chunk_head_t *)((char *)freed_ch + mask);
			if (allocated_in_map(b, (uintptr_t)to_merge_ch)
			    || to_merge_ch->level != order)
				break;

			/* Merge with successor */
			freed_ct =
			    (chunk_tail_t *)((char *)to_merge_ch + mask) - 1;
		}

		/* We are commited to merging, unlink the chunk */
		*(to_merge_ch->pprev) = to_merge_ch->next;
		to_merge_ch->next->pprev = to_merge_ch->pprev;

		order++;
	}

	/* Link the new chunk */
	freed_ch->level = order;
	freed_ch->next = b->free_head[order];
	freed_ch->pprev = &b->free_head[order];
	freed_ct->level = order;

	freed_ch->next->pprev = &freed_ch->next;
	b->free_head[order] = freed_ch;
#else
	/*
	 * Corrected simple splitter: instead of always inserting level-0 pages
	 * we insert the largest power-of-two aligned chunks possible so the
	 * free list can later merge them. Also fix pointer arithmetic when
	 * advancing `obj` (must advance by whole pages, not by sizeof tail).
	 */
	unsigned long nr_page_left = 1UL << order; /* number of pages to free */
	char *cur = (char *)obj;

	while (nr_page_left) {
		/* choose largest k such that (1<<k) <= nr_page_left and cur is aligned */
		int k = 0;
		/* find highest bit */
		while ((1UL << (k + 1)) <= nr_page_left)
			k++;

		/* reduce k until cur is aligned to that chunk size */
		while (k > 0) {
			unsigned long chunk_bytes = 1UL << (k + __PAGE_SHIFT);
			if (((uintptr_t)cur & (chunk_bytes - 1)) == 0)
				break;
			k--;
		}

		/* Now cur is aligned for a chunk of 1<<k pages */
		freed_ch = (chunk_head_t *)cur;
		freed_ct = (chunk_tail_t *)(cur + (1UL << (k + __PAGE_SHIFT))) - 1;

		freed_ch->level = k;
		/* insert into free list for level k */
		freed_ch->next = b->free_head[k];
		freed_ch->pprev = &b->free_head[k];
		if (freed_ch->next)
			freed_ch->next->pprev = &freed_ch->next;
		b->free_head[k] = freed_ch;

		freed_ct->level = k;

		/* advance */
		nr_page_left -= (1UL << k);
		cur += (1UL << (k + __PAGE_SHIFT));
	}
#endif
	freelist_sanitycheck(b->free_head);

	uk_bbpalloc_dump_freelist();
}
```

