#ifndef HEAP_H
#define HEAP_H

#include <stddef.h>
#include <limine.h>

void heap_init(struct limine_hhdm_response *hhdm);

void *kmalloc(size_t size);

void kfree(void *ptr);

void *krealloc(void *ptr, size_t new_size);

void heap_print_stats(void);

void test_heap(void);

#endif // HEAP_H