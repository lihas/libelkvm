#pragma once

struct elkvm_memory_region {
	void *host_base_p;
	uint64_t guest_virtual;
	uint64_t region_size;
	int grows_downward;
};

/*
 * There will be 8 memory regions in the system_chunk:
 * 1. text
 * 2. data
 * 3. bss (growing upward)
 * 4. stack (growing downward)
 * 5. env, which will hold the environment strings
 * 6. idth, which will hold the binaries the idt will point to
 * 7. idt, which will hold the interrupt descriptor table
 * 8. pt, which will hold the page tables
 */
#define MEMORY_REGION_TEXT  0
#define MEMORY_REGION_DATA  1
#define MEMORY_REGION_BSS   2
#define MEMORY_REGION_STACK 3
#define MEMORY_REGION_ENV   4
#define MEMORY_REGION_IDTH  5
#define MEMORY_REGION_IDT   6
#define MEMORY_REGION_PTS   7

#define MEMORY_REGION_COUNT 8