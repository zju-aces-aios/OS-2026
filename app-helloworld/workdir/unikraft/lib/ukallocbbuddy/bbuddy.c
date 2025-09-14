/* SPDX-License-Identifier: MIT */
/*
 * MIT License
 ****************************************************************************
 * (C) 2003 - Rolf Neugebauer - Intel Research Cambridge
 * (C) 2005 - Grzegorz Milos - Intel Research Cambridge
 * (C) 2017 - Simon Kuenzer - NEC Europe Ltd.
 ****************************************************************************
 *
 *        File: mm.c
 *      Author: Rolf Neugebauer (neugebar@dcs.gla.ac.uk)
 *     Changes: Grzegorz Milos
 *     Changes: Simon Kuenzer <simon.kuenzer@neclab.eu>
 *     Changes: Nour-eddine Taleb <contact@noureddine.xyz>
 *
 *        Date: Aug 2003, changes Aug 2005, changes Oct 2017, changes Dec 2022
 *
 * Environment: Unikraft
 * Description: buddy page allocator from Xen.
 *
 ****************************************************************************
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <uk/alloc_impl.h>
#include <uk/allocbbuddy.h>
#include <uk/arch/limits.h>
#include <uk/assert.h>
#include <uk/atomic.h>
#include <uk/bitops.h>
#include <uk/page.h>
#include <uk/print.h>
// 没有编译unikraft，还有一次定义在alloc.c
#define __S_PAGE_SHIFT 7
#define __S_PAGE_SIZE (1ULL << __S_PAGE_SHIFT)

typedef struct chunk_head_st chunk_head_t;
typedef struct chunk_tail_st chunk_tail_t;

struct chunk_head_st {
  chunk_head_t *next;
  chunk_head_t **pprev;
  unsigned int level;
};

struct chunk_tail_st {
  unsigned int level;
};

/* s_free_list */

/* Linked lists of free chunks of different powers-of-two in size. */
#define FREELIST_SIZE ((sizeof(void *) << 3) - __PAGE_SHIFT)
#define FREELIST_EMPTY(_l) ((_l)->next == NULL)
// 是否对齐
#define FREELIST_ALIGNED(ptr, lvl) \
  !((uintptr_t)(ptr) & ((1ULL << ((lvl) + __PAGE_SHIFT)) - 1))

/* keep a bitmap for each memory region separately */
struct uk_bbpalloc_memr {
  struct uk_bbpalloc_memr *next;
  unsigned long first_page;
  unsigned long nr_pages;
  unsigned long mm_alloc_bitmap_size;
  unsigned long *mm_alloc_bitmap;
};

struct uk_bbpalloc {
  unsigned long nr_free_pages;
  chunk_head_t *free_head[FREELIST_SIZE];
  chunk_head_t free_tail[FREELIST_SIZE];
  chunk_head_t *s_free_head[FREELIST_SIZE];
  chunk_head_t s_free_tail[FREELIST_SIZE];
  struct uk_bbpalloc_memr *memr_head;
};

#if CONFIG_LIBUKALLOCBBUDDY_FREELIST_SANITY
/* Provide sanity checking of freelists, walking their length and checking
 * for consistency. Useful when suspecting memory corruption.
 */

#include <uk/arch/paging.h>
#define _FREESAN_NONCANON(x) ((x) && (~(uintptr_t)(x)))
#define _FREESAN_BAD_CHUNKPTR(x)            \
  (((uintptr_t)x & (sizeof(void *) - 1)) || \
   _FREESAN_NONCANON((uintptr_t)(x) >> PAGE_Lx_SHIFT(PT_LEVELS - 1)))

#define _FREESAN_LOCFMT "\t@ %p (free_head[%zu](%p) + %zu): "

