static void bbuddy_pfree(struct uk_alloc *a, void *obj, unsigned long num_pages)
{
	struct uk_bbpalloc *b;
	chunk_head_t *freed_ch;
	chunk_tail_t *freed_ct;
	unsigned long nr_page_left;

	UK_ASSERT(a != NULL);
	uk_alloc_stats_count_pfree(a, obj, num_pages);
	b = (struct uk_bbpalloc *)&a->priv;

	freelist_sanitycheck(b->free_head);

	UK_ASSERT((((uintptr_t)obj) & (__PAGE_SIZE - 1)) == 0);
	map_free(b, (uintptr_t)obj, num_pages);

	nr_page_left = num_pages;
	while (nr_page_left--) {
		freed_ch = (chunk_head_t *)obj;
		freed_ct = (chunk_tail_t *)((char *)obj + (1UL << __PAGE_SHIFT)) - 1;

		freed_ch->level = 0;
		freed_ct->level = 0;

		freed_ch->next = b->free_head[0];
		if (freed_ch->next)
			freed_ch->next->pprev = &freed_ch->next;
		freed_ch->pprev = &b->free_head[0];
		b->free_head[0] = freed_ch;

		obj = (char *)obj + (1UL << __PAGE_SHIFT);
	}

	freelist_sanitycheck(b->free_head);
	uk_bbpalloc_dump_freelist();
}
