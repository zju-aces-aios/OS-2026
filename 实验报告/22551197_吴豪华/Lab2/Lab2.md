#**实验二：代码纠错**

### 找出代码片段中的2处问题并改正

1、文件中代码片段中，释放的块全是连接到level为0的链表上

```c

    freed_ch->level = order;
    freed_ch->next = b->free_head[order];

    freed_ch->pprev = &b->free_head[order]; 
    freed_ct->level = order;

    if (freed_ch->next != NULL) { 
        freed_ch->next->pprev = freed_ch->pprev; 
    }

    b->free_head[order] = freed_ch;
```

通过修改这里的level等来进行合理的空间释放

2、代码里的块起始点存在空间不对齐的问题，修改为

```c
char *page_ptr = (char *)obj;
const unsigned long PAGE_SIZE_BYTES = 1UL << __PAGE_SHIFT;
	freed_ct = (chunk_tail_t *)(page_ptr + PAGE_SIZE_BYTES) - 1;
	page_ptr += PAGE_SIZE_BYTES;
```

来进行合理的起始位置移动

实验：

通过实验一中的set small=0来达成使用pfree进行释放
![image-20251101143636290](C:\Users\qq117\AppData\Roaming\Typora\typora-user-images\image-20251101143636290.png)

![image-20251101143706248](C:\Users\qq117\AppData\Roaming\Typora\typora-user-images\image-20251101143706248.png)

达成相对合理的释放