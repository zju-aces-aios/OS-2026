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

- 修改后的代码片段,保存为一个文件foo.c，不要有多余内容


核心错误 1：obj 地址推进计算错误
错误描述：
多页释放循环中，obj 地址仅前进 1 字节，未按页大小跳转，导致下一页处理地址错位。
错误原因：
原始代码 obj = (char *) freed_ct + 1 中，freed_ct 指向当前页尾元数据 chunk_tail_t，+1 仅移动 1 字节，未到达下一页起始地址，后续页面元数据处理异常。
解决方法：
按页大小推进地址：利用 __PAGE_SHIFT 计算页大小（1UL << __PAGE_SHIFT 等价于 __PAGE_SIZE），从当前页起始地址 obj 直接偏移 1 个页大小。

核心错误 2：双向链表操作双错误（链表紊乱 + 空指针崩溃）
错误描述：
反向指针 pprev 错误指向当前节点的 next 成员地址，而非当前节点；
未判断 freed_ch->next 是否为 NULL，空链表场景下触发空指针解引用崩溃。
错误原因：
双向链表逻辑错误：pprev 应存储前一个节点的起始地址，而非节点成员地址，否则链表遍历失败；
边界场景未处理：当 Level 0 空闲链表为空时，freed_ch->next 为 NULL，直接访问其 pprev 字段会导致崩溃。
解决方法：
新增空指针判断：仅当 freed_ch->next 非空时，才更新其 pprev 字段；
修正指针指向：将 freed_ch->next->pprev 指向当前节点 freed_ch，符合双向链表规范。