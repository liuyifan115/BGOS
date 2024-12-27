#include <xtos.h>

#define CSR_PRMD 0x1
#define CSR_ERA 0x6
#define CSR_PRMD_PPLV (3UL << 0)
#define CSR_PRMD_PIE (1UL << 2)
#define VMEM_SIZE (1UL << (9 + 9 + 12))

void main()
{
	mem_init();
	test_buddy_allocator();
}



// 测试buddy allocator
void test_buddy_allocator() {
    void *ptr1, *ptr2, *ptr3;

    // 尝试分配8KB
    printk("Trying to allocate 8KB...\n");
    if (!(ptr1 = buddy_alloc(8192))) printk("Failed to allocate 8KB\n");
    else printk("Allocated 8KB\n");

    // 尝试分配16KB
    printk("Trying to allocate 16KB...\n");
    if (!(ptr2 = buddy_alloc(16384))) printk("Failed to allocate 16KB\n");
    else printk("Allocated 16KB\n");

    // 尝试分配4KB
    printk("Trying to allocate 4KB...\n");
    if (!(ptr3 = buddy_alloc(4096))) printk("Failed to allocate 4KB\n");
    else printk("Allocated 4KB\n");

    // 释放16KB
    if (ptr2) {
        printk("Freeing 16KB...\n");
        buddy_free(ptr2, 16384);
        printk("Freed 16KB\n");
    }

    // 重新尝试分配16KB
    printk("Trying to allocate 16KB again...\n");
    if (!(ptr2 = buddy_alloc(16384))) printk("Failed to allocate 16KB\n");
    else printk("Allocated 16KB\n");

    // 释放8KB
    if (ptr1) {
        printk("Freeing 8KB...\n");
        buddy_free(ptr1, 8192);
        printk("Freed 8KB\n");
    }

    // 释放4KB
    if (ptr3) {
        printk("Freeing 4KB...\n");
        buddy_free(ptr3, 4096);
        printk("Freed 4KB\n");
    }

    // 最后再释放一次16KB
    if (ptr2) {
        printk("Freeing 16KB again...\n");
        buddy_free(ptr2, 16384);
        printk("Freed 16KB\n");
    }
}