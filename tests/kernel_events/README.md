# Kernel events

KEVENTs a title builds by writing the header itself, with no
`KeInitializeEvent` call, checked through the thunk dispatcher. Like
`tests/memory_regressions`, it needs no title and no game files: it runs the
real memory layout on the small synthetic XBE from `tools/conformance`.

```
cmake -S tests/kernel_events -B build/kernel-events -A x64
cmake --build build/kernel-events --config Release
ctest --test-dir build/kernel-events -C Release --output-on-failure
```

| Test | What it checks |
|---|---|
| `kernel_events_default` | No switch set. Pins what a title gets for an event off the guest stacks: the guest address goes to Win32 as a `HANDLE`, so after `KeSetEvent` the wait still fails. Events on a guest stack are real in this mode too. |
| `kernel_events_title_kevents` | `RECOMP_TITLE_KEVENTS=1`. The table below. |

Each check fails on v0.12.0 (every wait returns `0xC0000001`) and passes with
the switch:

| Check | Why it matters |
|---|---|
| An unset synchronization event times out | A zero timeout must report the state, not an error. |
| `KeSetEvent` sets it; the wait succeeds and clears `SignalState` | A synchronization wait consumes the signal, in guest memory too. |
| A second wait times out again | The same. |
| `SignalState` written to 1 by the title counts | XDK code sets and clears events by writing the header. |
| A notification event stays set across two waits | Notification events are not consumed. |
| `KeResetEvent` clears it | Host event and guest header agree. |

A KEVENT on a guest stack is a real event in both modes (the main stack region,
its worker slices, and a live `xbox_AllocThreadStack` slice): unset times out,
`KeSetEvent` ends the wait, a wait on one thread is released from another, and
a freed thread slice is no stack and keeps no stale shadow. The XDK builds the
KEVENT of a synchronous request, such as `XInputGetCapabilities`, on the
caller's stack.

A wait that really blocks and is released from another thread is checked on a
stack event only (the bridges use a host event for exactly that, so a zero
timeout checks the same state elsewhere). The shadow table follows the object
at a reused stack address: a hand-built event then `KeInitializeEvent`, the
reverse, and `KeSetEventBoostPriority`. Not covered: any title.

The fixture also checks `KeWaitForMultipleObjects` through the dispatcher:
first access at a nonzero WaitAny index; only the selected synchronization
event being consumed; WaitAll success and timeout; header writes clearing a
stale host signal; notification persistence; failed waits preserving guest
state; and mixed header/explicitly initialized events. Every call checks
stdcall stack cleanup. Explicit events resolve in both modes; header events
outside the guest stacks remain opt-in, and the default multiple-wait rejection
preserves their state.

The new multiple-wait checks fail against upstream main `193e2995` and pass
with the bridge fix. These zero-timeout tests do not establish correctness for
concurrent guest header writes, event reinitialization, abandoned mutexes, or
cross-title compatibility. Synthetic `test.xbe` is generated locally and is
not part of the contribution.
