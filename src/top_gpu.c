/*
 * top_gpu.c — reading each GPU's busy-ness from whichever driver is there.
 *
 * The module walks EVERY DRM card, not card0. A machine can have an integrated
 * GPU and a discrete one as separate cards (card0, card1), and which is card0
 * depends on driver load order, so reading only card0 is how an AMD APU beside
 * an Intel iGPU — or any GPU whose driver loaded second — showed up as "no
 * reading on this machine". Every card that is a GPU is now listed.
 *
 * What can be read from a card depends on its driver:
 *
 *   1. amdgpu. /sys/class/drm/cardN/device/gpu_busy_percent is one integer
 *      0..100, already the answer. This is tried first for ANY card, because a
 *      driver that offers the file is the easy case whoever made it.
 *
 *   2. i915, newer parts. Per-engine millisecond counters under the card's gt
 *      directory, /sys/class/drm/cardN/gt/gtX/engines/<engine>/busy. The busiest
 *      engine's growth over the interval, against the wall-clock milliseconds
 *      that passed, is the percentage. The engine list differs between chips, so
 *      the files are discovered by reading the directory, not by name.
 *
 *   3. i915, older parts. The driver publishes a perf PMU whose rcs0-busy event
 *      counts the render engine's busy nanoseconds; differenced over the
 *      interval. Needs the CAP_PERFMON the installer grants and the
 *      perf_event_paranoid the installer lowers.
 *
 *   4. Everything else. radeon (the older AMD driver), xe (the newest Intel),
 *      and the proprietary NVIDIA driver expose no busy value in sysfs at all.
 *      The card is STILL LISTED, with its name and `n/a`, because it is a GPU
 *      the machine has and the panel's job is to say so — only the value is
 *      missing, and hiding the card would report a machine that has no GPU.
 *
 * A card whose value cannot be read leaves its `present` at 0; one where a
 * reading is possible but two samples have not yet been taken leaves
 * `has_reading` at 0. Neither is an error.
 */
#include "top_gpu.h"

#include <dirent.h>
#include <linux/perf_event.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

/* The DRM class directory, where every card is a `cardN` entry. */
#define DRM_DIR "/sys/class/drm"

/* The most engine counters one card's i915 path tracks. */
#define GPU_MAX_ENGINES 16

/* --- reading helpers ------------------------------------------------------- */

/* Read a single decimal whole number from a file. Returns 0 and sets *out when
   it read one, -1 otherwise. */
static int read_number(const char *path, long *out) {
    FILE *file = fopen(path, "r");
    if (!file) {
        return -1;
    }
    long value = -1;
    int matched = fscanf(file, "%ld", &value);
    fclose(file);
    if (matched != 1) {
        return -1;
    }
    *out = value;
    return 0;
}

/* Read a hexadecimal whole number from a file. The kernel writes the PCI vendor
   as "0x1002", which a plain %ld reads as 0 — the bug that made every card label
   come out "GPU 0x0". %lx reads it for what it is. */
static int read_hex(const char *path, long *out) {
    FILE *file = fopen(path, "r");
    if (!file) {
        return -1;
    }
    long value = 0;
    int matched = fscanf(file, "%lx", &value);
    fclose(file);
    if (matched != 1) {
        return -1;
    }
    *out = value;
    return 0;
}

/* The kernel driver bound to a card, from the `driver` symlink beside the
   device. Returns "unknown" when the link is not there. */
static void read_driver_name(const char *card, char *out, size_t size) {
    snprintf(out, size, "unknown");
    char path[TOP_TEXT];
    snprintf(path, sizeof(path), DRM_DIR "/%.16s/device/driver", card);
    char target[TOP_TEXT];
    ssize_t length = readlink(path, target, sizeof(target) - 1);
    if (length <= 0) {
        return;
    }
    target[length] = '\0';
    const char *slash = strrchr(target, '/');
    if (slash && slash[1]) {
        snprintf(out, size, "%.31s", slash + 1);
    }
}