#define _FREESAN_HEAD(head)                                                    \
  do {                                                                         \
    size_t off = 0;                                                            \
    for (chunk_head_t *c = head; c; c = c->next, off++) {                      \
      if (c->next != NULL && c->level != i) {                                  \
        uk_pr_err("Bad page level" _FREESAN_LOCFMT "got %u, expected %zu\n",   \
                  c, i, head, off, c->level, i);                               \
      }                                                                        \
      if (_FREESAN_BAD_CHUNKPTR(c->pprev)) {                                   \
        uk_pr_err("Bad pprev pointer" _FREESAN_LOCFMT "%p\n", c, i, head, off, \
                  c->pprev);                                                   \
      } else if (*c->pprev != c) {                                             \
        uk_pr_err("Bad backward link" _FREESAN_LOCFMT "got %p, expected %p\n", \
                  c, i, head, off, *c->pprev, c);                              \
      }                                                                        \
      if (_FREESAN_BAD_CHUNKPTR(c->next)) {                                    \
        uk_pr_err("Bad next pointer" _FREESAN_LOCFMT "%p\n", c, i, head, off,  \
                  c->next);                                                    \
        break;                                                                 \
      } else if (!FREELIST_ALIGNED(c->next, i) && c->next->next != NULL) {     \
        uk_pr_err("Unaligned next page" _FREESAN_LOCFMT                        \
                  "%p not aligned to %zx boundary\n",                          \
                  c, i, head, off, c->next, (size_t)1 << (__PAGE_SHIFT + i));  \
      }                                                                        \
    }                                                                          \
  } while (0)

#define freelist_sanitycheck(free_head)        \
  for (size_t i = 0; i < FREELIST_SIZE; i++) { \
    UK_ASSERT((free_head)[i] != NULL);         \
    _FREESAN_HEAD((free_head)[i]);             \
  }

#else /* !CONFIG_LIBUKALLOCBBUDDY_FREELIST_SANITY */

#define freelist_sanitycheck(x) \
  do {                          \
  } while (0)

#endif /* CONFIG_LIBUKALLOCBBUDDY_FREELIST_SANITY */

/*********************
 * ALLOCATION BITMAP
 *  One bit per page of memory. Bit set => page is allocated.
 *
 * Hint regarding bitwise arithmetic in map_{alloc,free}:
 *  -(1<<n)  sets all bits >= n.
 *  (1<<n)-1 sets all bits <  n.
 * Variable names in map_{alloc,free}:
 *  *_idx == Index into `mm_alloc_bitmap' array.
 *  *_off == Bit offset within an element of the `mm_alloc_bitmap' array.
 */

#define BITS_PER_BYTE 8
#define BYTES_PER_MAPWORD (sizeof(unsigned long))
#define PAGES_PER_MAPWORD (BYTES_PER_MAPWORD * BITS_PER_BYTE)

static inline struct uk_bbpalloc_memr *map_get_memr(struct uk_bbpalloc *b,
                                                    unsigned long page_va) {
  struct uk_bbpalloc_memr *memr = NULL;

  /*
   * Find bitmap of according memory region
   * This is a linear search but it is expected that we have only a few
   * of them. It should be just one region in most cases
   */
  for (memr = b->memr_head; memr != NULL; memr = memr->next) {
    if ((page_va >= memr->first_page) &&
        (page_va < (memr->first_page + (memr->nr_pages << __PAGE_SHIFT))))
      return memr;
  }

  /*
   * No region found
   */
  return NULL;
}

static inline unsigned long allocated_in_map(struct uk_bbpalloc *b,
                                             unsigned long page_va) {
  struct uk_bbpalloc_memr *memr = map_get_memr(b, page_va);
  unsigned long page_idx;
  unsigned long bm_idx, bm_off;

  /* treat pages outside of region as allocated */
  if (!memr) return 1;

  page_idx = (page_va - memr->first_page) >> __PAGE_SHIFT;
  bm_idx = page_idx / PAGES_PER_MAPWORD;
  bm_off = page_idx & (PAGES_PER_MAPWORD - 1);

  return ((memr)->mm_alloc_bitmap[bm_idx] & (1UL << bm_off));
}

static void map_alloc(struct uk_bbpalloc *b, uintptr_t first_page,
                      unsigned long nr_pages) {
  struct uk_bbpalloc_memr *memr;
  unsigned long first_page_idx, end_page_idx;
  unsigned long start_off, end_off, curr_idx, end_idx;

  /*
   * In case there was no memory region found, the allocator
   * is in a really bad state. It means that the specified page
   * region is not covered by our allocator.
   */
  memr = map_get_memr(b, first_page);
  UK_ASSERT(memr != NULL);
  UK_ASSERT((first_page + (nr_pages << __PAGE_SHIFT)) <=
            (memr->first_page + (memr->nr_pages << __PAGE_SHIFT)));

  first_page -= memr->first_page;
  first_page_idx = first_page >> __PAGE_SHIFT;
  curr_idx = first_page_idx / PAGES_PER_MAPWORD;
  start_off = first_page_idx & (PAGES_PER_MAPWORD - 1);
  end_page_idx = first_page_idx + nr_pages;
  end_idx = end_page_idx / PAGES_PER_MAPWORD;
  end_off = end_page_idx & (PAGES_PER_MAPWORD - 1);

  /*****
   * 5 / 64 = 0，8 / 64 = 0
   * 63 + 2 = 65，65 / 64 = 1 这样就不相等
   */
  if (curr_idx == end_idx) {
    // 更新位图
    memr->mm_alloc_bitmap[curr_idx] |=
        ((1UL << end_off) - 1) & -(1UL << start_off);
  } else {
    memr->mm_alloc_bitmap[curr_idx] |= -(1UL << start_off);
    while (++curr_idx < end_idx) memr->mm_alloc_bitmap[curr_idx] = ~0UL;
    memr->mm_alloc_bitmap[curr_idx] |= (1UL << end_off) - 1;
  }

  b->nr_free_pages -= nr_pages;
}

