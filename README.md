# LAB-4 实验报告

## 过程日志
1. 2025.10.20 更新lab4文件
2. 2025.10.20 张子扬初步完成实验，存在panic问题  
3. 2025.10.23 王俊翔完善实验修改存在的错误版本 
4. 2025.10.27 王俊翔完成测试二剩余部分
5. 2025.11.03 张子扬、王俊翔完善README
## 代码组织结构
```
OKOS
├── LICENSE        开源协议
├── .vscode        配置了可视化调试环境
├── registers.xml  配置了可视化调试环境
├── .gdbinit.tmp-riscv xv6自带的调试配置
├── common.mk      Makefile中一些工具链的定义
├── Makefile       编译运行整个项目
├── kernel.ld      定义了内核程序在链接时的布局 (CHANGE, 支持trampsec)
├── pictures       README使用的图片目录 (CHANGE, 日常更新)
├── README.md      实验报告 (DONE)
├── lab4-README.md 实验指导书 (CHANGE, 日常更新)
└── src            源码
    ├── kernel     内核源码
    │   ├── arch   RISC-V相关
    │   │   ├── method.h
    │   │   ├── mod.h
    │   │   └── type.h
    │   ├── boot   机器启动
    │   │   ├── entry.S
    │   │   └── start.c
    │   ├── lock   锁机制
    │   │   ├── spinlock.c
    │   │   ├── method.h
    │   │   ├── mod.h
    │   │   └── type.h
    │   ├── lib    常用库
    │   │   ├── cpu.c (CHANGE, 新增myproc函数)
    │   │   ├── print.c
    │   │   ├── uart.c
    │   │   ├── utils.c
    │   │   ├── method.h (CHANGE, 新增myproc函数)
    │   │   ├── mod.h
    │   │   └── type.h (CHANGE, 扩充CPU结构体 + 帮助)
    │   ├── mem    内存模块
    │   │   ├── pmem.c
    │   │   ├── kvm.c (DONE, 增加内核页表的映射内容 trampoline + KSTACK(0))
    │   │   ├── method.h
    │   │   ├── mod.h
    │   │   └── type.h
    │   ├── trap   陷阱模块
    │   │   ├── plic.c
    │   │   ├── timer.c
    │   │   ├── trap_kernel.c (CHANGE, 去掉了提示信息的static标记)
    │   │   ├── trap_user.c (DONE, 用户态陷阱处理)
    │   │   ├── trap.S
    │   │   ├── trampoline.S
    │   │   ├── method.h (CHANGE, 增加函数定义)
    │   │   ├── mod.h
    │   │   └── type.h
    │   ├── proc   进程模块
    │   │   ├── proc.c (DONE, 进程管理核心逻辑)
    │   │   ├── swtch.S (NEW, 上下文切换)
    │   │   ├── method.h (NEW)
    │   │   ├── mod.h (NEW)
    │   │   └── type.h (NEW)
    │   └── main.c (CHANGE, 日常更新)
    └── user       用户程序
        ├── initcode.c (NEW)
        ├── sys.h (NEW)
        ├── syscall_arch.h (NEW)
        └── syscall_num.h (NEW)
```

## 核心知识

### 1. 进程控制块 (PCB) 结构

>进程控制块是操作系统管理进程的核心数据结构，在我们的实现中对应 `proc_t` 结构：

```
+----------------------+
|      proc_t          |
+----------------------+
| pid: 进程ID          |
| state: 进程状态      |
| kstack: 内核栈指针   |
| pgtbl: 用户页表      |
| tf: trapframe指针    |
| ctx: 上下文          |
| ustack_npage: 用户栈页数|
| heap_top: 堆顶地址   |
+----------------------+
```

### 2. Trapframe 结构

>Trapframe 保存用户态到内核态切换时的寄存器状态：

