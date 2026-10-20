# pse.tasks (version 1)

Run code on the game thread from anywhere, now-ish or a few frames later. Header: `pse/pse_api.h`. API 0 = unstable
(phase 1).

```c
const PSETasksInterface* tasks = pse->Query(pse->plugin, PSE_TASKS_INTERFACE, 1);
typedef void (*PSETaskFn)(void* ctx);
```

Most PSE calls must be made on the game thread. Worker threads (network, file loading, Discord …) hand their results
over with a task.

## When tasks run

Each game frame the loader first runs the tasks that are due, in the order they were queued, then raises
`pse.frame` ([messaging](messaging.md)). A task queued while tasks are running (by a task, or by another thread at
that moment) runs in the next frame, never in the same one. Tasks queued before the first frame run in the first
frame.

External plugins' tasks run inside SEH: a task that faults disables its plugin
([plugins.md - Crash guard](plugins.md#crash-guard-callbacks-after-load)); its other tasks due in the same frame are
skipped, everyone else's still run, and `pse.frame` is still raised. Built-in plugins' tasks are called directly.

## Functions

### RunOnGameThread

```c
PSEResult RunOnGameThread(PSEPluginHandle plugin, PSETaskFn fn, void* ctx);
```

Any thread. Runs `fn(ctx)` at the start of the next game frame, on the game thread. Even when called on the game
thread it does not run at once. `ctx` must stay valid until the task has run. Errors: `PSE_E_INVALID_ARG` (bad
handle, NULL `fn`).

```c
/* network thread */
static void ApplyJoin(void* ctx) { AddPlayer((Peer*)ctx); }
tasks->RunOnGameThread(me, ApplyJoin, peer);
```

### RunAfterFrames

```c
PSEResult RunAfterFrames(PSEPluginHandle plugin, uint32_t frames, PSETaskFn fn, void* ctx);
```

Any thread. Runs after `frames` more frames: 0 = the next frame (same as `RunOnGameThread`), 1 = the frame after,
… Errors as above.

```c
tasks->RunAfterFrames(me, 30, ShowHint, NULL);   /* about half a second at 60 fps */
```

### IsGameThread

```c
int IsGameThread(void);
```

Any thread. 1 on the game thread (the thread the loader started on), else 0.

### FrameNumber

```c
uint32_t FrameNumber(void);
```

Any thread. Game frames since start: 0 before the first frame, 1 during the first frame's tasks and `pse.frame`, …