static void map_free(struct uk_bbpalloc *b, uintptr_t first_page,
                     unsigned long nr_pages) {
  struct uk_bbpalloc_memr *memr;
  unsigned long first_page_idx, end_page_idx;
  unsigned long start_off, end_off, curr_idx, end_idx;

  /*
   * In case there was no memory region found, the allocator
   * is in a really bad state. It means that the specified page
   * region is not covered by our allocator.
   */
  memr = map_get_memr(b, first_page);
  UK_ASSERT(memr != NULL);
  UK_ASSERT((first_page + (nr_pages << __PAGE_SHIFT)) <=
            (memr->first_page + (memr->nr_pages << __PAGE_SHIFT)));

  first_page -= memr->first_page;
  first_page_idx = first_page >> __PAGE_SHIFT;
  curr_idx = first_page_idx / PAGES_PER_MAPWORD;
  start_off = first_page_idx & (PAGES_PER_MAPWORD - 1);
  end_page_idx = first_page_idx + nr_pages;
  end_idx = end_page_idx / PAGES_PER_MAPWORD;
  end_off = end_page_idx & (PAGES_PER_MAPWORD - 1);

  if (curr_idx == end_idx) {
    memr->mm_alloc_bitmap[curr_idx] &=
        -(1UL << end_off) | ((1UL << start_off) - 1);
  } else {
    memr->mm_alloc_bitmap[curr_idx] &= (1UL << start_off) - 1;
    while (++curr_idx != end_idx) memr->mm_alloc_bitmap[curr_idx] = 0;
    memr->mm_alloc_bitmap[curr_idx] &= -(1UL << end_off);
  }

  b->nr_free_pages += nr_pages;
}

/* return log of the next power of two of passed number */
static inline unsigned long num_pages_to_order(unsigned long num_pages) {
  UK_ASSERT(num_pages != 0);

  /* uk_flsl has undefined behavior when called with zero */
  if (num_pages == 1) return 0;

  /* uk_flsl(num_pages - 1) returns log of the previous power of two
   * of num_pages. uk_flsl is called with `num_pages - 1` and not
   * `num_pages` to handle the case where num_pages is already a power
   * of two.
   */
  return uk_flsl(num_pages - 1) + 1;
}

// lab 2
void uk_bbpalloc_dump_freelist() {
  // 获取内存管理
  struct uk_alloc *a = uk_alloc_get_default();
  struct uk_bbpalloc *b = (struct uk_bbpalloc *)&a->priv;
  // 当前内存块
  chunk_head_t *entry;
  // 当前分配的level
  unsigned long i, k;

  uk_pr_err("Dumping current state of the free list:\n");

  /* Traverse through all the free lists */
  /**
   * #define FREELIST_SIZE ((sizeof(void *) << 3) - __PAGE_SHIFT)
   * 这个freelist要解释一下 (sizeof(void *) << 3)
   * 这一步可以计算出是32还是64，根据机器不同计算出的值不一样
   * 然后__PAGE_SHIFT是12，因为12=4K就是一个页面的大小
   * 所以就是freesize就是预留出一个页面的大小，然后其他全部是可以分配的freelist
   * 也就是不同的level
   */
  // i = 0;
  // k = 10;
  for (i = 0; i < MIN(FREELIST_SIZE, 10); i++) {
    if (b->free_head[i] == &b->free_tail[i]) {
      uk_pr_err("Free list %lu is empty.\n", i);
    } else {
      uk_pr_err("Free list %lu (Order %lu):\n", i, i);
      entry = b->free_head[i];
      while (entry != &b->free_tail[i]) {
        // while (k-- && entry != &b->free_tail[i]) {
        uk_pr_err("  Entry at address: %p, end: %p, level: %u\n", entry,
                  (unsigned int)entry + (__PAGE_SIZE << i), entry->level);
        entry = entry->next;
      }
    }
  }
}

