# NativeMetal swap wait (patch 0013)

On the phone, CoreAnimation in backboardd calls `-[NBCommandBuffer commitAndWaitUntilSubmitted]` right before it hands the drawable IOSurface to the display (IOMFB swap). The previous implementation returned once the command buffer was scheduled (submitted to the GPU), not completed, and the display had no writer lease to consult. The working theory, from source and boot-log reading and not measured on the device, is that the display then copies a half-rendered or stale surface, which shows as flicker while scrolling (reported by the user in queue mode, `fuxigpuncimport=1536`). With patch 0013, a committed command buffer that wrote into an IOSurface (`encodedWork.storedSurface`) also waits for GPU completion before `commitAndWaitUntilSubmitted` returns.

## Switches

Swap wait is enabled by default. It can be turned off via:
- Environment variable: `FUXI_NATIVE_SWAP_WAIT=0`
- Kernel boot argument: `fuxiswapwait=0`
When set, `FUXI_NATIVE_SWAP_WAIT` takes precedence over the boot argument.

## On-device diagnostics

On device, wait statistics are printed to stderr on the first wait and every 600 occurrences:
`FUXI MTL: native swap wait n=... avg_us=... max_us=... refused=...`

## Known cost and test limitations

The render thread blocks for approximately one GPU job per frame during the swap wait.
Host tests exercise token parsing, environment gating, and the no-op path (`fuxiNoop`) only; the actual GPU wait (`nmb_command_buffer_wait`) is only exercised on the device.
This change is not yet verified on device.