/* A card's label for the panel: the vendor and the driver. */
static void make_label(char *out, size_t size, long vendor, const char *driver) {
    const char *vendor_name = "GPU";
    if (vendor == 0x1002) {
        vendor_name = "AMD GPU";
    } else if (vendor == 0x8086) {
        vendor_name = "Intel GPU";
    } else if (vendor == 0x10de) {
        vendor_name = "NVIDIA GPU";
    }
    snprintf(out, size, "%s (%s)", vendor_name, driver);
}

/* --- per-card reader state ------------------------------------------------- */

typedef struct {
    char path[TOP_TEXT];  /* room for the card, tile, engine and `busy` */
    long last_busy;       /* milliseconds reported by the last read */
    int  seen;            /* 1 once a first read has been taken */
} EngineCounter;

/* Everything kept between samples for one card. Indexed in step with the
   device list, which is discovered once and never reordered, so the reader and
   the card it belongs to stay paired. */
typedef struct {
    int           engines_discovered;
    int           engine_count;
    EngineCounter engines[GPU_MAX_ENGINES];
    long long     engine_last_wall_ms;  /* the millisecond clock of the last
                                           engine read, for the interval */
    int           perf_tried;
    int           perf_fd;
    long long     perf_last_busy_ns;
    long long     perf_last_wall_ns;
    int           perf_seen;
} GpuReader;

static GpuReader readers[TOP_MAX_GPUS];

/* --- i915 engine counters -------------------------------------------------- */

/* Discover the engine busy files under a card's gt directory. */
static void discover_engines(const char *card, GpuReader *reader) {
    reader->engine_count = 0;
    char gt_dir[TOP_TEXT];
    snprintf(gt_dir, sizeof(gt_dir), DRM_DIR "/%.16s/gt", card);
    DIR *gt = opendir(gt_dir);
    if (!gt) {
        return;
    }
    struct dirent *tile;
    while ((tile = readdir(gt)) != NULL) {
        if (strncmp(tile->d_name, "gt", 2) != 0) {
            continue;
        }
        char engine_dir[TOP_TEXT];
        snprintf(engine_dir, sizeof(engine_dir), "%s/%.16s/engines", gt_dir,
                 tile->d_name);
        DIR *engines_opened = opendir(engine_dir);
        if (!engines_opened) {
            continue;
        }
        struct dirent *entry;
        while ((entry = readdir(engines_opened)) != NULL) {
            if (entry->d_name[0] == '.') {
                continue;
            }
            if (reader->engine_count >= GPU_MAX_ENGINES) {
                break;
            }
            char busy_path[TOP_TEXT];
            snprintf(busy_path, sizeof(busy_path), "%s/%.32s/busy", engine_dir,
                     entry->d_name);
            FILE *probe = fopen(busy_path, "r");
            if (!probe) {
                continue;
            }
            fclose(probe);
            EngineCounter *counter = &reader->engines[reader->engine_count];
            snprintf(counter->path, sizeof(counter->path), "%s", busy_path);
            counter->last_busy = 0;
            counter->seen = 0;
            reader->engine_count++;
        }
        closedir(engines_opened);
    }
    closedir(gt);
}

/* The wall-clock time in milliseconds, from CLOCK_MONOTONIC so a clock change
   does not corrupt the interval. */
static long long now_millis(void) {
    struct timespec time;
    clock_gettime(CLOCK_MONOTONIC, &time);
    return (long long)time.tv_sec * 1000 + time.tv_nsec / 1000000;
}

/* The i915 engine reading: for each engine, how much its counter grew over the
   interval as a share of the wall-clock interval. The busiest engine is the
   answer. Returns 0 when any engine produced a percentage, -1 when the card has
   no engine counters. */
