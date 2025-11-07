**原始代码存在问题**：
1. 原代码在空闲链表为空时直接写 freed_ch->next->pprev，会解引用空指针导致崩溃。修改为判空后再更新 pprev。
2. 页迭代时用 obj = (char *)freed_ct + 1 只移动一个字节，导致第二页地址错误。改成 obj = (char *)obj + (1UL << __PAGE_SHIFT) 以整页步长向前。