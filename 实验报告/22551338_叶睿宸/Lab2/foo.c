static void bbuddy_pfree(struct uk_alloc *a, void *obj, unsigned long num_pages)
{
    struct uk_bbpalloc *b;
    chunk_head_t *freed_ch;
    chunk_tail_t *freed_ct;

    UK_ASSERT(a != NULL);

    uk_alloc_stats_count_pfree(a, obj, num_pages);
    b = (struct uk_bbpalloc *)&a->priv;

    freelist_sanitycheck(b->free_head);

    size_t order = (size_t)num_pages_to_order(num_pages);

    /* if the object is not page aligned it was clearly not from us */
    UK_ASSERT((((uintptr_t)obj) & (__PAGE_SIZE - 1)) == 0);

    /* First free the chunk */
    map_free(b, (uintptr_t)obj, 1UL << order);

    unsigned long nr_page_left = 1UL << order; /* 修改：使用无符号长整型类型 */
    while (nr_page_left) {
        freed_ch = (chunk_head_t *)obj;
        freed_ct = (chunk_tail_t *)((char *)obj
                    + (1UL << __PAGE_SHIFT)) - 1;

        freed_ch->level = 0;
        freed_ch->next = b->free_head[0];
        freed_ch->pprev = &b->free_head[0];
        freed_ct->level = 0;

        if (freed_ch->next)
            freed_ch->next->pprev = &freed_ch->next;
        b->free_head[0] = freed_ch;

        nr_page_left--;
        /* 修改：按页面大小前进，而不是 +1 字节 */
        obj = (char *)obj + (1UL << __PAGE_SHIFT);
    }
    freelist_sanitycheck(b->free_head);

    uk_bbpalloc_dump_freelist();
}