# ws::jobs - CLAUDE.md

The headless job system (nextplan stage 8): a Result-shaped pool with
priorities, cooperative cancellation, coalesced progress and completions
marshalled to the owner thread. Pure std + ws::core - no third-party
anywhere, no platform header, nothing GUI-shaped.

## Purpose
- `errors.hpp`: `JobError` codes + `describe(JobError)` (the static message
  table) + `fail(JobError, msg)` (the `unexpected` form, Result paths only).
- `marshal.hpp`: `MarshalQueue` - `post()` from any thread, `drain()` one
  FIFO pass on the consumer thread (a post during a drain lands the next
  drain, never re-entrant), `has_pending()` / `pending_count()`.
- `job_system.hpp`: `JobState`, `Priority`, `JobOptions` (key/group,
  registered by the next arc), `JobCallbacks<T>`, `JobContext`,
  `JobHandle<T>`, `JobSystem` (`submit<T>`, `drain`, `thread_count`).
- `detail/record.hpp`: the record (one job's shared state) + the progress
  machinery. `JobState` and `CancelToken` are public types that live here
  because the record stores them; users reach them through
  job_system.hpp and never name `ws::jobs::detail` - it is under include/
  only because the typed `JobHandle<T>` must see `Record<T>`'s shape.
- `src/job_system.cpp`: the pool (workers, priority queues with FIFO
  within a level, the shutdown sequence).

## Allowed dependencies
- `ws::core` (Result/Status, logging) - PUBLIC. Nothing else, ever: this
  lib is the "pure std" tier the gui helpers in ws::ui build on.

## Invariants / rules
- Jobs NEVER touch the gui or any other non-thread-safe surface: a job's
  only outbound channels are its Result and `report_progress`. Everything
  else crosses through the marshal queue to the drain thread.
- Cancellation is cooperative. The callable polls
  `ctx.check_cancel()` (WS_TRY-able) or hands out `ctx.token()`.
  "Honoured" = RETURNING the cancelled error: the system maps that one
  code (jobs-category, `cancelled`) to `JobState::cancelled`. A job that
  returns success after a cancel request lands `done` - the cancel lost
  the race, and that is the documented outcome, not an error.
- A queued job cancelled before pickup never runs: the worker's pickup
  skip answers `cancelled` (completion still fires on the drain); the
  destructor's mark pass answers `cancelled` without running (no
  completion - the marshal is dying).
- Progress: clamped [0,1], the record keeps the max (the bar never runs
  backwards), the latest message wins, and delivery is coalesced - at most
  one armed post per job; the deliver loop re-reads the mailbox while
  reports keep landing during its run and clears the armed flag only when
  the seq is quiet, under the same lock that checked it (no report can
  slip between check and clear).
- Exceptions never cross: the worker wrapper catches anything and converts
  it to a failed Result (`exception` code, `what()` in the message).
- Callbacks never allocate to report errors: `on_progress` gets a
  (float, const char*) whose message pointer is valid only during the
  call; copy it out to keep it. `on_complete` gets the handle by value.
- Lock order: record mutex -> marshal queue, record -> live, live ->
  work queue, queue -> record (dtor only). Never the reverse of any of
  these pairs.
- `wait()` is for the owner thread only, never from inside a job (the
  one caller-owned deadlock; wait_all/wait_group, next arc, same rule).
- Safe shutdown (`~JobSystem`): stopping set, then cancel-all live jobs,
  then queued entries marked cancelled without running, then join. A
  submit racing teardown answers an already-terminal cancelled handle and
  enqueues NOTHING - this kills the jobs-submitting-jobs-at-shutdown
  deadlock class. Undrained marshal posts are DROPPED with one warn;
  drain before tearing down if completions matter.
- Every record's Result defaults to the cancelled error: any terminal
  state reached without the callable running answers the cancelled error
  on take(), never a default-constructed success.

## Known tradeoffs
- `Record<T>::result`'s default-then-overwrite costs one Error construction
  per job; the alternative was an untyped "no result" hole that take()
  could not answer cleanly.
- The deliver loop copies the message string once per delivery (drain
  thread, under lock for the copy, outside it for the callback) - it keeps
  the callback free of any lifetime coupling to the record.
- `JobOptions.key/group` ride on the record but nothing reads them yet;
  the latest-wins key registry and the group registry are the next arc
  (published.hpp + debouncer.hpp land with them).

## Tests
ctest --preset debug -R jobs
14 cases: errors table + fail shape, the null handle on every call,
marshal FIFO + re-entrant post + the drain-thread identity, submit
happy paths (int/void, second take, take-before-terminal, distinct ids),
priority order on a gated single worker, cancel-before-pickup never
runs, cancel-mid-run honoured via WS_TRY(check_cancel), the lost cancel
race landing done, progress clamp/monotonic/coalesced delivery (one
delivery per drain, message pointer copied out), exceptions (std and
non-std), completions firing exactly once on the drain thread, and both
shutdown contracts (running job honours the teardown cancel + queued job
skipped; a submit racing teardown answers cancelled and enqueues
nothing). Deterministic-order tests gate the one worker with an in-test
spin flag; nothing sleeps longer than 2 ms per wait cycle.
