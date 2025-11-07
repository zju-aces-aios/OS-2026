## 问题1
- `# else`中的逻辑并非完整pfree逻辑，仅对order为0且最大order也为0时的chunk进行了释放；完整逻辑在`# if 0`当中
- 删除函数中所有宏流程控制语句即可
## 问题2
-  当 `b->free_head[order]` 为 NULL 时（即该 order 的空闲链表为空），`freed_ch->next` 将被赋值为 NULL，对其的解引用会导致空指针解引用。
-  添加空指针检查即可