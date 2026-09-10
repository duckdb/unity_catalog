"""Concurrency harness for the OSS UC optimistic-write / read-invariant tests.

Runs N writers + M readers concurrently, retries writers on a (returned) conflict, propagates the
first error, and keeps the threading out of the test body:

    stats = run_concurrent(writer=append, reader=check, writers=3, readers=3, commits=4)

`writer(i)` performs ONE write attempt and returns truthy on success / falsy on a retryable conflict
(exactly what `uc.duckdb`'s `commit()` returns). The harness loops it until `commits` successes.
`reader(i)` reads + asserts; it is looped until the writers finish. Readers are always released
(even if a writer raises), so a failure can't hang the run.

UC-LOCAL on purpose: the `commits`-to-N-with-retry shape is specific to optimistic-write correctness
testing (not a general concurrency primitive -- a benchmark harness would be duration/op-count based
with no retry). Kept here until a genuinely general shape earns a place in the driver.
"""

import threading
import time
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass


@dataclass
class Stats:
    """Outcome of a :func:`run_concurrent`: successful writes, retried conflicts, reader iterations."""

    commits: int
    conflicts: int
    reads: int


def run_concurrent(writer, reader, *, writers, readers, commits, max_attempts=100, deadline_s=120, reader_pause_s=0.02):
    """Run `writers` writers to `commits` successful writes each and `readers` readers in a poll loop,
    concurrently. Returns :class:`Stats`. Re-raises the first writer/reader exception (writer failures
    take precedence). `max_attempts`/`deadline_s` bound a non-converging conflict path so it fails
    loudly instead of spinning forever."""
    deadline = time.monotonic() + deadline_s

    def _writer(wid):
        ok = conflicts = attempts = 0
        while ok < commits:
            if time.monotonic() > deadline:
                raise TimeoutError(f"writer {wid} stalled at {ok}/{commits} ({conflicts} conflicts)")
            attempts += 1
            if attempts > max_attempts:
                raise RuntimeError(
                    f"writer {wid} livelocked: {attempts} attempts, {ok} commits, {conflicts} conflicts "
                    "-- optimistic-concurrency path not converging"
                )
            if writer(wid):
                ok += 1
            else:
                conflicts += 1
        return ok, conflicts

    def _reader(rid, stop):
        reads = 0
        while not stop.is_set():
            reader(rid)
            reads += 1
            time.sleep(reader_pause_s)
        return reads

    stop = threading.Event()
    with ThreadPoolExecutor(max_workers=writers + readers) as ex:
        wfuts = [ex.submit(_writer, i) for i in range(writers)]
        rfuts = [ex.submit(_reader, i, stop) for i in range(readers)]
        try:
            wr = [f.result() for f in wfuts]  # re-raises writer failures
        finally:
            stop.set()  # ALWAYS release readers, even if a writer raised (no hang)
        rd = [f.result() for f in rfuts]  # re-raises reader assertion failures

    return Stats(commits=sum(c for c, _ in wr), conflicts=sum(x for _, x in wr), reads=sum(rd))
