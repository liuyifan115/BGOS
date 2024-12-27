#include <xtos.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdbool.h>

#define CSR_BADV 0x7
#define CSR_PWCL 0x1c
#define CSR_DMW0 0x180
#define CSR_DMW3 0x183
#define CSR_DMW0_PLV0 (1UL << 0)
#define MEMORY_SIZE 0x8000000 // 定义内存大小为128MB
#define NR_PAGE (MEMORY_SIZE >> 12) // 页数，每页4KB
#define KERNEL_START_PAGE (0x200000UL >> 12) // 内核起始页
#define KERNEL_END_PAGE (0x300000UL >> 12) // 内核结束页
#define ENTRY_SIZE 8 // 页表项大小
#define PWCL_PTBASE 12
#define PWCL_PTWIDTH 9
#define PWCL_PDBASE 21
#define PWCL_PDWIDTH 9
#define PWCL_EWIDTH 0
#define ENTRYS 512 // 页表条目数
#define MAX_ORDER 14 // 最大阶数
#define PAGE_SIZE 4096 // 每页大小4KB
#define BUDDY_MASK (~(PAGE_SIZE - 1)) // 伙伴掩码，用于对齐地址

typedef struct buddy {
    struct buddy *next; // 指向下一个空闲块
} buddy_t;

buddy_t *free_list[MAX_ORDER + 1]; // 自由链表数组，每个链表存储相同阶数的空闲块
char mem_map[NR_PAGE]; // 内存页映射表，用于标记每页的状态
unsigned int bitmap[MAX_ORDER + 1]; // 位向量，加速查找
extern struct process *current; // 当前进程指针
extern struct shmem shmem_table[NR_SHMEM]; // 共享内存表

// 计算所需大小对应的阶数
static inline int get_order(unsigned int size) {
    int order = 0;
    size = (size - 1) >> 12; // 将大小转换为页数
    while (size > 0) {
        order++;
        size >>= 1; // 每次右移一位，计算阶数
    }
    return order;
}

// 计算伙伴块的地址
static inline unsigned long buddy_of(unsigned long addr, int order) {
    return addr ^ (1 << (order + 12)); // 伙伴块地址通过异或计算得出
}

// 更新位向量,置1
static inline void set_bit(int order) {
    bitmap[order] |= 1U;
}

//置0
static inline void clear_bit(int order) {
    bitmap[order] &= ~1U;
}

// 初始化伙伴分配器
void buddy_init(void) {
    // 初始化所有级别的自由链表和位图
    for (int i = 0; i <= MAX_ORDER; i++) {
        free_list[i] = NULL;      // 每个级别的自由链表初始为空
        bitmap[i] = 0;            // 每个级别的位图初始为0，表示未分配
    }

    unsigned long base_addr = 0x300000; // 定义内存池的起始地址
    int max_order = MAX_ORDER;          // 获取最大块大小对应的级别

    // 将整个内存池作为一个最大级别的空闲块加入到自由链表中
    free_list[max_order] = (buddy_t *)base_addr;
    free_list[max_order]->next = NULL;

    set_bit(max_order); // 在位图中标记该级别的第一个块为空闲状态

    //print_debug("Buddy system initialized. Memory starts at ", base_addr);
}

// 延迟分割分配指定大小的内存块
void *buddy_alloc(unsigned int size) {
    // 计算所需内存大小对应的伙伴系统阶数（order）
    int order = get_order(size);
    
    // 遍历从所需阶数到最大阶数的自由链表，寻找可用的内存块
    for (int i = order; i <= MAX_ORDER; i++) {
        if (bitmap[i]) { // 检查位图以快速定位有空闲块的阶数
            buddy_t *block = free_list[i]; // 获取该阶数的第一个空闲块
            free_list[i] = block->next;   // 更新自由链表，移除已分配块

            if (!free_list[i]) clear_bit(i); // 如果没有剩余块，清除位图中的标记

            // 如果找到的块比需要的大，则进行递归分割，直到匹配所需的阶数
            while (i > order) {
                i--;
                unsigned long split_addr = (unsigned long)block + (1 << (i + 12));
                buddy_t *split_block = (buddy_t *)split_addr;
                
                // 将新分裂出的块加入到当前阶数的自由链表中
                split_block->next = free_list[i];
                free_list[i] = split_block;
                set_bit(i); // 更新位图，标记新的空闲块
                
                // 注释掉的调试信息可以在这里重新启用以帮助调试
                //print_debug("Splitting block. New block at ", split_addr);
                //print_debug("Order: ", i);
            }

            // 标记所分配的页为占用状态
            unsigned long start_page = (unsigned long)block >> 12;
            for (int j = 0; j < (1 << order); j++) {
                mem_map[start_page + j] = 1; // 设置页面为已使用
            }

            // 初始化分配的内存块，并返回虚拟地址
            unsigned long aligned_address = ((unsigned long)block | DMW_MASK);
            set_mem((char *)aligned_address, 0, PAGE_SIZE); // 清零分配的内存
            print_debug("Allocated page at address: ", aligned_address);

            return (void *)aligned_address; // 返回对齐后的虚拟地址
        }
    }

    // 如果未能找到足够大的空闲块，则分配失败
    print_debug("Failed to allocate memory of size ", size);
    return NULL; // 返回NULL表示分配失败
}

