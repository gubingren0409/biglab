#include "mod.h"

// 内核页表
static pgtbl_t kernel_pgtbl;

// 根据pagetable,找到va对应的pte
// 若设置alloc=true 则在PTE无效时尝试申请一个物理页
// 成功返回PTE, 失败返回NULL
// 提示：使用 VA_TO_VPN + PTE_TO_PA + PA_TO_PTE
pte_t *vm_getpte(pgtbl_t pgtbl, uint64 va, bool alloc)
{
    // 检查虚拟地址是否合法
    if (va >= VA_MAX)
        return NULL;
    
    // 三级页表查找
    for (int level = 2; level >= 0; level--) {
        // 获取当前级别的VPN
        uint64 vpn = VA_TO_VPN(va, level);
        pte_t *pte = &pgtbl[vpn];
        
        if (level == 0) {
            // 最后一级，返回PTE地址
            return pte;
        }
        
        // 检查PTE是否有效
        if (!(*pte & PTE_V)) {
            if (!alloc) {
                return NULL;
            }
            // 分配新的页表页面
            pgtbl_t new_pgtbl = (pgtbl_t)pmem_alloc(true);
            if (new_pgtbl == NULL) {
                return NULL;
            }
            // 设置PTE指向新页表
            *pte = PA_TO_PTE((uint64)new_pgtbl) | PTE_V;
        }
        
        // 检查这是否是页表页面（不是数据页面）
        if (!PTE_CHECK(*pte)) {
            return NULL;
        }
        
        // 移动到下一级页表
        pgtbl = (pgtbl_t)PTE_TO_PA(*pte);
    }
    
    return NULL;
}

// 在pgtbl中建立 [va, va + len) -> [pa, pa + len) 的映射
// 本质是找到va在页表对应位置的pte并修改它
// 检查: va pa 应当是 page-aligned, len(字节数) > 0, va + len <= VA_MAX
// 注意: perm 应该如何使用
void vm_mappages(pgtbl_t pgtbl, uint64 va, uint64 pa, uint64 len, int perm)
{
    // 参数检查
    assert((va % PGSIZE) == 0, "vm_mappages: va not aligned");
    assert((pa % PGSIZE) == 0, "vm_mappages: pa not aligned");
    assert(len > 0, "vm_mappages: len must be positive");
    assert(va + len <= VA_MAX, "vm_mappages: va + len exceeds VA_MAX");
    
    // 向上舍入到页边界
    uint64 end_va = va + ((len + PGSIZE - 1) / PGSIZE) * PGSIZE;
    
    // 按页对齐长度到页边界
    for (uint64 curr_va = va, curr_pa = pa; curr_va < end_va; curr_va += PGSIZE, curr_pa += PGSIZE) {
        pte_t *pte = vm_getpte(pgtbl, curr_va, true);
        if (pte == NULL) {
            panic("vm_mappages: vm_getpte failed");
        }
        
        // 如果已经映射，则更新映射（允许重新映射以更改权限）
        if (*pte & PTE_V) {
            // 允许重新映射到相同的物理地址或不同的物理地址
            *pte = PA_TO_PTE(curr_pa) | perm | PTE_V;
        } else {
            // 设置新映射
            *pte = PA_TO_PTE(curr_pa) | perm | PTE_V;
        }
    }
}

// 解除pgtbl中[va, va+len)区域的映射
// 如果freeit == true则释放对应物理页, 默认是用户的物理页
void vm_unmappages(pgtbl_t pgtbl, uint64 va, uint64 len, bool freeit)
{
    // 参数检查
    assert((va % PGSIZE) == 0, "vm_unmappages: va not aligned");
    assert(len > 0, "vm_unmappages: len must be positive");
    assert(va + len <= VA_MAX, "vm_unmappages: va + len exceeds VA_MAX");
    
    // 向上舍入到页边界
    uint64 end_va = va + ((len + PGSIZE - 1) / PGSIZE) * PGSIZE;
    
    // 按页解除映射
    for (uint64 curr_va = va; curr_va < end_va; curr_va += PGSIZE) {
        pte_t *pte = vm_getpte(pgtbl, curr_va, false);
        if (pte == NULL || !(*pte & PTE_V)) {
            // 页面未映射，跳过
            continue;
        }
        
        // 如果需要释放物理页
        if (freeit) {
            uint64 pa = PTE_TO_PA(*pte);
            pmem_free(pa, false); // 默认释放用户物理页
        }
        
        // 清除PTE
        *pte = 0;
    }
}