static int sample_engines(GpuReader *reader, TopGpuDevice *device) {
    if (!reader->engines_discovered) {
        discover_engines(device->card, reader);
        reader->engines_discovered = 1;
    }
    if (reader->engine_count == 0) {
        return -1;
    }

    long long now = now_millis();
    long long elapsed = now - reader->engine_last_wall_ms;
    if (reader->engine_last_wall_ms == 0 || elapsed <= 0) {
        elapsed = 0;
    }

    double busiest = 0.0;
    for (int i = 0; i < reader->engine_count; i++) {
        long busy = 0;
        if (read_number(reader->engines[i].path, &busy) != 0) {
            continue;
        }
        if (reader->engines[i].seen && elapsed > 0 &&
            busy >= reader->engines[i].last_busy) {
            double percent = ((double)(busy - reader->engines[i].last_busy) *
                              100.0) / (double)elapsed;
            if (percent > busiest) {
                busiest = percent;
            }
        }
        reader->engines[i].last_busy = busy;
        reader->engines[i].seen = 1;
    }
    reader->engine_last_wall_ms = now;

    if (busiest > 100.0) {
        busiest = 100.0;
    }
    device->usage = busiest;
    device->has_reading = 1;
    device->present = 1;
    return 0;
}

/* --- i915 perf event ------------------------------------------------------- */

static long long now_nanos(void) {
    struct timespec time;
    clock_gettime(CLOCK_MONOTONIC, &time);
    return (long long)time.tv_sec * 1000000000LL + time.tv_nsec;
}

/* Open the i915 PMU's busy event once. Returns 0 when the event is open, -1
   when this machine has no such PMU or the kernel refused the open — the last
   is the CAP_PERFMON case, which the installer fixes with setcap. */
static int open_perf(GpuReader *reader) {
    char path[TOP_TEXT];
    snprintf(path, sizeof(path), "/sys/bus/event_source/devices/i915/type");
    long type = 0;
    if (read_number(path, &type) != 0) {
        return -1;
    }
    struct perf_event_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.type = (unsigned int)type;
    attr.size = sizeof(attr);
    attr.config = 0x0;   /* rcs0-busy, busy nanoseconds of the render engine */
    attr.disabled = 1;
    long fd = syscall(__NR_perf_event_open, &attr, -1, 0, -1, 0);
    if (fd < 0) {
        return -1;
    }
    ioctl((int)fd, PERF_EVENT_IOC_RESET, 0);
    ioctl((int)fd, PERF_EVENT_IOC_ENABLE, 0);
    reader->perf_fd = (int)fd;
    return 0;
}

/* The perf reading: the busy nanoseconds the render engine gained over the
   wall-clock nanoseconds that passed. Returns 0 when a percentage was produced,
   -1 when this card offers no perf event. */
static int sample_perf(GpuReader *reader, TopGpuDevice *device) {
    if (!reader->perf_tried) {
        reader->perf_tried = 1;
        open_perf(reader);
    }
    if (reader->perf_fd < 0) {
        return -1;
    }

    long long busy = 0;
    if (read(reader->perf_fd, &busy, sizeof(busy)) != (ssize_t)sizeof(busy)) {
        return -1;
    }
    long long wall = now_nanos();

    if (reader->perf_seen && wall > reader->perf_last_wall_ns &&
        busy >= reader->perf_last_busy_ns) {
        long long busy_delta = busy - reader->perf_last_busy_ns;
        long long wall_delta = wall - reader->perf_last_wall_ns;
        double percent = (double)busy_delta * 100.0 / (double)wall_delta;
        if (percent < 0.0)   percent = 0.0;
        if (percent > 100.0) percent = 100.0;
        device->usage = percent;
        device->has_reading = 1;
    }
    reader->perf_last_busy_ns = busy;
    reader->perf_last_wall_ns = wall;
    reader->perf_seen = 1;
    device->present = 1;
    return 0;
}

/* --- one card -------------------------------------------------------------- */

/* Whether a directory entry name is a DRM card: "card" then digits only. This
   keeps the connectors — card0-DP-1, card0-HDMI-A-1, which are also entries of
   the class directory — from being mistaken for cards. */
static int is_card_name(const char *name) {
    if (strncmp(name, "card", 4) != 0) {
        return 0;
    }
    if (name[4] == '\0') {
        return 0;
    }
    for (const char *cursor = name + 4; *cursor; cursor++) {
        if (*cursor < '0' || *cursor > '9') {
            return 0;
        }
    }
    return 1;
}

