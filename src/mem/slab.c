// since we're building an Hypervisor with a Desktop interface, we need slab allocator for small objects
// my dog still tryna scratch his ass, so we will add this later
/*

typedef struct _SLAB {
   CACHE*          cache;
   LIST_ENTRY      listEntry;
   size_t          objectCount;
   size_t          usedObjects;
   size_t          bufferSize;
   union {
      void*         freeList;
      LIST_ENTRY    bufferControlFreeListHead;
   }u;
}SLAB;

typedef struct _BUFCTRL {
   void* buffer;
   SLAB* parent;
   LIST_ENTRY entry;
}BUFCTRL;

typedef struct _CACHE {
   size_t            size;
   int               align;
   int               flags;
   SPIN_LOCK         lock;
   LIST_ENTRY        fullSlabListHead;
   LIST_ENTRY        partialSlabListHead;
   LIST_ENTRY        emptySlabListHead;
   LIST_ENTRY        listEntry;
}CACHE;


struct vm_descriptor *vm = kmalloc(sizeof(*vm));  // VM metadata
struct vcpu *vcpu = kmalloc(sizeof(*vcpu));       // vCPU state
struct page_table *ept = kmalloc(sizeof(*ept));   // EPT entries
*/