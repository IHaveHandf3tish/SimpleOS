#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <limine.h>
#include <string.h>
#include <serial.h>
#include <kprintf.h>
#include <gdt.h>
#include <idt.h>
#include <memmap.h>
#include <pmm.h>
#include <vmm.h>
#include <slab.h>
#include <heap.h>


//------- Limine Requests (send them to a different .c file later)-------

__attribute__((used, section(".limine_requests")))
volatile struct limine_hhdm_request hhdm_request = {
    .id = LIMINE_HHDM_REQUEST_ID,
    .revision = 0
};

__attribute__((used, section(".limine_requests")))
volatile struct limine_memmap_request memmap_request = {
    .id = LIMINE_MEMMAP_REQUEST_ID,
    .revision = 0
};
__attribute__((used, section(".limine_requests")))
volatile struct limine_executable_address_request kernel_address_request = {
    .id = LIMINE_EXECUTABLE_ADDRESS_REQUEST_ID,
    .revision = 0
};

// Set the base revision to 4, this is recommended as this is the latest
// base revision described by the Limine boot protocol specification.
// See specification for further info.

__attribute__((used, section(".limine_requests")))
static volatile uint64_t limine_base_revision[] = LIMINE_BASE_REVISION(4);

// The Limine requests can be placed anywhere, but it is important that
// the compiler does not optimise them away, so, usually, they should
// be made volatile or equivalent, _and_ they should be accessed at least
// once or marked as used with the "used" attribute as done here.

__attribute__((used, section(".limine_requests")))
static volatile struct limine_framebuffer_request framebuffer_request = {
    .id = LIMINE_FRAMEBUFFER_REQUEST_ID,
    .revision = 0
};

// Finally, define the start and end markers for the Limine requests.
// These can also be moved anywhere, to any .c file, as seen fit.

__attribute__((used, section(".limine_requests_start")))
static volatile uint64_t limine_requests_start_marker[] = LIMINE_REQUESTS_START_MARKER;

__attribute__((used, section(".limine_requests_end")))
static volatile uint64_t limine_requests_end_marker[] = LIMINE_REQUESTS_END_MARKER;

// GCC and Clang reserve the right to generate calls to the following
// 4 functions even if they are not directly called.
// Implement them as the C specification mandates.
// DO NOT remove or rename these functions, or stuff will eventually break!
// They CAN be moved to a different .c file.


// Halt and catch fire function.
static void hcf(void) {
    for (;;) {
        asm ("hlt");
    }
}
//----- Credit to Osdev for these starter kmain functions, although they might be subject to change in the future -----//
// The following will be our kernel's entry point.
// If renaming kmain() to something else, make sure to change the
// linker script accordingly.
void kmain(void) {
    // Ensure the bootloader actually understands our base revision (see spec).
    if (LIMINE_BASE_REVISION_SUPPORTED(limine_base_revision) == false) {
        hcf();
    }

    // Ensure we got a framebuffer.
    if (framebuffer_request.response == NULL
     || framebuffer_request.response->framebuffer_count < 1) {
        hcf();
    }

    // Fetch the first framebuffer.
    struct limine_framebuffer *framebuffer = framebuffer_request.response->framebuffers[0];

    // Note: we assume the framebuffer model is RGB with 32-bit pixels.
    for (size_t i = 0; i < 100; i++) {
        volatile uint32_t *fb_ptr = framebuffer->address;
        fb_ptr[i * (framebuffer->pitch / 4) + i] = 0xffffff;
    }
    
    //first function calls/tests subject to change
    init_serial();
    write_string_serial("Hello from SimpleOS! Serial logging is working.\n");
    kprintf("Hello World from kprintf!\n");
    kprintf("Numbers: %d, Negative: %d\n", 123, -456);
    kprintf("Hex: 0x%x\n", 0xDEADBEEF);
    init_gdt(); // <--- Add this!
    kprintf("GDT Loaded successfully.\n");
    init_idt(); // Initialize the IDT
    print_memmap();
    
    pmm_init(memmap_request.response, hhdm_request.response);
    test_pmm(); 
    vmm_init();
    // Simple HHDM test(might delete later or add to vmm tests)
    uint64_t *test_ptr = (uint64_t*)(hhdm_request.response->offset + 0x200000);
    *test_ptr = 0xCAFEBABE;
    
    if (*test_ptr == 0xCAFEBABE) {
        kprintf("HHDM Write Test Passed: [0x%x] = 0x%x\n", test_ptr, *test_ptr);
    } else {
        kprintf("HHDM Write Test Failed!\n");
    }
    test_vmm(); 
    slab_init();
    heap_init(hhdm_request.response);
    test_heap();

    // We're done, just hang...
    hcf();
}