void uk_dump_s_freelist() {
  // 获取内存管理
  struct uk_alloc *a = uk_alloc_get_default();
  struct uk_bbpalloc *b = (struct uk_bbpalloc *)&a->priv;
  // 当前内存块
  chunk_head_t *entry;
  // 当前分配的level
  unsigned long i, k;
  i = 0;
//   k = __PAGE_SIZE / __S_PAGE_SIZE;
  k = 10;
  uk_pr_err("Dumping current state of the sfree list:\n");

  if (b->s_free_head[i] == &b->s_free_head[i]) {
    uk_pr_err("SFree list %lu is empty.\n", i);
  } else {
    uk_pr_err("SFree list %lu (Order %lu):\n", i, i);
    entry = b->s_free_head[i];
    while (k-- && entry != &b->s_free_head[i]) {
      // while (k-- && entry != &b->free_tail[i]) {
      uk_pr_err("  Entry at address: %p, end: %p, level: %u\n", entry,
                (unsigned int)entry + (__S_PAGE_SIZE << i), entry->level);
      entry = entry->next;
    }
  }
}

/*********************
 * BINARY BUDDY PAGE ALLOCATOR
 */
// lab 2
static void *bbuddy_palloc(struct uk_alloc *a, unsigned long num_pages) {
  struct uk_bbpalloc *b;
  size_t i;
  chunk_head_t *alloc_ch, *spare_ch;
  chunk_tail_t *spare_ct;

  uk_pr_err("==========【bbuddy_palloc start】===========\n");
  uk_bbpalloc_dump_freelist();
  UK_ASSERT(a != NULL);
  b = (struct uk_bbpalloc *)&a->priv;

  freelist_sanitycheck(b->free_head);

  size_t order = (size_t)num_pages_to_order(num_pages);

  /* Find smallest order which can satisfy the request. */
  for (i = order; i < FREELIST_SIZE; i++) {
    if (!FREELIST_EMPTY(b->free_head[i])) break;
  }
  if (i >= FREELIST_SIZE) goto no_memory;

  /* Unlink a chunk. */
  alloc_ch = b->free_head[i];
  b->free_head[i] = alloc_ch->next;
  alloc_ch->next->pprev = alloc_ch->pprev;

  /* We may have to break the chunk a number of times. */
  while (i != order) {
    /* Split into two equal parts. */
    i--;
    spare_ch = (chunk_head_t *)((char *)alloc_ch + (1UL << (i + __PAGE_SHIFT)));
    spare_ct =
        (chunk_tail_t *)((char *)spare_ch + (1UL << (i + __PAGE_SHIFT))) - 1;

    /* Create new header for spare chunk. */
    spare_ch->level = i;
    spare_ch->next = b->free_head[i];
    spare_ch->pprev = &b->free_head[i];
    spare_ct->level = i;

    /* Link in the spare chunk. */
    spare_ch->next->pprev = &spare_ch->next;
    b->free_head[i] = spare_ch;
  }
  // 分配完之后对齐
  UK_ASSERT(FREELIST_ALIGNED(alloc_ch, order));
  // 更新位图
  map_alloc(b, (uintptr_t)alloc_ch, 1UL << order);

  uk_alloc_stats_count_palloc(a, (void *)alloc_ch, num_pages);
  freelist_sanitycheck(b->free_head);

  uk_pr_err("==========【bbuddy_palloc end】===========\n");
  uk_bbpalloc_dump_freelist();

  return ((void *)alloc_ch);

no_memory:
  uk_pr_warn(
      "%"__PRIuptr
      ": Cannot handle palloc request of order %"__PRIsz
      ": Out of memory\n",
      (uintptr_t)a, order);

  uk_alloc_stats_count_penomem(a, num_pages);
  errno = ENOMEM;
  return NULL;
}

