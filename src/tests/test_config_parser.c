#include "top_config.h"

#include <assert.h>

int main(void) {
    TopConfig config;
    top_config_defaults(&config);
    assert(config.update_time == 1.0);
    assert(config.proc_limit == 200);
    assert(config.show_gpu == 1);
    assert(config.sort == TOP_SORT_CPU);

    assert(top_config_load(&config) == 0);
    assert(config.update_time == 0.25);
    assert(config.proc_limit == 23);
    assert(config.show_gpu == 0);
    assert(config.sort == TOP_SORT_NAME);
    return 0;
}
