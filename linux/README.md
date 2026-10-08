# flexptpd: flexPTP as a Linux daemon

`flexptpd` runs flexPTP's gPTP profile on one network interface. It uses the
kernel's hardware timestamps and the interface's PHC, or, with `-s`, software
timestamps and a virtual clock. It publishes the port's gPTP state in
`/dev/shm` for other processes.

## Build

```sh
cmake -S linux -B build && cmake --build build          # build/flexptpd, build/flexptpd-status
```

For a Buildroot aarch64 target (the PocketBeagle 2's toolchain by default; `BR_HOST` points at
another Buildroot output's `host/`):

```sh
cmake -S linux -B build-aarch64 -DCMAKE_TOOLCHAIN_FILE=linux/cmake/aarch64-buildroot.cmake [-DBR_HOST=...]
cmake --build build-aarch64
```

## Options

```
flexptpd -i IF [-c CONFIG] [-s] [-O NS] [-D PPB] [-P PRIO] [-m NAME] [-q] [-v]
```

| Option | Meaning |
|---|---|
| `-i IF` | the PTP interface (required) |
| `-c CONFIG` | a file of CLI commands, one per line; `#` starts a comment. It is applied once the core has initialized and the gPTP preset is loaded, so it changes the preset (e.g. `ptp priority 100 100`) |
| `-s` | software timestamps and a virtual clock, for interfaces without a PHC (veth pairs in tests) |
| `-O NS` | with `-s`: the virtual clock's initial offset from `CLOCK_REALTIME` (0) |
| `-D PPB` | with `-s`: the virtual clock's free-running frequency error (0) |
| `-P PRIO` | `SCHED_FIFO` priority for all of the daemon's threads (none) |
| `-m NAME` | the status block's name in `/dev/shm` (`flexptpd.IF`); `-m ""` publishes none |
| `-q` | no interactive CLI, even on a terminal. Without a terminal on stdin there is never one, and stdin at end of file is not read |
| `-v` | with `-s`: print `vclock-error-ns N` (virtual clock minus `CLOCK_REALTIME`) once a second |

`SIGINT` or `SIGTERM` stops the daemon within a second, exit status 0. The logs go to stdout, line-buffered,
with colours only when stdout is a terminal.

`-P` sets the policy of the main thread before any other starts, so every thread inherits it. The
transceiver and PTP task threads are also set explicitly. Without permission (`CAP_SYS_NICE`, or an
`RLIMIT_RTPRIO`) the daemon says so and runs at the default policy.

On a terminal, the CLI takes flexPTP's commands (`?` lists them) and `exit`.

## The status block

`/dev/shm/NAME` holds a `FlexptpdShm` (`flexptpd_shm.h`):

| Offset | Field | Content |
|---|---|---|
| 0 | `char magic[8]` | `"FLXPTPD\0"` |
| 8 | `uint32_t layout` | `PTP_GPTP_STATUS_VERSION` of the status below |
| 12 | `uint32_t seq` | seqlock: odd while the status is being written |
| 16 | `uint32_t size` | `sizeof(PtpGptpStatus)` |
| 20 | `uint32_t _r` | 0 |
| 24 | `PtpGptpStatus status` | `src/flexptp/gptp_status.h`, in native byte order and C alignment |

A reader reads `seq`, copies the status, reads `seq` again, and keeps the copy if both reads are equal
and even (`flexptpd_shm_read()` does this). `flexptpd-status [NAME]` prints the block as `key value`
lines.

The daemon polls the core's snapshot every 20 ms and rewrites the block whenever
`PtpGptpStatus.updates` changes (the core refreshes the snapshot on every heartbeat, about 32 times a
second). It removes the block on a clean exit. A reader still holding a mapping of a removed block sees
`updates` stop moving.

## The Linux network stack driver

`src/flexptp/port/example_netstack_drivers/nsd_linux.c`:

* **Transmit timestamps** are matched to their messages by content: messageType, domainNumber and
  sequenceId, plus the requestingPortIdentity for a Pdelay_Resp. The key is read from the frame the
  kernel echoes on the error queue (`nsd_linux_txts.c`), so the order they arrive in does not matter.
  * A timestamp that has not come back within 20 ms (`linux_nsd_set_txts_timeout_ms()`) is reported
    lost (`ptp_transmit_timestamp_lost()`), and the core releases the message.
  * So is an event message whose `sendto()` fails, e.g. with the interface down.
  * A timestamp that arrives after its timeout is dropped.
  * The task loop's wait after a transmission is bounded too, at three times that timeout, as a
    safety net.
* **The link state** (`IFF_UP` and `IFF_RUNNING`) comes from netlink and goes to the core
  (`ptp_nsd_link_state_changed()`). It is reported at every (re)initialization, then on every change.
* **Sockets over 802.3:**
  * One packet socket, bound to the interface, receives every PTP frame and sends the event messages.
    It has joined both destination groups of the profile.
  * The socket that sends the general messages receives nothing.
  * The destinations are the profile's (`01:80:C2:00:00:0E` for gPTP).
  * No IPv4 address is needed.
* **Timers** (heartbeat, auxiliary) run on `CLOCK_MONOTONIC`.
* **The software-timestamp mode** (`linux_nsd_set_sw_mode()`):
  * The kernel's software timestamps (`CLOCK_REALTIME`) are converted to a virtual clock,
    `v = base + (rt - t0) * (1 + (drift + tuning) / 1e9)`, before they reach the core.
  * `linux_set_time()`, `linux_adjust_clock()` and `linux_get_time()` act on that clock.
  * A frequency change rebases it, so its time stays continuous.

## Tests

```sh
make -C tests/linux check      # both of the below
make -C tests/linux unit       # the transmit timestamp matcher: out of order, duplicate, late, lost, full
make -C tests/linux netns      # two daemons over veth in unshare -rn (no root needed)
```

The netns test starts a master (`ptp priority 100 100`) and a slave with its virtual clock 50 ms ahead
and drifting 20 ppm. It checks:

* the roles;
* the slave's convergence to within 100 us;
* running without an IPv4 address;
* idle CPU with stdin at end of file;
* a 2 s drop of the slave's own link, where its sends fail;
* a 3 s carrier loss, where the kernel drops its frames and the timestamps are reported lost by the
  timeout;
* the recovery to a settled SLAVE;
* a clean exit on SIGTERM.

The `SCHED_FIFO` check runs only where permitted; it is skipped in an unprivileged namespace.
