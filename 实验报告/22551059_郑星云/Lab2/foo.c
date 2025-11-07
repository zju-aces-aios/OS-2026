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