// 不积极合并释放指定大小的内存块
void buddy_free(void *addr, unsigned int size) {
    // 计算要释放的块对应的阶数（order）
    int order = get_order(size);

    // 获取物理地址（去掉可能存在的对齐掩码）
    unsigned long phys_addr = (unsigned long)addr & ~DMW_MASK;

    // 计算伙伴块的地址
    unsigned long buddy = buddy_of(phys_addr, order);

    // 更新mem_map，标记所释放的页为未占用状态
    unsigned long start_page = phys_addr >> 12; 
    int flag = 1; // 标志位，用于检查所有涉及页面是否都已完全释放

    for (int j = 0; j < (1 << order); j++) {
        mem_map[start_page + j]--; // 减少引用计数，表示页面被部分释放
        if (mem_map[start_page + j] != 0) {
            flag = 0; // 如果某个页面还有其他引用，则设置标志位为0
        }
    }

    // 如果所有涉及页面都已完全释放（即没有其他引用），则进行合并操作
    if (flag) { // 兼顾页共享情况
        print_debug("Freeing block at address: ", phys_addr);
        print_debug("Size: ", size);

        // 尝试与伙伴块合并，直到无法合并或达到最大阶数
        while (order < MAX_ORDER && free_list[order] == (buddy_t *)buddy) {
            // 合并当前块和它的伙伴块，并更新物理地址为两者中较小的一个
            phys_addr = (phys_addr & buddy); // 选择两个地址中较小的那个作为新的起始地址
            buddy = buddy_of(phys_addr, ++order); // 计算更高一级的伙伴块地址
            
            //print_debug("Merging with buddy at address: ", buddy);
            //print_debug("New order: ", order);
        }

        // 将合并后的块加入到自由链表中
        ((buddy_t *)phys_addr)->next = free_list[order];
        free_list[order] = (buddy_t *)phys_addr;
        set_bit(order); // 在位图中标记该阶数的块为空闲

        print_debug("Block merged and added to free list at order: ", order);
    } 
}

// 分配一个页大小的内存块
unsigned long get_page() {
    unsigned long page = (unsigned long)buddy_alloc(PAGE_SIZE); // 分配4KB块
    if (page == 0) {
        panic("panic: out of memory!\n"); // 内存不足，触发恐慌
    }
    
    
    return page;
    
}

// 释放一个页大小的内存块
void free_page(unsigned long page) {
    print_debug("Freeing page at ", page);
    buddy_free((void *)page, PAGE_SIZE); // 释放4KB块
}


// 增加页的共享计数
void share_page(unsigned long page)
{
	unsigned long i;

	i = (page & ~DMW_MASK) >> 12;
	if (!mem_map[i])
		panic("panic: try to share free page!\n");
	mem_map[i]++;
}
int is_share_page(unsigned long page)
{
	unsigned long i;

	i = (page & ~DMW_MASK) >> 12;
	if (mem_map[i] > 1)
		return 1;
	else
		return 0;
}
unsigned long *get_pte(struct process *p, unsigned long u_vaddr) {
    unsigned long pd, pt;
    unsigned long *pde, *pte;

    pd = p->page_directory; // 获取页目录基地址
    pde = (unsigned long *)(pd + ((u_vaddr >> 21) & 0x1ff) * ENTRY_SIZE); // 计算页目录项地址

    if (*pde) { // 如果页目录项有效
        pt = *pde | DMW_MASK; // 获取页表地址
    } else { // 如果页目录项无效
        pt = get_page(); // 分配一个新页作为页表
        *pde = pt & ~DMW_MASK; // 更新页目录项
        //print_debug("New page table allocated at: \n", pt); // 添加调试信息
    }

    pte = (unsigned long *)(pt + ((u_vaddr >> 12) & 0x1ff) * ENTRY_SIZE); // 计算页表项地址
    //print_debug("PDE: ", *pde); 
    //print_debug(" PTE: \n",*pte);// 添加调试信息
    return pte; // 返回页表项指针
}
void put_page(struct process *p, unsigned long u_vaddr, unsigned long k_vaddr, unsigned long attr) {
    unsigned long *pte;

    pte = get_pte(p, u_vaddr); // 获取虚拟地址对应的页表项指针
    if (*pte) // 如果页表项已经存在
        panic("panic: try to remap!\n"); // 抛出错误

    *pte = (k_vaddr & ~DMW_MASK) | attr; // 设置页表项，映射物理地址和属性
    //print_debug("Setting PTE: \n", *pte); // 添加调试信息
    invalidate(); // 刷新TLB
}


