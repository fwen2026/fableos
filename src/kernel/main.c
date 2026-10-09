#include <stddef.h>
#include <kernel/console.h>
#include <io/serial.h>
#include <memory/pmm.h>
#include <limine.h>
#include <util/hcf.h>

// Limine setup

__attribute__((used, section(".limine_requests")))
static volatile uint64_t limine_base_revision[] = LIMINE_BASE_REVISION(6);

__attribute__((used, section(".limine_requests")))
static volatile struct limine_framebuffer_request framebuffer_request = {
    .id = LIMINE_FRAMEBUFFER_REQUEST_ID,
    .revision = 0
};

__attribute__((used, section(".limine_requests_start")))
static volatile uint64_t limine_requests_start_marker[] = LIMINE_REQUESTS_START_MARKER;

__attribute__((used, section(".limine_requests_end")))
static volatile uint64_t limine_requests_end_marker[] = LIMINE_REQUESTS_END_MARKER;

void kmain(void){
    if(LIMINE_BASE_REVISION_SUPPORTED(limine_base_revision)){
        serial_init();
        log_info("Serial initialized.");
        log_memory_map();

        console_printf("Hello from FableOS! \n");
    }

    hcf();
}