```
+----------------------+
|    trapframe_t       |
+----------------------+
| user_to_kern_satp    | 内核页表
| user_to_kern_sp      | 内核栈指针
| user_to_kern_trapvector| trap处理函数
| user_to_kern_epc     | 用户程序入口点
| user_to_kern_hartid  | 硬件线程ID
| sp                   | 用户栈指针
| ra, sp, gp, tp...    | 通用寄存器
| a0-a7                | 参数寄存器
| s0-s11               | 保存寄存器
| t0-t6                | 临时寄存器
+----------------------+
```



## 执行流程分析

### 1. 进程创建阶段 ([`proc_make_first`](src/kernel/proc/proc.c#L59))

**状态变化**:
```
内核态 → 准备用户环境 → 设置 trapframe → 准备切换
```

**代码流程**:
 1. 分配 trapframe 内存
 2. 创建用户页表
 3. 映射用户栈和代码页
 4. 设置 trapframe 关键字段

### 寻找用户程序逻辑入口地址：
>虚拟地址映射：initcode 二进制被映射到用户虚拟页 PGSIZE = 0x1000，反汇编中偏移 0x2c 对应运行时地址 0x102c  

源代码（嵌入的机器码数组）：  
([`unsigned char target_user_initcode[]`](src/user/initcode.h)) 

把以上的汇编翻译成C的伪代码如下，方便理解分析：
```c
// 地址 0x1000 + 0x0（反汇编偏移 0x0）
// helper routine，作用：把传入的参数放到 a7，然后触发 ecall，返回 a0
long helper(long arg /* in a0 */) {
    // prologue: push/alloc frame (sp -= 32), save s0, set fp
    // sd x8, 24(sp); fp = sp + 32;
    // store arg into frame: store a0 at frame offset
    long saved_arg = arg;           // sd x10, -24(fp)
    long a7 = saved_arg;            // ld x17, -24(fp) -> x17 = a7
    // syscall:
    syscall();                      // ecall
    // after ecall: move return into a0 as needed (mv x15,x10; mv x10,x15)
    long ret = /* a0 after ecall */;
    // epilogue: restore s0, free frame, return
    return ret;                     // ret
}
// 地址 0x1000 + 0x2c（反汇编偏移 0x2c）
// main entry，程序入口，从这里开始执行（所以 EPC 要指向这里）
int main(void) {
    // prologue: sp -= 16; save ra, save s0; fp = sp + 16
    // prepare first call
    long a0 = 0;
    helper(a0);      // auipc + jalr -> 调用 helper 位于同一页的 0x0 处 (位置无关调用)
    // prepare second call
    a0 = 0;
    helper(a0);      // 再一次调用 helper
    // finished: busy loop
    for(;;) { /* spin */ }  // j 0x54
}
```

### 为什么要[`p->tf->user_to_kern_epc = USER_BASE + INITCODE_ENTRY_OFFSET`](src/kernel/proc/proc.c#L95)？

- 用户代码页被映射到 `USER_BASE`（`#define USER_BASE (PGSIZE)`，即 0x1000）。
- `initcode` 是嵌入到内核的用户程序机器码（由 `user/initcode.c` 生成并经由 `initcode.h` 引入），加载时从该页的起始处拷贝到物理页，再以 `PTE_R|PTE_X|PTE_U` 映射到用户虚拟地址 `USER_BASE`。
- 反汇编可以看到真正的“程序入口”在该页内偏移 `0x2c` 处（helper 在 0x0，main 在 0x2c）。因此运行时入口虚拟地址应为 `USER_BASE + 0x2c`。
- `trap_user_return()` 会把 `tf->user_to_kern_epc` 写入 `sepc`，随后执行 `sret` 返回用户态，从而从该虚拟地址开始取指执行。


后续可能出现的问题：该偏移取决于当前 `initcode` 的内容与编译结果，若用户程序发生结构性变更（例如新增 prologue），入口偏移可能变化，需相应更新 `INITCODE_ENTRY_OFFSET`（或改为通过符号导出入口地址以避免硬编码）。

### 2. 用户态返回阶段 ([`trap_user_return`](src/kernel/trap/trap_user.c#L72))

