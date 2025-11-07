# lab2

# 1.地址计算错误

​`obj = (char *)freed_ct + 1;`此处地址只移动了1B，而不是1页

应改为`obj = (char *)freed_ct + (1UL<<_PAGE_SHIFT);`

# 2.回收内存错误

```c
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
freed_ct->level = 0;
b->free_head[0] = freed_ch;
```

此处本应该在回收内存，但是产生了一堆order0的页

‍
