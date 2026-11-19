# Fork notes: stability fixes for eie-pro-linux

This fork of [MatTaylorSharrow/eie-pro-linux](https://github.com/MatTaylorSharrow/eie-pro-linux)
(base commit `efdd04e`, 2025-04-18) carries a series of small, separate fixes for
the Akai EIE Pro kernel driver. The original README (protocol notes) is
unchanged except for one typo fix (see patch 0016, documentation only).

The driver is still **experimental**. Everything below was tested by one person
on one machine; read the "What was tested" section before relying on it.

All code is GPL-2.0 (see `LICENSE`), original author Michal Rydlo; the changes
in this fork are by Ml Dean (https://mldean.bandcamp.com).

## What is in `master`

`master` is the base plus patches 0001-0016 applied in order (they are
incremental: each applies on the result of the previous one).

| # | Subject (short) | What it addresses |
|---|---|---|
| 0001 | check `init_urbs()` result | `probe` continued after a failed URB setup |
| 0002 | `usb_free_coherent` with allocation size | wrong size passed when freeing; `URB_NO_TRANSFER_DMA_MAP` on bulk |
| 0003 | `reset_eie` timeout | unbounded wait; `-ETIMEDOUT` / `-ENODEV` |
| 0004 | `reset_eie` rate and disconnect | `-ENODEV` after disconnect; `rate = 0` on error paths |
| 0005 | `eie->mutex` | serialise prepare / disconnect / probe error path |
| 0006 | stream state under lock | open/close/trigger/prepare vs. URB callbacks |
| 0007 | `.sync_stop` and callback counters | wait for running callbacks before freeing |
| 0008 | capture completion under lock | bounds checks in `cap_urb_complete` |
| 0009 | disconnect order | tear-down order in `disconnect` / `probe` error path |
| 0010 | start capture | capture URBs were never submitted |
| 0011 | guard rate | `-EBUSY` when the other direction runs at another rate |
| 0012 | ratelimit the refusal message | log spam |
| 0013 | capture URBs armed by prepare | URBs only circulate between prepare and close |
| 0014 | `reset_eie` capture conditional | follow-up to 0013 |
| 0015 | do not call `snd_card_set_dev()` | refcount imbalance on newer kernels, see below |
| 0016 | README typo | 96 kHz byte sequence (documentation only) |

Exact one-line rationale for each patch is in its commit message.

### Branch `diag-urb-status`

One optional patch on top of `master`: it logs a rate-limited warning
(`<type> urb error <status>`) when a URB completes with a status other than the
ordinary kill/unlink codes. It is useful for debugging (e.g. it prints
`Capture urb error -71` when the cable is pulled) but is not needed for normal
use, so it is kept out of `master`.

## The kernel-version issue fixed by 0015

The upstream `probe()` calls `snd_card_set_dev(card, &interface->dev)` after
`snd_card_new(&usb_device->dev, ...)`.

* In mainline v6.12, v7.0 and v7.1-rc1, `snd_card_init()` stores the parent
  as `card->dev = parent;` (no reference taken).
* In current mainline `master`, it stores `card->dev = get_device(parent);` and
  `snd_card_do_free()` calls `put_device(card->dev);`. I did not identify
  which commit introduced this.
* Ubuntu's 7.0.0-43 kernel behaves like the second case (seen in the
  disassembly of its `snd.ko`; I do not have its source).

With the `snd_card_set_dev()` override the `get` is done on the `usb_device`
and the `put` on the interface, which gave a `refcount_t: underflow` warning
when the device was unplugged on 7.0.0-43. Dropping the override (as
`sound/usb/misc/ua101.c` does) keeps the references balanced.

## What was tested

Hardware: one Akai EIE Pro (`09e8:0010`, `bcdDevice 2.00`), x86_64 laptop with
an AMD xHCI controller, Ubuntu with kernel 7.0.0-38 and 7.0.0-43.

* Playback at 44.1 / 48 / 88.2 / 96 kHz, repeated open and close (about 300
  cycles, playback only, on an early build of the series), SIGINT / SIGKILL of players, rate changes; capture and full duplex
  at the rates used in the runs below.
* Duplex runs of 61 min (no gaps heard, no xruns in `pw-top`) and 30 min
  (sample-exact check, no gaps); several hours of recording with a DAW
  through PipeWire.
* Cable pulled while playing and/or capturing (four scenarios on 7.0.0-43 after
  patch 0015: capture + play, capture only, play only, nothing open): no
  warnings in the kernel log.

Caveats, so nothing here is oversold:

* The 30-minute duplex run, the multi-hour DAW sessions and the unplug tests
  ran with the diagnostic patch (`diag-urb-status`) applied. The 60-minute
  duplex run and the open/close loops ran on earlier builds of the series,
  without it and without patches 0013-0015. The `master` code without it was built (`W=1 -Wall -Wextra`: no code
  warnings; `checkpatch.pl --no-tree --file`: 0 errors, same 8 warnings as the
  base) but has **not** been run on hardware as such.
* Capture has stalled (state RUNNING, `hw_ptr` frozen or 0, nothing in the
  kernel log) three times: 2026-11-11, 2026-11-13 and 2026-11-19; each time
  a power cycle of the EIE fixed it. The cause is unknown (possibly the EIE
  or its USB socket; not shown). The 2026-11-19 case, right after the first
  plug-in following `insmod`, could not be reproduced afterwards.
* After a cold start of the EIE the log can show a couple of
  `Sync urb: device did not advance clock` messages; they have been harmless.
* MIDI is visible as a port but was not tested systematically.
* After one suspend (s2idle) the EIE did not re-enumerate on resume (USB
  errors -110/-62, module not loaded at the time); unplugging, power-cycling
  the EIE and replugging fixed it. Suspend/resume with the module loaded has
  not been tested.
* `-EBUSY` (patch 0011) is the intended behaviour when you open the second
  direction at a different rate than the first.
* One tester, one device, one laptop.

## Installation with DKMS

```
sudo cp -r dkms/eie-pro-0.1 /usr/src/eie-pro-0.1
sudo dkms add -m eie-pro -v 0.1
sudo dkms build -m eie-pro -v 0.1
sudo dkms install -m eie-pro -v 0.1
```

The module is then loaded when the device is plugged in. If you use Secure
Boot, DKMS must be able to sign the module (MOK); otherwise the module will not
load. Unplugging the cable was tested with this code base plus the diagnostic patch
(see "What was tested"); with the upstream base on kernels that take a
reference on the parent device it produced a refcount warning (see above).

To remove: `sudo dkms remove -m eie-pro -v 0.1 --all`.

## Applying the patches to upstream

```
git am patches/master/*.patch          # on a checkout of upstream efdd04e
git am patches/diag-urb-status/*.patch # optional
```

The patches apply in order on the base without conflicts (checked with
`patch -p1`). Only the final result was built; intermediate states were not
built one by one.
