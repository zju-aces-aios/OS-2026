# 实验二：代码纠错

## 找出代码片段中的2处问题并改正

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
	int nr_page_left = 1UL << order;
	while(nr_page_left) {
		freed_ch = (chunk_head_t *)obj;
		freed_ct = (chunk_tail_t *)((char *)obj
				    + (1UL << __PAGE_SHIFT)) - 1;

		freed_ch->level = 0;
		freed_ch->next = b->free_head[0];
		freed_ch->pprev = &b->free_head[0];
		freed_ct->level = 0;

		freed_ch->next->pprev = &freed_ch->next;
		b->free_head[0] = freed_ch;

		nr_page_left--;
		obj = (char *) freed_ct + 1;
	}
#endif
	freelist_sanitycheck(b->free_head);

	uk_bbpalloc_dump_freelist();
}
```

## 提交内容

- Markdown文档简洁、准确地说明问题
	- nr_page_left 类型错误：原代码用 int 存放 1UL << order，可能溢出并与其它无符号类型混用。应改为 unsigned long（或 size_t）。
	- obj 前进计算错误：原代码用 obj = (char *)freed_ct + 1; 这只前进 1 字节，正确应按页面大小前进（1UL << __PAGE_SHIFT），否则下次循环地址错位、越界。

- 修改后的代码片段,保存为一个文件foo.c，不要有多余内容