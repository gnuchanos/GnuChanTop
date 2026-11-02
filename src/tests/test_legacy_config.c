#include "top_config.h"

#include <assert.h>

int main(void) {
    TopConfig config;
    assert(top_config_load(&config) == 0);
    assert(config.update_time == 0.75);
    assert(config.proc_limit == 37);
    assert(config.show_gpu == 0);
    assert(config.sort == TOP_SORT_NAME);
    return 0;
}
