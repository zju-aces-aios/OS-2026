# 实验二：代码纠错 — 问题说明与修复（两处）

依据 `Lab2/Lab2.md` 的代码片段，本文精准说明并修复如下两处问题。

## 问题 1：`obj` 前进计算错误，仅前进 1 字节导致地址错位

原始代码：
```87:87:Lab2/Lab2.md
	obj = (char *) freed_ct + 1;
```

**原因：** `freed_ct` 指向页尾的 `chunk_tail_t`，上述写法仅将地址前进 1 字节，而非一个页大小，导致下一次循环的对象地址错位、可能越界。

**修复：** 按页大小推进：
```c
obj = (char *) obj + (1UL << __PAGE_SHIFT);
```

## 问题 2：释放页挂回空链表时缺少空指针判断

原始代码：
```79:84:Lab2/Lab2.md
	freed_ch->level = 0;
	freed_ch->next = b->free_head[0];
	freed_ch->pprev = &b->free_head[0];
	freed_ct->level = 0;

	freed_ch->next->pprev = &freed_ch->next;
	b->free_head[0] = freed_ch;
```

**原因：** 当 `b->free_head[0]` 为空时，`freed_ch->next` 为 `NULL`，立即写 `freed_ch->next->pprev` 会解引用空指针，导致崩溃。

**修复：** 回链前先判空，再回写 `pprev`：
```c
if (freed_ch->next)
    freed_ch->next->pprev = &freed_ch->next;
b->free_head[0] = freed_ch;
```

---

### 提交说明
- 修正后的代码片段已保存为 `Lab2/foo.c`（仅包含函数实现，无多余内容），并已按以上两处问题完成修改。


