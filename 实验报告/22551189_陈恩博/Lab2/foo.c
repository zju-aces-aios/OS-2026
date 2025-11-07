void uk_bbpalloc_dump_freelist()
{
	struct uk_alloc *a = uk_alloc_get_default();
    struct uk_bbpalloc *b = (struct uk_bbpalloc *)&a->priv;
    chunk_head_t *entry;
    unsigned long i;

    uk_pr_crit("Dumping current state of the free list:\n");

    /* Traverse through all the free lists */
    for (i = 0; i < 1; i++) {
        if (b->free_head[i] == &b->free_tail[i]) {
            uk_pr_crit("Free list %lu is empty.\n", i);
        } else {
            uk_pr_crit("Free list %lu (Order %lu):\n", i, i);
            entry = b->free_head[i];
			int sum = 0;
            while (entry != &b->free_tail[i]) {
                uk_pr_crit("  Entry at address: %p, end: %p, level: %u\n", 
                           entry, (unsigned int)entry + (__PAGE_SIZE << i), entry->level);
                entry = entry->next;
				sum ++;
				if (sum == 10) {
					break;
				}
            }
        }
    }
}

static void *bbuddy_palloc(struct uk_alloc *a, unsigned long num_pages)
{
	uk_pr_crit("BEFORE bbuddy_palloc %lu page(s)\n", num_pages);
	uk_bbpalloc_dump_freelist();
	
	struct uk_bbpalloc *b;
	size_t i;
	chunk_head_t *alloc_ch, *spare_ch;
	chunk_tail_t *spare_ct;

	UK_ASSERT(a != NULL);
	b = (struct uk_bbpalloc *)&a->priv;

	freelist_sanitycheck(b->free_head);

	size_t order = (size_t)num_pages_to_order(num_pages);

#if 0
	/* Find smallest order which can satisfy the request. */
	for (i = order; i < FREELIST_SIZE; i++) {
		if (!FREELIST_EMPTY(b->free_head[i]))
			break;
	}
	if (i >= FREELIST_SIZE)
		goto no_memory;

	/* Unlink a chunk. */
	alloc_ch = b->free_head[i];
	b->free_head[i] = alloc_ch->next;
	alloc_ch->next->pprev = alloc_ch->pprev;

	/* We may have to break the chunk a number of times. */
	while (i != order) {
		/* Split into two equal parts. */
		i--;
		spare_ch = (chunk_head_t *)((char *)alloc_ch
					    + (1UL << (i + __PAGE_SHIFT)));
		spare_ct = (chunk_tail_t *)((char *)spare_ch
					    + (1UL << (i + __PAGE_SHIFT))) - 1;

		/* Create new header for spare chunk. */
		spare_ch->level = i;
		spare_ch->next = b->free_head[i];
		spare_ch->pprev = &b->free_head[i];
		spare_ct->level = i;

		/* Link in the spare chunk. */
		spare_ch->next->pprev = &spare_ch->next;
		b->free_head[i] = spare_ch;
	}

	UK_ASSERT(FREELIST_ALIGNED(alloc_ch, order));
	map_alloc(b, (uintptr_t)alloc_ch, order);

	uk_alloc_stats_count_palloc(a, (void *) alloc_ch, num_pages);
	freelist_sanitycheck(b->free_head);
#else
	chunk_head_t *pred_ch, *curr_ch, *high_ch, *low_ch;
	curr_ch = b->free_head[0];
	high_ch = curr_ch;
	i = 1;

	// uk_pr_crit("0x%016lx 0x%016lx\n", curr_ch, &b->free_tail[0]);
	while(i < num_pages && (curr_ch->next != &b->free_tail[0])) {
		pred_ch = curr_ch;
		curr_ch = curr_ch->next;
		if ((unsigned long) curr_ch + __PAGE_SIZE != (unsigned long) pred_ch) {
			i = 0;
			alloc_ch = curr_ch;
		}
		i += 1;
		// uk_pr_crit("curr 0x%016lx\n", curr_ch);
	}

	if (i != num_pages) {
		goto no_memory;
	}
	low_ch = curr_ch;

	*high_ch->pprev = low_ch->next;
	low_ch->next->pprev = high_ch->pprev;

	uk_pr_crit("ALLOC 0x%016lx - 0x%016lx %d\n", low_ch, (size_t) high_ch + __PAGE_SIZE, num_pages);
	
	map_alloc(b, (uintptr_t)low_ch, num_pages);

	uk_alloc_stats_count_palloc(a, (void *) low_ch, num_pages);
	freelist_sanitycheck(b->free_head);

	alloc_ch = low_ch;
#endif

	uk_pr_crit("AFTER bbuddy_palloc %lu page(s)\n", num_pages);
	uk_bbpalloc_dump_freelist();

	return ((void *)alloc_ch);

no_memory:
	uk_pr_warn("%"__PRIuptr": Cannot handle palloc request of order %"__PRIsz": Out of memory\n",
		   (uintptr_t)a, order);

	uk_alloc_stats_count_penomem(a, num_pages);
	errno = ENOMEM;
	return NULL;
}

static void bbuddy_pfree(struct uk_alloc *a, void *obj, unsigned long num_pages)
{
	uk_bbpalloc_dump_freelist();
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
	map_free(b, (uintptr_t)obj, num_pages);

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

	chunk_head_t *entry;
	entry = b->free_head[0];
	chunk_head_t *target_entry;
	while (entry != &b->free_tail[0]) {
		uk_pr_crit("  Entry at address: %p, end: %p, level: %u\n", 
					entry, (unsigned int)entry + (__PAGE_SIZE << 0), entry->level);
		if (entry < (chunk_head_t *)obj) {
			target_entry = entry;
			break;
		}
		uk_pr_crit("entry->next %p\n", entry->next);
		entry = entry->next;
	}
	
	freed_ch = (chunk_head_t *)obj;
	freed_ct = (chunk_tail_t *)(obj	+ (1UL << __PAGE_SHIFT)) - 1;
	freed_ch->level = 0;
	
	freed_ch->next = target_entry;
	freed_ch->pprev = target_entry->pprev;
	*target_entry->pprev = freed_ch;
	target_entry->pprev = &freed_ch->next;

#endif
	freelist_sanitycheck(b->free_head);

	uk_bbpalloc_dump_freelist();
}
