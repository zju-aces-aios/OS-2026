static void bbuddy_pfree(struct uk_alloc *a, void *obj, unsigned long num_pages)
{
        struct uk_bbpalloc *b;
        chunk_head_t *freed_ch, *to_merge_ch;
        chunk_tail_t *freed_ct;
        unsigned long mask;
        uk_pr_err("==========【bbuddy_pfree start】===========\n");
        uk_bbpalloc_dump_freelist();
        UK_ASSERT(a != NULL);
        uk_alloc_stats_count_pfree(a, obj, num_pages);
        b = (struct uk_bbpalloc *)&a->priv;
        freelist_sanitycheck(b->free_head);
        size_t order = (size_t)num_pages_to_order(num_pages);
        UK_ASSERT((((uintptr_t)obj) & (__PAGE_SIZE - 1)) == 0);
        map_free(b, (uintptr_t)obj, 1UL << order);
#if 0
        freed_ch = (chunk_head_t *)obj;
        freed_ct = (chunk_tail_t *)((char *)obj
                                    + (1UL << (order + __PAGE_SHIFT))) - 1;
        while (order < FREELIST_SIZE) {
                mask = 1UL << (order + __PAGE_SHIFT);
                if ((unsigned long)freed_ch & mask) {
                        to_merge_ch = (chunk_head_t *)((char *)freed_ch - mask);
                        if (allocated_in_map(b, (uintptr_t)to_merge_ch)
                            || to_merge_ch->level != order)
                                break;
                        freed_ch = to_merge_ch;
                } else {
                        to_merge_ch = (chunk_head_t *)((char *)freed_ch + mask);
                        if (allocated_in_map(b, (uintptr_t)to_merge_ch)
                            || to_merge_ch->level != order)
                                break;
                        freed_ct =
                            (chunk_tail_t *)((char *)to_merge_ch + mask) - 1;
                }
                *(to_merge_ch->pprev) = to_merge_ch->next;
                to_merge_ch->next->pprev = to_merge_ch->pprev;
                order++;
        }
        freed_ch->level = order;
        freed_ch->next = b->free_head[order];
        freed_ch->pprev = &b->free_head[order];
        freed_ct->level = order;
        freed_ch->next->pprev = &freed_ch->next;
        b->free_head[order] = freed_ch;
#else
        /*核心思路，先整块添加到对应level，再考虑合并问题（其实正确写法就在IF 0那里……为了长得和原始代码不那么像，改变了一些写法），这里由于整块添加顺手消掉了(char *)freed_ct + 1的指针错误*/
        freed_ch = (chunk_head_t *)obj;
        freed_ct = (chunk_tail_t *)((char *)obj
                                    + (1UL << (order + __PAGE_SHIFT))) - 1;
        freed_ch->level = order;
        freed_ch->next = b->free_head[order];
        freed_ch->pprev = &b->free_head[order];
        freed_ct->level = order;
        freed_ch->next->pprev = &freed_ch->next;
        b->free_head[order] = freed_ch;
        /*进入循环，尝试将加入空闲列表的块与其伙伴进行合并。（与IF 0的原始实现部分流程不太相同，其实也是没活硬整）*/
        size_t current_order = order;
        chunk_head_t *current_block_ch = freed_ch;
        while (current_order < FREELIST_SIZE) {
                unsigned long current_size = 1UL << (current_order + __PAGE_SHIFT);
                /* 使用 XOR 找到伙伴的地址*/
                uintptr_t buddy_addr = (uintptr_t)current_block_ch ^ current_size;
                chunk_head_t *buddy_ch = (chunk_head_t *)buddy_addr;
                /* 检查伙伴是否空闲且 order 相同 */
                if (allocated_in_map(b, buddy_addr) || buddy_ch->level != current_order) {
                        /* 伙伴不可用，停止合并 */
                        break;
                }
                /*找到了可合并的伙伴，从free_head[current_order]摘除自己和伙伴*/
                *(current_block_ch->pprev) = current_block_ch->next;
                current_block_ch->next->pprev = current_block_ch->pprev;
                *(buddy_ch->pprev) = buddy_ch->next;
                buddy_ch->next->pprev = buddy_ch->pprev;
                /*准备下一轮循环：确定合并后新块的起始地址*/
                if (buddy_addr < (uintptr_t)current_block_ch) {
                        current_block_ch = buddy_ch; 
                }
                current_order++; 
                /*将新合并得到的大块添加到高一级的空闲链表中，以便支持继续进行合并*/
                current_block_ch->level = current_order;
                current_block_ch->next = b->free_head[current_order];
                current_block_ch->pprev = &b->free_head[current_order];
                /* 更新新块的尾部 */
                chunk_tail_t *new_ct = (chunk_tail_t *)((char *)current_block_ch
                                         + (1UL << (current_order + __PAGE_SHIFT))) - 1;
                new_ct->level = current_order;
                current_block_ch->next->pprev = &current_block_ch->next;
                b->free_head[current_order] = current_block_ch;
        }
#endif
        freelist_sanitycheck(b->free_head);
  uk_pr_err("==========【bbuddy_pfree end】===========\n");
        uk_bbpalloc_dump_freelist();
}