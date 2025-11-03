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