void free_page_table(struct process *p)
{
	unsigned long pd, pt;
	unsigned long *pde, *pte;
	unsigned long page;

	pd = p->page_directory;
	pde = (unsigned long *)pd;
	for (int i = 0; i < ENTRYS; i++, pde++)
	{
		if (*pde == 0)
			continue;
		pt = *pde | DMW_MASK;
		pte = (unsigned long *)pt;
		for (int j = 0; j < ENTRYS; j++, pte++)
		{
			if (*pte == 0)
				continue;
			page = (~0xfffUL & *pte) | DMW_MASK;
			if (is_share_page(page) && (*pte & PTE_D))
			{
				for (i = 0; i < NR_SHMEM; i++)
				{
					if (shmem_table[i].mem == page)
					{
						shmem_table[i].count--;
						break;
					}
				}
			}
			free_page(page);
			*pte = 0;
		}
		free_page(*pde | DMW_MASK);
		*pde = 0;
	}
}
void copy_page_table(struct process *from, struct process *to)
{
	unsigned long from_pd, to_pd, from_pt, to_pt;
	unsigned long *from_pde, *to_pde, *from_pte, *to_pte;
	unsigned long page;
	int i, j;

	from_pd = from->page_directory;
	from_pde = (unsigned long *)from_pd;
	to_pd = to->page_directory;
	to_pde = (unsigned long *)to_pd;
	for (i = 0; i < ENTRYS; i++, from_pde++, to_pde++)
	{
		if (*from_pde == 0)
			continue;
		from_pt = *from_pde | DMW_MASK;
		from_pte = (unsigned long *)from_pt;
		to_pt = get_page();
		to_pte = (unsigned long *)to_pt;
		*to_pde = to_pt & ~DMW_MASK;
		for (j = 0; j < ENTRYS; j++, from_pte++, to_pte++)
		{
			if (*from_pte == 0)
				continue;
			page = (~0xfffUL & *from_pte) | DMW_MASK;
			if (is_share_page(page) && (*from_pte & PTE_D))
				continue;
			share_page(page);
			*from_pte &= ~PTE_D;
			*to_pte = *from_pte;
		}
	}
	invalidate();
}
void do_wp_page()
{
	unsigned long *pte;
	unsigned long u_vaddr;
	unsigned long old_page, new_page;

	u_vaddr = read_csr_64(CSR_BADV);
	//print_debug("Write-protect exception at address: \n", u_vaddr);
	pte = get_pte(current, u_vaddr);
	old_page = (~0xfff & *pte) | DMW_MASK;
	if (is_share_page(old_page))
	{
		new_page = get_page();
		*pte = (new_page & ~DMW_MASK) | PTE_PLV | PTE_D | PTE_V;
		copy_mem((char *)new_page, (char *)old_page, PAGE_SIZE);
		free_page(old_page);
	}
	else
		*pte |= PTE_D;
	invalidate();
}
void do_no_page() {
    unsigned long page;
    unsigned long u_vaddr;

    u_vaddr = read_csr_64(CSR_BADV);
    //print_debug("Page-fault exception at address: \n", u_vaddr); // 添加调试信息
    u_vaddr &= ~0xfffUL;
    //print_debug("Aligned address: %lx\n", u_vaddr); // 添加调试信息
    page = get_page();
    if (u_vaddr < current->exe_end)
        get_exe_page(u_vaddr, page);
    put_page(current, u_vaddr, page, PTE_PLV | PTE_D | PTE_V);
    //print_debug("Mapped virtual address:  to physical address: \n", u_vaddr); // 添加调试信息
}

// 内存初始化
void mem_init() {
    buddy_init();
    for (int i = 0; i < NR_PAGE; i++) {
        if (i >= KERNEL_START_PAGE && i < KERNEL_END_PAGE)
            mem_map[i] = 1;
        else
            mem_map[i] = 0;
    }
    write_csr_64(CSR_DMW0_PLV0 | DMW_MASK, CSR_DMW0);
    write_csr_64(0, CSR_DMW3);
    write_csr_64((PWCL_EWIDTH << 30) | (PWCL_PDWIDTH << 15) | (PWCL_PDBASE << 10) | (PWCL_PTWIDTH << 5) | (PWCL_PTBASE << 0), CSR_PWCL);
    invalidate();
    
    //shmem_init();
}