// 完成UART、CLINT、PLIC、内核代码区、内核数据区、可分配区域的页表映射
// 相当于部分填充kernel_pgtbl
void kvm_init()
{
    // 分配内核页表
    kernel_pgtbl = (pgtbl_t)pmem_alloc(true);
    
    // UART映射 (0x10000000)
    vm_mappages(kernel_pgtbl, 0x10000000ul, 0x10000000ul, PGSIZE, PTE_R | PTE_W);
    
    // CLINT映射 (0x2000000)
    vm_mappages(kernel_pgtbl, CLINT_BASE, CLINT_BASE, 0x10000, PTE_R | PTE_W);
    
    // PLIC映射 (0x0c000000)
    vm_mappages(kernel_pgtbl, PLIC_BASE, PLIC_BASE, 0x400000, PTE_R | PTE_W);
    
    // 内核代码区域映射 (KERNEL_BASE ~ KERNEL_DATA)
    uint64 kernel_code_size = (uint64)KERNEL_DATA - KERNEL_BASE;
    vm_mappages(kernel_pgtbl, KERNEL_BASE, KERNEL_BASE, kernel_code_size, PTE_R | PTE_X);
    
    // 内核数据区域映射 (KERNEL_DATA ~ ALLOC_BEGIN)
    uint64 kernel_data_size = (uint64)ALLOC_BEGIN - (uint64)KERNEL_DATA;
    vm_mappages(kernel_pgtbl, (uint64)KERNEL_DATA, (uint64)KERNEL_DATA, kernel_data_size, PTE_R | PTE_W);
    
    // 可分配区域映射 (ALLOC_BEGIN ~ ALLOC_END)
    uint64 alloc_size = (uint64)ALLOC_END - (uint64)ALLOC_BEGIN;
    vm_mappages(kernel_pgtbl, (uint64)ALLOC_BEGIN, (uint64)ALLOC_BEGIN, alloc_size, PTE_R | PTE_W);

    // trampoline: 需要在内核/用户页表均映射到同一高地址
    extern char trampoline[];
    vm_mappages(kernel_pgtbl, TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X);

    // KSTACK(0): 进程0的内核栈映射（2个页面）
    void *kstack0_pa = pmem_alloc(false);
    if (kstack0_pa == NULL) panic("kvm_init: alloc kstack0 failed");
    vm_mappages(kernel_pgtbl, KSTACK(0), (uint64)kstack0_pa, 2 * PGSIZE, PTE_R | PTE_W);
}

// 每个CPU都需要调用, 从不使用页表切换到使用内核页表
// 切换后需要刷新TLB里面的缓存
void kvm_inithart()
{
    w_satp(MAKE_SATP(kernel_pgtbl));
    sfence_vma();
}

// 输出页表内容(for debug)
void vm_print(pgtbl_t pgtbl)
{
    // 顶级页表，次级页表，低级页表
    pgtbl_t pgtbl_2 = pgtbl, pgtbl_1 = NULL, pgtbl_0 = NULL;
    pte_t pte;

    printf("level-2 pgtbl: pa = %p\n", pgtbl_2);
    for (int i = 0; i < PGSIZE / sizeof(pte_t); i++)
    {
        pte = pgtbl_2[i];
        if (!((pte)&PTE_V))
            continue;
        assert(PTE_CHECK(pte), "vm_print: pte check fail (1)");
        pgtbl_1 = (pgtbl_t)PTE_TO_PA(pte);
        printf(".. level-1 pgtbl %d: pa = %p\n", i, pgtbl_1);

        for (int j = 0; j < PGSIZE / sizeof(pte_t); j++)
        {
            pte = pgtbl_1[j];
            if (!((pte)&PTE_V))
                continue;
            assert(PTE_CHECK(pte), "vm_print: pte check fail (2)");
            pgtbl_0 = (pgtbl_t)PTE_TO_PA(pte);
            printf(".. .. level-0 pgtbl %d: pa = %p\n", j, pgtbl_0);

            for (int k = 0; k < PGSIZE / sizeof(pte_t); k++)
            {
                pte = pgtbl_0[k];
                if (!((pte)&PTE_V))
                    continue;
                assert(!PTE_CHECK(pte), "vm_print: pte check fail (3)");
                printf(".. .. .. physical page %d: pa = %p flags = %d\n", k, (uint64)PTE_TO_PA(pte), (int)PTE_FLAGS(pte));
            }
        }
    }
}
