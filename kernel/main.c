#include <stddef.h>
#include <kernel/console.h>
#include <io/serial.h>
#include <limine.h>

// Limine setup

__attribute__((used, section(".limine_requests")))
static volatile uint64_t limine_base_revision[] = LIMINE_BASE_REVISION(6);

__attribute__((used, section(".limine_requests")))
static volatile struct limine_framebuffer_request framebuffer_request = {
    .id = LIMINE_FRAMEBUFFER_REQUEST_ID,
    .revision = 0
};

__attribute__((used, section(".limine_requests"))) // places memmap_request under .limine_requests in the o file
static volatile struct limine_memmap_request memmap_request = {
    .id = LIMINE_MEMMAP_REQUEST_ID,
    .revision = 0
};

__attribute__((used, section(".limine_requests_start")))
static volatile uint64_t limine_requests_start_marker[] = LIMINE_REQUESTS_START_MARKER;

__attribute__((used, section(".limine_requests_end")))
static volatile uint64_t limine_requests_end_marker[] = LIMINE_REQUESTS_END_MARKER;


/** Halt and Catch Fire */
void hcf(void){
    for(;;){
        __asm__ volatile ("hlt");
    }   
}


/** Memory map logging */
void log_memory_map(void) {
    if (memmap_request.response != NULL) {
        console_print("Memory map request received.\n");

        struct limine_memmap_response *response = memmap_request.response;

        for(int i = 0; i < response->entry_count; i++) {
            struct limine_memmap_entry *entry = response->entries[i];
            console_print("Memory map entry %d: base = 0x%lx, length = 0x%lx\n",
                           i, entry->base, entry->length);
        }
    }
}


void kmain(void){
    if(LIMINE_BASE_REVISION_SUPPORTED(limine_base_revision)){
        serial_init();
        log_memory_map();

        console_print("Hello from FableOS! \n");
    }

    hcf();
}