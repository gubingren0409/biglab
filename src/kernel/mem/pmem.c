#include "mod.h"

// 内核空间和用户空间的可分配物理页分开描述
static alloc_region_t kern_region, user_region;

static inline alloc_region_t *select_region(bool in_kernel)
{
    return in_kernel ? &kern_region : &user_region;
}

static void region_setup(alloc_region_t *region, uint64 begin, uint64 end, char *name)
{
    region->begin = begin;
    region->end = end;
    spinlock_init(&region->lk, name);
    region->allocable = 0;
    region->list_head.next = NULL;

    if (begin >= end)
        return;

    for (uint64 addr = begin; addr + PGSIZE <= end; addr += PGSIZE)
    {
        page_node_t *node = (page_node_t *)addr;
        node->next = region->list_head.next;
        region->list_head.next = node;
        region->allocable++;
    }
}

// 物理内存的初始化
// 本质上就是填写kern_region和user_region, 包括基本数值和空闲链表
void pmem_init(void)
{
    static bool initialized = false;
    if (initialized)
        return;

    uint64 alloc_begin = (uint64)ALLOC_BEGIN;
    uint64 alloc_end = (uint64)ALLOC_END;

    assert((alloc_begin % PGSIZE) == 0, "pmem_init: alloc_begin not aligned");
    assert((alloc_end % PGSIZE) == 0, "pmem_init: alloc_end not aligned");
    assert(alloc_end > alloc_begin, "pmem_init: invalid range");

    uint64 kern_begin = alloc_begin;
    uint64 kern_end = kern_begin + (uint64)KERN_PAGES * PGSIZE;
    assert(kern_end <= alloc_end, "pmem_init: kernel pages overflow");

    region_setup(&kern_region, kern_begin, kern_end, "kern_region");
    region_setup(&user_region, kern_end, alloc_end, "user_region");

    initialized = true;

}

// 尝试返回一个可分配的清零后的物理页
// 失败则panic锁死
void* pmem_alloc(bool in_kernel)
{
    alloc_region_t *region = select_region(in_kernel);

    spinlock_acquire(&region->lk);

    page_node_t *page = region->list_head.next;
    if (page == NULL)
    {
        spinlock_release(&region->lk);
        panic(in_kernel ? "pmem_alloc: no kernel pages" : "pmem_alloc: no user pages");
    }

    region->list_head.next = page->next;
    region->allocable--;
    spinlock_release(&region->lk);

    memset(page, 0, PGSIZE);

    return page;
}

// 释放一个物理页
// 失败则panic锁死
void pmem_free(uint64 page, bool in_kernel)
{
    alloc_region_t *region = select_region(in_kernel);

    assert((page % PGSIZE) == 0, "pmem_free: page not aligned");
    assert(page >= region->begin && page < region->end, "pmem_free: page out of range");

    page_node_t *node = (page_node_t *)page;

    spinlock_acquire(&region->lk);

    node->next = region->list_head.next;
    region->list_head.next = node;
    region->allocable++;

    spinlock_release(&region->lk);

}
