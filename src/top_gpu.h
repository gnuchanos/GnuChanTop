/*
 * top_gpu.h — how busy each GPU is, when its driver says.
 *
 * There is no one file for this, and no one GPU either. A machine can have an
 * integrated GPU and a discrete one, and the kernel hands them out as separate
 * DRM cards — card0, card1 — in an order that depends on which driver loaded
 * first, not on which is the one a person cares about. Reading only card0, as
 * this module once did, is why an AMD APU beside a discrete card, or an Intel
 * iGPU whose driver loaded second, showed up as "no reading".
 *
 * So every card is examined and every one that is a real GPU is kept. What can
 * be read from a card depends on its driver:
 *
 *   amdgpu    /sys/class/drm/cardN/device/gpu_busy_percent is one integer 0..100.
 *   radeon    GRBM GUI-active status, averaged from low-rate DRM register reads.
 *   i915      per-engine millisecond counters under the card's gt directory,
 *             differenced between two reads, or the i915 perf event on older
 *             parts. See top_gpu.c.
 *   xe        the newest Intel driver; listed, no generic busy file here.
 *   nvidia    the proprietary driver has no generic busy counter; listed,
 *             no value.
 *
 * A card whose value cannot be read is still shown — with its name — because
 * seeing that a GPU exists, and whether it reads, is the point of the panel. A
 * machine with no GPU at all finds `count` zero and the drawing says so.
 */
#ifndef GNUCHANTOP_GPU_H
#define GNUCHANTOP_GPU_H

#include "top_common.h"

/* The most GPUs the panel lists. A machine with more — a render node's worth of
   accelerators — shows the first this many; a desktop never reaches it. */
#define TOP_MAX_GPUS 6

/* One GPU: what it is, and how busy it is, which may be "unknown". */
typedef struct {
    char   card[16];        /* the DRM node, "card0" */
    char   name[TOP_NAME];  /* a label for the panel, "AMD GPU (amdgpu)" */
    char   driver[32];      /* the kernel driver, for the label and for tests */
    int    present;         /* 1 when a busy reading is possible on this GPU */
    int    has_reading;     /* 1 once two samples have made a percentage */
    double usage;           /* 0..100, meaningful when has_reading */
} TopGpuDevice;

/* Every GPU the machine has. */
typedef struct {
    int          count;                 /* GPUs found, 0 when there are none */
    int          primary;               /* the one the graph follows, or -1 */
    TopGpuDevice devices[TOP_MAX_GPUS];
} TopGpu;

/* Find the GPUs once and sample them. Returns 0 when the machine has at least
   one GPU (whether or not a value could be read yet), -1 when it has none. */
int top_gpu_sample(TopGpu *gpu);

/* The GPU the history graph follows: the first one a reading is possible on, or
   NULL when no GPU has one. */
const TopGpuDevice *top_gpu_primary(const TopGpu *gpu);

#endif /* GNUCHANTOP_GPU_H */