static void *bbuddy_salloc(struct uk_alloc *a, unsigned long num_pages) {
  struct uk_bbpalloc *b;
  size_t i;
  chunk_head_t *alloc_ch, *spare_ch;
  chunk_tail_t *spare_ct;

  UK_ASSERT(a != NULL);
  b = (struct uk_bbpalloc *)&a->priv;
  uk_pr_err("==========【bbuddy_salloc start】===========\n");
  uk_dump_s_freelist();

  freelist_sanitycheck(b->s_free_head);

  size_t order = (size_t)num_pages_to_order(num_pages);

  chunk_head_t *pred_ch, *curr_ch;
  curr_ch = b->s_free_head[0];
  i = 1;

  uk_pr_debug("0x%016lx 0x%016lx\n", curr_ch, &b->s_free_tail[0]);
  while (i < num_pages && (curr_ch->next != &b->s_free_tail[0])) {
    pred_ch = curr_ch;
    curr_ch = curr_ch->next;
    if ((unsigned long)curr_ch + __S_PAGE_SIZE != (unsigned long)pred_ch) {
      i = 0;
    }
    i += 1;
    // uk_pr_debug("curr 0x%016lx\n", curr_ch);
  }

  if (i != num_pages) {
    goto no_memory;
  }

  alloc_ch = curr_ch;
  b->s_free_head[0] = alloc_ch->next;
  alloc_ch->next->pprev = alloc_ch->pprev;

  uk_pr_err("ALLOC 0x%016lx %d\n", alloc_ch, num_pages);

  // map_alloc(b, (uintptr_t)alloc_ch, num_pages);

  uk_alloc_stats_count_palloc(a, (void *)alloc_ch, num_pages);
  freelist_sanitycheck(b->s_free_head);
  uk_pr_err("==========【bbuddy_salloc end】===========\n");
  uk_dump_s_freelist();

  return ((void *)alloc_ch);

no_memory:
  uk_pr_warn(
      "%"__PRIuptr
      ": Cannot handle palloc request of order %"__PRIsz
      ": Out of memory\n",
      (uintptr_t)a, order);

  uk_alloc_stats_count_penomem(a, num_pages);
  errno = ENOMEM;
  return NULL;
}

