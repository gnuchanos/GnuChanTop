#include "top_gpu.h"

#include <assert.h>
#include <string.h>

int main(void) {
    char driver[32];
    top_gpu_driver_from_link("../../bus/pci/drivers/nouveau", driver,
                             sizeof(driver));
    assert(strcmp(driver, "nouveau") == 0);
    top_gpu_driver_from_link("/sys/bus/pci/drivers/nvidia", driver,
                             sizeof(driver));
    assert(strcmp(driver, "nvidia") == 0);
    top_gpu_driver_from_link("", driver, sizeof(driver));
    assert(strcmp(driver, "unknown") == 0);

    assert(strcmp(top_gpu_status_label(TOP_GPU_STATUS_WAITING), " WAIT") == 0);
    assert(strcmp(top_gpu_status_label(TOP_GPU_STATUS_READY), " READY") == 0);
    assert(strcmp(top_gpu_status_label(TOP_GPU_STATUS_UNAVAILABLE), "  N/A") == 0);
    return 0;
}
