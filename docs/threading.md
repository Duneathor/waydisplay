# Threading contract

WayDisplay favors bounded latency over completing stale work. Thread ownership and lock scope are therefore part of the runtime contract, not implementation details.

## Server

The server uses the Wayland event-loop thread, one network/event thread, one stream-frame worker, bounded tile-compression workers, the video encoder worker, and optional audio workers. Scene damage and wlroots objects are owned exclusively by the Wayland event-loop thread. After readback, the compositor copies the small damage bitmap into the stream-frame mailbox and returns to dispatch; the stream worker owns the stable framebuffer and shadow until that job completes. New scene damage accumulates independently for the next frame. Other threads request a full refresh through the compositor request mailbox and wake the event loop; they never mutate scene-damage fields directly.

`wd_net_state.lock` is the primary cross-thread state lock. The network thread owns connection descriptors, protocol phase, transport queues, and stream-policy mutation. The compositor thread may take the lock only for short input, resize, completion, and policy transfers. It never waits for tile compression or transport submission. The stream-frame worker owns framebuffer comparison and queue servicing, releases the network lock while compression workers run, and validates generations after reacquiring it. Code holding the network lock must not perform blocking socket I/O, call into PipeWire, or invoke wlroots operations that may dispatch callbacks.

When more than one server lock is required, acquire them in this order:

1. `wd_net_state.lock`
2. `wd_net_state.video_encoder_lock`
3. worker-private mutexes

Condition-variable waits are the only operations permitted to release and reacquire the primary lock implicitly. Worker completion must publish results under the primary lock and must reject obsolete session, content-epoch, or generation work before submission.

## Client

The SDL/render thread owns SDL objects and presentation. One receive worker polls the control, video, audio, selection, and UDP io_uring completion descriptors. TCP channels use incremental nonblocking readers, and the receive worker performs framing and cheap dispatch only. Dedicated video and optional audio decode workers consume bounded media queues so codec work cannot stall control, selection, or tile reception. A saturated video queue is discarded and decoding resumes from the next keyframe; a saturated audio queue drops new packets rather than extending latency. Transmit queues use io_uring without permanent sender threads. Ring creation fails unless the Linux 5.14-era operations required by that queue are available. The session lifecycle owner shuts descriptors down before joining the receive worker and then stops the media workers before destroying codec state.

Single-mutex operations should keep their critical section local. Operations requiring multiple client state mutexes must use `std::scoped_lock`, which performs deadlock-avoiding acquisition, rather than a sequence of nested `std::lock_guard` objects.

The conceptual state order is:

1. session transport mutexes
2. remote-content/configuration state
3. framebuffer and dirty-region state
4. generation and retransmit state
5. video-frame and presentation telemetry state

No thread may call SDL while holding a producer-state mutex. The receive worker may signal `ClientRenderWake`, but only the render thread consumes and presents frames. Audio synchronization may discard or adjust audio, but it must not hold the video presentation path indefinitely.

## Queue policy

All cross-thread queues are bounded. Producers must discard obsolete generations and epochs before expensive processing and again before publication. Telemetry is always lower priority than control, input, retransmit, and media traffic and may be sampled approximately or dropped.
