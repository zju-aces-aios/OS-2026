# 实验二：代码纠错

### 1. obj = (char *) freed_ct + 1;

此处修改obj指向下一个页块由尾指针freed_ct向后移动1个字节得到，而freed_ct的计算为`freed_ct = (chunk_tail_t *)((char *)obj + (1UL << __PAGE_SHIFT)) - 1`得到，即由下一页的起始减去一个chunk_tail_t结构体大小。所有此处obj并非下一个页块的起始位置，应改为`obj = (char *) (freed_ct + 1)`。

### 2.块插入倒序

```
freed_ch->level = 0;
freed_ch->next = b->free_head[0];
freed_ch->pprev = &b->free_head[0];
freed_ct->level = 0;

freed_ch->next->pprev = &freed_ch->next;
b->free_head[0] = freed_ch;
```

此处使用头插法将页块插入空闲链表，会导致地址低的页块反而落在后面。先在循环内部将页块处理好再插入空闲链表