static void bbuddy_pfree(struct uk_alloc *a, void *obj,
                         unsigned long num_pages) {
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

  /* if the object is not page aligned it was clearly not from us */
  UK_ASSERT((((uintptr_t)obj) & (__PAGE_SIZE - 1)) == 0);

  /* First free the chunk */
  map_free(b, (uintptr_t)obj, 1UL << order);

  /* Create free chunk */
  freed_ch = (chunk_head_t *)obj;
  freed_ct =
      (chunk_tail_t *)((char *)obj + (1UL << (order + __PAGE_SHIFT))) - 1;

  /* Now, possibly we can conseal chunks together */
  while (order < FREELIST_SIZE) {
    mask = 1UL << (order + __PAGE_SHIFT);
    if ((unsigned long)freed_ch & mask) {
      to_merge_ch = (chunk_head_t *)((char *)freed_ch - mask);
      if (allocated_in_map(b, (uintptr_t)to_merge_ch) ||
          to_merge_ch->level != order)
        break;

      /* Merge with predecessor */
      freed_ch = to_merge_ch;
    } else {
      to_merge_ch = (chunk_head_t *)((char *)freed_ch + mask);
      if (allocated_in_map(b, (uintptr_t)to_merge_ch) ||
          to_merge_ch->level != order)
        break;

      /* Merge with successor */
      freed_ct = (chunk_tail_t *)((char *)to_merge_ch + mask) - 1;
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

  freelist_sanitycheck(b->free_head);
  uk_pr_err("==========【bbuddy_pfree end】===========\n");
  uk_bbpalloc_dump_freelist();
}

static void bbuddy_sfree(struct uk_alloc *a, void *obj,
                         unsigned long num_pages) {
  struct uk_bbpalloc *b;
  chunk_head_t *freed_ch, *to_merge_ch;
  chunk_tail_t *freed_ct;
  unsigned long mask;

  UK_ASSERT(a != NULL);
  uk_pr_err("==========【bbuddy_sfree start】===========\n");
  uk_dump_s_freelist();

  uk_alloc_stats_count_pfree(a, obj, num_pages);
  b = (struct uk_bbpalloc *)&a->priv;

  freelist_sanitycheck(b->s_free_head);

  size_t order = (size_t)num_pages_to_order(num_pages);

  /* if the object is not page aligned it was clearly not from us */
  uk_pr_err("obj => %p, num_pages => %llu\n", obj, num_pages);
  UK_ASSERT((((uintptr_t)obj) & (__S_PAGE_SIZE - 1)) == 0);

  /* First free the chunk */
  // map_free(b, (uintptr_t)obj, num_pages);

  int nr_page_left = num_pages;

  // 定义指针以找到合适的插入位置
  chunk_head_t **current = &b->s_free_head[0];
  chunk_head_t *prev = NULL;  // 用于保存前一个节点的指针

  // 首先遍历 b->s_free_head 找到合适的插入位置
  while (nr_page_left) {
    // 将传过来的地址块转换为 chunk
    freed_ch = (chunk_head_t *)obj;
    freed_ct = (chunk_tail_t *)((char *)obj + (1UL << __S_PAGE_SHIFT)) - 1;

    freed_ch->level = 0;
    freed_ct->level = 0;

    // 在插入之前找到合适的位置
    while (*current != NULL) {
      // 比较当前块的地址和要插入块的地址
      if ((uintptr_t)freed_ch > (uintptr_t)(*current)) {
        // 找到合适的插入位置
        break;
      }
      prev = *current;              // 更新前一个节点
      current = &(*current)->next;  // 移动到下一个节点
    }

    // 插入操作
    freed_ch->next = *current;  // 将 freed_ch 的 next 指向当前指针指向的块
    freed_ch->pprev = current;  // 将 freed_ch 的 pprev 指向当前指针

    if (*current != NULL) {
      (*current)->pprev = &freed_ch->next;  // 更新当前块的 pprev
    }

    *current = freed_ch;  // 更新链表头指针位置

    // 更新 obj 为下一个块
    nr_page_left--;
    obj = freed_ct + 1;  // 强制转换出错，导致地址错误
  }

  freelist_sanitycheck(b->s_free_head);
  uk_pr_err("==========【bbuddy_sfree end】===========\n");
  uk_dump_s_freelist();
}

static long bbuddy_pmaxalloc(struct uk_alloc *a) {
  struct uk_bbpalloc *b;
  size_t i, order;

  UK_ASSERT(a != NULL);
  b = (struct uk_bbpalloc *)&a->priv;

  /* Find biggest order that has still elements available */
  order = FREELIST_SIZE;
  for (i = 0; i < FREELIST_SIZE; i++) {
    if (!FREELIST_EMPTY(b->free_head[i])) order = i;
  }
  if (order == FREELIST_SIZE) return 0; /* no memory left */

  return (long)(1 << order);
}

static long bbuddy_pavailmem(struct uk_alloc *a) {
  struct uk_bbpalloc *b;

  UK_ASSERT(a != NULL);
  b = (struct uk_bbpalloc *)&a->priv;

  return (long)b->nr_free_pages;
}

static void *bbuddy_addsmem(struct uk_alloc *a) {
  struct uk_bbpalloc *b;
  size_t i;
  chunk_head_t *alloc_ch, *spare_ch;
  chunk_tail_t *spare_ct;
  // 这里写死是因为当前的算法是确定好s_free_list总大小是一个page_size这么大，所以只需要一个num_pages就可以
  unsigned long num_pages = 1;

  uk_pr_err("==========【bbuddy_addsmem start】===========\n");
  uk_bbpalloc_dump_freelist();

  UK_ASSERT(a != NULL);
  b = (struct uk_bbpalloc *)&a->priv;

  freelist_sanitycheck(b->s_free_head);

  // 这里是判断free_list中是否还有一个页面大小的块
  for (i = 0; i < FREELIST_SIZE; i++) {
    if (!FREELIST_EMPTY(b->free_head[i])) break;
  }
  if (i >= FREELIST_SIZE) goto no_memory;

  // /* Unlink a chunk. */
  alloc_ch = b->free_head[i];
  b->free_head[i] = alloc_ch->next;
  if (alloc_ch->next != NULL) {
    alloc_ch->next->pprev = alloc_ch->pprev;
  }

  // // 计算 alloc_ch 的总大小
  size_t total_size =
      (1UL << (i + __PAGE_SHIFT)); /* 获取 alloc_ch 的实际大小 */
  size_t remaining_size = total_size - __PAGE_SIZE;

  // // 确保剩余空间可以划分为多个 PAGE_SIZE
  if (remaining_size > 0) {
    size_t num_chunks = remaining_size / __PAGE_SIZE;

    if (num_chunks > 0) {
      for (size_t j = 0; j < num_chunks; j++) {
        spare_ch =
            (chunk_head_t *)((char *)alloc_ch + __PAGE_SIZE + j * __PAGE_SIZE);
        spare_ct = (chunk_tail_t *)((char *)spare_ch + __PAGE_SIZE) - 1;

        spare_ch->level = 0;
        spare_ch->next = b->free_head[0];
        spare_ch->pprev = &b->free_head[0];
        spare_ct->level = 0;

        if (spare_ch->next != NULL) {
          spare_ch->next->pprev = &spare_ch->next;
        }
        b->free_head[0] = spare_ch;
      }
    }
  }

  // // 分配完之后对齐
  UK_ASSERT(FREELIST_ALIGNED(alloc_ch, 0));
  map_alloc(b, (uintptr_t)alloc_ch, 1UL << 0);

  uk_pr_err("==========【bbuddy_addsmem end】===========\n");
  uk_bbpalloc_dump_freelist();

  uk_alloc_stats_count_palloc(a, (void *)alloc_ch, num_pages);
  freelist_sanitycheck(b->s_free_head);

  return ((void *)alloc_ch);

no_memory:
  uk_pr_warn(
      "%"__PRIuptr
      ": Cannot handle palloc request of order %"__PRIsz
      ": Out of memory\n",
      (uintptr_t)a, 0);
  errno = ENOMEM;
  return NULL;
}

/***
 * 假设有一个链表节点 A <-> B <-> C
 * 当前结构体是 B，next 指向 C，pprev 指向 A 的 next
 * B->pprev = &(A->next);
 * 如果你删除 B 节点，可以通过 pprev 直接更新 A->next = C
 * *(B->pprev) = B->next;
 * 所以总结下，这个pprev指向的是前一个指针的next指针的指针
 */

// lab3
static int bbuddy_addmem(struct uk_alloc *a, void *base, size_t len) {
  struct uk_bbpalloc *b;
  struct uk_bbpalloc_memr *memr;
  size_t memr_size;
  unsigned long i, j;
  chunk_head_t *ch;
  chunk_tail_t *ct;
  uintptr_t min, max, range, srange;

  UK_ASSERT(a != NULL);
  UK_ASSERT(base != NULL);
  b = (struct uk_bbpalloc *)&a->priv;

  freelist_sanitycheck(b->free_head);

  min = round_pgup((uintptr_t)base);
  max = round_pgdown((uintptr_t)base + (uintptr_t)len);
  if (max < min) {
    uk_pr_err(
        "%"__PRIuptr
        ": Failed to add memory region %"__PRIuptr
        "-%"__PRIuptr
        ": Invalid range after applying page alignments\n",
        (uintptr_t)a, (uintptr_t)base, (uintptr_t)base + (uintptr_t)len);
    return -EINVAL;
  }

  range = max - min;

  /* We should have at least one page for bitmap tracking
   * and one page for data.
   */
  if (range < round_pgup(sizeof(*memr) + BYTES_PER_MAPWORD) + __PAGE_SIZE) {
    uk_pr_err(
        "%"__PRIuptr
        ": Failed to add memory region %"__PRIuptr
        "-%"__PRIuptr
        ": Not enough space after applying page alignments\n",
        (uintptr_t)a, (uintptr_t)base, (uintptr_t)base + (uintptr_t)len);
    return -EINVAL;
  }

  memr = (struct uk_bbpalloc_memr *)min;

  /*
   * The number of pages is found by solving the inequality:
   *
   * sizeof(*memr) + bitmap_size + page_num * page_size <= range
   *
   * where: bitmap_size = page_num / BITS_PER_BYTE
   *
   */
  memr->nr_pages = range >> __PAGE_SHIFT;
  memr->mm_alloc_bitmap = (unsigned long *)(min + sizeof(*memr));
  memr_size =
      round_pgup(sizeof(*memr) + DIV_ROUND_UP(memr->nr_pages, BITS_PER_BYTE));
  memr->mm_alloc_bitmap_size = memr_size - sizeof(*memr);

  min += memr_size;
  range -= memr_size;
  memr->nr_pages -= memr_size >> __PAGE_SHIFT;

  /*
   * Initialize region's bitmap
   */
  memr->first_page = min;
  /* add to list */
  memr->next = b->memr_head;
  b->memr_head = memr;

  /* All allocated by default. */
  memset(memr->mm_alloc_bitmap, (unsigned char)~0, memr->mm_alloc_bitmap_size);

  /* free up the memory we've been given to play with */
  map_free(b, min, memr->nr_pages);

  while (range != 0) {
    /*
     * Next chunk is limited by alignment of min, but also
     * must not be bigger than remaining range.
     */
    /**
     * range是空闲内存空间
     * 1UL就是1，每次分配13开始，然后+1，与min一起&就是为了对齐而以
     * 相当于当range没有被完全分配出去之前，就会被循环分配，每次分配都会比以前多2倍
     * */
    for (i = __PAGE_SHIFT; (1UL << (i + 1)) <= range; i++)
      if (min & (1UL << i)) break;
    // lab3
    // i = __PAGE_SHIFT;

    uk_pr_debug(
        "%"__PRIuptr
        ": Add allocate unit %"__PRIuptr
        " - %"__PRIuptr
        " (order %lu)\n",
        (uintptr_t)a, min, (uintptr_t)(min + (1UL << i)), (i - __PAGE_SHIFT));

    ch = (chunk_head_t *)min;
    min += 1UL << i;
    range -= 1UL << i;
    ct = (chunk_tail_t *)min - 1;
    i -= __PAGE_SHIFT;
    ch->level = i;
    ch->next = b->free_head[i];
    ch->pprev = &b->free_head[i];
    ch->next->pprev = &ch->next;
    b->free_head[i] = ch;
    ct->level = i;
  }

  freelist_sanitycheck(b->free_head);

  // 从free_list中取一个order=1的块分配给s_free_list bbuddy_salloc(a);
  chunk_head_t *alloc_ch = bbuddy_addsmem(a);
  uintptr_t start_addr = (uintptr_t)alloc_ch;
  srange = __PAGE_SIZE;

  // 输出开始地址和结束地址
  uk_pr_err(" SFree list at address: %p, end: %p, level: %u\n", start_addr,
            (unsigned int)start_addr + __PAGE_SIZE, 0);
  int epch = 0;
  // 把得到的内存进行分配
  while (srange != 0) {
    j = __S_PAGE_SHIFT;

    // uk_pr_err(
    //     "%"__PRIuptr
    //     ": Add allocate unit %"__PRIuptr
    //     " - %"__PRIuptr
    //     " - %d"__PRIuptr
    //     " (order %lu)\n",
    //     (uintptr_t)a, start_addr, (uintptr_t)(start_addr + (1UL << j)), epch
    //     + 1, (j - __S_PAGE_SHIFT));
    epch++;
    ch = (chunk_head_t *)start_addr;
    start_addr += 1UL << j;
    srange -= 1UL << j;
    ct = (chunk_tail_t *)start_addr - 1;
    j -= __S_PAGE_SHIFT;
    ch->level = j;
    ch->next = b->s_free_head[j];
    ch->pprev = &b->s_free_head[j];
    ch->next->pprev = &ch->next;
    b->s_free_head[j] = ch;
    ct->level = j;
  }

  uk_pr_err("==========【bbuddy_addmem s_free_list init】===========\n");
  uk_dump_s_freelist();

  return 0;
}

struct uk_alloc *uk_allocbbuddy_init(void *base, size_t len) {
  struct uk_alloc *a;
  struct uk_bbpalloc *b;
  size_t metalen;
  uintptr_t min, max;
  unsigned long i;

  min = round_pgup((uintptr_t)base);
  max = round_pgdown((uintptr_t)base + (uintptr_t)len);
  UK_ASSERT(max > min);

  /* Allocate space for allocator descriptor */
  metalen = round_pgup(sizeof(*a) + sizeof(*b));

  /* enough space for allocator available? */
  if (min + metalen > max) {
    uk_pr_err(
        "Not enough space for allocator: %"__PRIsz
        " B required but only %"__PRIuptr
        " B usable\n",
        metalen, (max - min));
    return NULL;
  }

  a = (struct uk_alloc *)min;
  uk_pr_info(
      "Initialize binary buddy allocator %"__PRIuptr
      "\n",
      (uintptr_t)a);
  min += metalen;
  memset(a, 0, metalen);
  b = (struct uk_bbpalloc *)&a->priv;

  for (i = 0; i < FREELIST_SIZE; i++) {
    b->free_head[i] = &b->free_tail[i];
    b->free_tail[i].pprev = &b->free_head[i];
    b->free_tail[i].next = NULL;

    b->s_free_head[i] = &b->s_free_tail[i];
    b->s_free_tail[i].pprev = &b->s_free_head[i];
    b->s_free_tail[i].next = NULL;
  }

  b->memr_head = NULL;

  /* initialize and register allocator interface */
  uk_alloc_init_palloc(a, bbuddy_palloc, bbuddy_pfree, bbuddy_pmaxalloc,
                       bbuddy_pavailmem, bbuddy_addmem, bbuddy_salloc,
                       bbuddy_sfree);

  if (max > min) {
    /* add left memory - ignore return value */
    bbuddy_addmem(a, (void *)(min), (size_t)(max - min));
  }

  return a;
}
