# GnuChanTop

GnuChanTop is a system monitor in the shape of btop: a CPU panel on the left of
the top bar, a GPU panel on the right, and a process list below that the arrow
keys walk and `k` kills from. It reads `/proc` and sysfs directly and draws to
the terminal. It is written in C, uses libdrm for legacy Radeon utilization, and
is Debian-only, on purpose.

Every GPU the machine has is listed, one to a line by name: an AMD APU, an Intel
iGPU, a discrete card that loaded as `card1`, all of them, not only `card0`.
The older AMD `radeon` driver exposes its GRBM GUI-active status through DRM;
GnuChanTop samples it at low frequency and averages the latest second to produce
a utilization percentage without blocking screen updates. The newer `amdgpu`
driver uses its sysfs counter. The installer adds the installing user to the
`render` group if needed; log out and back in after installation for that group
change to take effect.
Cards whose driver exposes no generic busy value (such as `xe` or proprietary
NVIDIA) remain listed and show `--` instead of a made-up percentage. The rolling
graph follows the first card a reading is possible on.

## Building

    git clone <repository-url> GnuChanTop
    cd GnuChanTop/src
    python3 makefile.py

The makefile installs the build dependency with apt-get, compiles the sources,
and re-runs itself through sudo to write into `/usr/local/bin`. The binary goes
to `/usr/local/bin/GnuChanTop`; the settings script goes under
`~/.config/GnuChanTop/`.

The install also grants the binary `CAP_PERFMON` with `setcap` (from
`libcap2-bin`, installed when missing) so older i915 perf counters can be read
without running as root, and lowers `kernel.perf_event_paranoid` to 2 through
`/etc/sysctl.d/99-gnuchantop.conf`.

The makefile takes an action as its first argument: `install` (the default),
`build`, `run` and `uninstall`.

## Dependencies

- `build-essential` (gcc)
- `libdrm-dev` and `pkgconf` — for Radeon register queries.

At install time:

- `libcap2-bin` — for `setcap`, used to grant `CAP_PERFMON`.

## Terminal

This is a terminal program: it needs no X server and no Wayland compositor, only
a terminal. It can run over ssh.

## License

GPL3.
