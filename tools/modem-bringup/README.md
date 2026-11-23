# MT6893 (Infinix X6710) userspace MD1 gate

This directory contains the userspace half of the ported CCCI modem stack.
It is the **ready gate**: without the backend connected to
`/dev/ccci-service-10209000.modem-platform`, `md_boot prepare` fails with
`-EBUSY`, `md_boot release` returns `-EAGAIN`, and the MD CPU is never started.

The tools are the reference op6893 FS/RPC runner with board-only string changes
(marker and lock names). Protocol code is unchanged and passes the reference
unit tests (`test_modem_service_backend`, `test_modem_fs_backend`,
`test_modem_fs_wire`, `test_modem_rpc_backend`, `test_modem_at_mux`).

## Modules

| File | Role |
| --- | --- |
| `modem_service_backend.py` | Endpoint record loop (FS + optional user RPC). Root-only, exclusive. |
| `modem_fs_wire.py`, `modem_fs_backend.py` | Vendor FS packet codec and marked private working copy. |
| `modem_rpc_backend.py` | User RPC `0x400f` / `0x4010` codec; needs an explicit audited policy. |
| `modem_at_probe.py`, `modem_at_mux.py`, `modem_slot_probe.py` | AT / 0710 MUX / SIM slot probes. |
| `modem_data_session.py`, `modem_ipv6_probe.py` | PDP activation and `ccmnidp0` addressing. |

The kernel-side board facts provider for this device is
`drivers/misc/mediatek/ccci/ccci-rpc-x6710.c` (transcribed from the stock
`device.dts`, not the op6893 values).

## Install

```
./install.sh                     # on the device (or use --root <staging-dir>)
```

This installs:

- `/usr/local/lib/mt6893-modem/*.py`
- `/usr/local/sbin/mt6893-modem-fs-runner`
- `/usr/local/sbin/mt6893-modem-bringup`

## First boot (all steps explicit; nothing automatic)

```
mt6893-modem-bringup status                 # inspect every state first
mt6893-modem-bringup up 1                   # FS copy + runner + md_power on + prepare
mt6893-modem-bringup status                 # expect md_boot=prepared, dispatch=prepared
mt6893-modem-bringup release                # issues MD_KERNEL_BOOT_UP SMC
mt6893-modem-bringup status                 # watch handshake HS1/HS2, ccism, dispatch
```

To stop: `mt6893-modem-bringup down`.

`up` defaults the generation to the current epoch if omitted. The generation
is saved under `/run/mt6893-modem/generation` and reused by `release`. Each new
MD session needs a **new, larger** generation and a new backend fd; `down`
followed by `up` handles that.

### FS working copy

`up` creates `/tmp/mt6893-fs` and marks it with `.mt6893-fs-working-copy`. It is
a disposable private copy — never a real NVRAM partition or backup. `fs-init`
is idempotent and refuses an unmarked existing directory. The runner will not
read or write anything outside this root.

### User RPC

No policy is installed by default. If MD requests property/SAR RPC (`0x400f` /
`0x4010`) the runner fails closed and closes the endpoint — that is by design,
not a silent empty answer. Enable opt-in tracing to see whether it is needed:

```
touch /run/mt6893-modem/trace-rpc
mt6893-modem-bringup down && mt6893-modem-bringup up 2
```

If requests appear, author a private audited policy at
`/run/mt6893-modem/rpc-policy.json` (mode `0600`, owned by root, single link,
max 64 KiB):

```json
{
  "version": 1,
  "properties": { "ro.build.type": "userdebug" },
  "sar": {
    "product_hw": "X6710",
    "regionmark": "CE",
    "projects": { "X6710": { "CE": 0 } }
  }
}
```

Every fact must be audited for this device; unreviewed keys fail closed.

### After HS2

```
mt6893-modem-bringup at  --help
mt6893-modem-bringup slot --help
mt6893-modem-bringup data --help
```

`data` activates CID 1 and configures `ccmnidp0` with a policy route so it does
not take over the USB management path. Check with `ip addr show ccmnidp0`.

## Limitations

- Reaching HS2 / CCISM RUNNING does not mean registration, calls or data work.
- NVRAM writes do not persist across AP reboots; only the marked private copy is
  used.
- No Android CCCI ioctl compatibility layer, ModemManager integration or phone
  service daemon.