/* The simple path: amdgpu's own percentage. Returns 0 when the file was there
   and read, -1 otherwise. */
static int sample_percent(const char *card, TopGpuDevice *device) {
    char path[TOP_TEXT];
    snprintf(path, sizeof(path), DRM_DIR "/%.16s/device/gpu_busy_percent", card);
    long percent = 0;
    if (read_number(path, &percent) != 0) {
        return -1;
    }
    if (percent < 0)   percent = 0;
    if (percent > 100) percent = 100;
    device->usage = (double)percent;
    device->has_reading = 1;
    device->present = 1;
    return 0;
}

/* Take one card's reading, whichever way its driver allows. */
static void sample_device(TopGpuDevice *device, GpuReader *reader) {
    if (sample_percent(device->card, device) == 0) {
        return;
    }
    if (sample_engines(reader, device) == 0) {
        return;
    }
    if (sample_perf(reader, device) == 0) {
        return;
    }
    /* No path read a value: the card is listed, present stays as discovery set
       it (0), and the panel shows n/a beside its name. */
}

/* --- discovery ------------------------------------------------------------- */

/* Fill the device list from the DRM class directory. Each card is named by its
   vendor and driver; `present` is set later, by whether a reading can be taken.
   Returns the number of cards found. */
static int discover_devices(TopGpu *gpu) {
    memset(gpu, 0, sizeof(*gpu));
    gpu->primary = -1;

    DIR *drm = opendir(DRM_DIR);
    if (!drm) {
        return 0;
    }
    struct dirent *entry;
    while ((entry = readdir(drm)) != NULL && gpu->count < TOP_MAX_GPUS) {
        if (!is_card_name(entry->d_name)) {
            continue;
        }
        TopGpuDevice *device = &gpu->devices[gpu->count];
        snprintf(device->card, sizeof(device->card), "%.15s", entry->d_name);

        char path[TOP_TEXT];
        snprintf(path, sizeof(path), DRM_DIR "/%.16s/device/vendor",
                 entry->d_name);
        long vendor = 0;
        read_hex(path, &vendor);   /* a card with no vendor file is still listed */

        read_driver_name(entry->d_name, device->driver, sizeof(device->driver));
        make_label(device->name, sizeof(device->name), vendor, device->driver);
        device->present = 0;
        device->has_reading = 0;
        device->usage = 0.0;
        gpu->count++;
    }
    closedir(drm);
    return gpu->count;
}

/* The GPU the graph follows: the first a reading is possible on. -1 when none
   can be read (the graph stays flat and every card is still listed). */
static int choose_primary(const TopGpu *gpu) {
    for (int i = 0; i < gpu->count; i++) {
        if (gpu->devices[i].present) {
            return i;
        }
    }
    return -1;
}

/* --- the entry point ------------------------------------------------------- */

int top_gpu_sample(TopGpu *gpu) {
    /* The card list is found once. It is stable for the life of the process —
       a card does not come and go under a running monitor — and the readers[]
       state is indexed against it, so re-discovering would desync the two. The
       flag is a file-static rather than something read from `gpu`: the caller
       zeroes the struct before the first call, so `gpu->primary` starts at 0,
       not -1, and a test on the struct cannot tell "not yet discovered" from
       "discovered, no primary". */
    static int discovered = 0;
    if (!discovered) {
        discover_devices(gpu);
        discovered = 1;
    }
    if (gpu->count == 0) {
        return -1;
    }

    for (int i = 0; i < gpu->count && i < TOP_MAX_GPUS; i++) {
        sample_device(&gpu->devices[i], &readers[i]);
    }

    /* A reading that became possible this frame — the first engine sample, or
       the perf event — moves the graph onto that card. */
    gpu->primary = choose_primary(gpu);
    return 0;
}

const TopGpuDevice *top_gpu_primary(const TopGpu *gpu) {
    if (!gpu || gpu->primary < 0 || gpu->primary >= gpu->count) {
        return NULL;
    }
    return &gpu->devices[gpu->primary];
}