**状态变化**:
```
内核态 → 设置 trap 向量 → 设置用户页表 → 执行 sret → 用户态
```

**关键寄存器设置**:
- `stvec`: 指向 trampoline 的 `user_vector`
- `sscratch`: 设置为 `TRAPFRAME` 虚拟地址
- `sstatus`: 清除 SPP，设置 SPIE
- `sepc`: 设置为 `USER_BASE + INITCODE_ENTRY_OFFSET`
- 
### 3. 用户程序执行阶段

**状态变化**:
```
用户态执行 → 调用 helper(0) → 执行 ecall → 触发 trap
```
也就是用户态的main函数，调用两次syscall, "helloworld"。  

### 4. 系统调用处理阶段

**状态变化**:
```
用户态 → 硬件 trap → trampoline → 内核态 → 处理系统调用 → 返回用户态
```

**详细流程**:

1. **硬件自动操作**:
   - 保存 `sepc` (当前 PC)
   - 设置 `scause = 8` (U-mode ecall)
   - 跳转到 `stvec` 指向的 trampoline

2. **Trampoline 处理** (`user_vector`):
   ```asm
   csrrw a0, sscratch, a0  ; 交换 a0 和 sscratch
   sd ra, 40(a0)           ; 保存所有寄存器到 trapframe
   sd sp, 48(a0)
   ...                     ; 保存其他寄存器
   ld sp, 8(a0)            ; 加载内核栈指针
   ld t0, 16(a0)           ; 加载 trap 处理函数
   jr t0                   ; 跳转到 trap_user_handler
   ```

3. **内核系统调用处理** (`trap_user_handler`):
   ```c
   // 识别 ecall from U-mode
   if (trap_id == 8) {
       tf->user_to_kern_epc += 4;  // 跳过 ecall 指令
       if (tf->a7 == 0) {
           printf("proczero: hello world\n");
       }
   }
   ```

4. **返回用户态** (`trap_user_return`):
   - 恢复用户寄存器
   - 切换到用户页表
   - 执行 `sret` 返回用户程序



## 测试样例
![alt image](pictures/测试helloworld_time_uart.png)
通过测试样例，我们实现了以下的目标：
1.  验证系统调用功能：
    *   创建并运行第一个用户进程 `proczero`。
    *   `proczero` 执行的用户程序 (`initcode.c`) 会发起两次 `SYS_helloworld` 系统调用。

2.  **验证用户态下的中断处理**：
    *   在用户进程 `proczero` 运行时，系统能正确响应——时钟中断: tick计数器的变化。（tick=100, tick=200...）
    *   在用户进程 `proczero` 运行时，系统能正确响应——串口中断: 其他字符（test, balal, balabala等字符）

总而言之，测试样例验证了从内核态到用户态的切换、用户进程的执行、系统调用的完整处理流程以及在用户态下中断响应的正确性。

## 五、设计取舍与反思

- 从 CSAPP 的“异常控制流”到 OS 的“trap/中断/系统调用”
    - CSAPP 里把系统调用看作一种异常控制流（ECF），这次实验里亲手把这条链路打通：U-mode 执行 ecall → sepc/scause 自动更新 → 跳到 `stvec` 的 trampoline → 保存寄存器到 `trapframe` → 进入内核的 `trap_user_handler` → 处理后返回 `trap_user_return` → `sret` 回到用户。教材里的概念，变成了寄存器级别、地址级别的真实实现。
- 初始用户入口与 initcode 布局：
    - xv6：`initcode.S` 链接在 VA=0 起始，入口通常为 0；内核把 `p->trapframe->epc` 设为 0 后返回用户，随后 `initcode` 会 `exec("/init")` 拉起真正的 init。
    - 本实现：把用户代码页映射到 `USER_BASE=0x1000`，入口是页内偏移 `0x2c`，即 `USER_BASE + 0x2c = 0x102c`；用“页基址+页内偏移”的表达替代写死绝对地址。

