
# poll_loop - A Simple C Event Loop Library using poll()

A C library designed to simplify event-driven I/O programming by providing a clean abstraction layer over the `poll()` system call. It allows monitoring multiple file descriptors for various event types and dispatching specific user-defined callback functions when events occur.

## Features

* Monitors multiple file descriptors using `poll()`.
* Allows registration of distinct callbacks for individual event types (`READ`, `WRITE`, `ERROR`, `HUP`, `INVALID`) per file descriptor.
* Central event loop context (`PollEventLoop`) manages all registered FDs and callbacks.
* **Modification Safety:** Uses a deferred update mechanism. Modifications requested during event processing (within `poll_loop_wait`) are queued and applied safely before the next poll cycle, preventing corruption of internal state.
* Simple API for creating/destroying the loop, registering/unregistering FDs, setting/unsetting callbacks, and running the loop.
* Includes a self-pipe mechanism for cleanly interrupting a blocked `poll()` call via `poll_loop_stop()`.

## Core Concepts

* **`PollEventLoop`**: An opaque handle representing the event loop instance. All operations are performed relative to this context.
* **File Descriptors (FDs)**: Integers representing open files, sockets, pipes, etc., that the loop monitors for I/O events.
* **Callbacks (`EventCallback`)**: User-defined functions with the signature `void (*EventCallback)(PollEventLoop* loop, int fd, void* user_data)` that are executed when a specific event occurs on a registered FD.
* **Event Types (`PollEventType`)**: An enum (`EVENT_READ`, `EVENT_WRITE`, `EVENT_ERROR`, `EVENT_HUP`, `EVENT_INVALID`) used to specify which event a callback should be registered for. `EVENT_HUP` corresponds to `POLLHUP | POLLRDHUP`.
* **User Data (`void* user_data`)**: An arbitrary pointer associated with each registered FD, passed back to the callback functions for context. The library does not manage the memory pointed to by `user_data`.
* **Deferred Updates**: To ensure stability, calls made to `poll_loop_register_fd`, `poll_loop_unregister_fd`, or `poll_loop_set_callback` while the loop is actively polling or dispatching callbacks (i.e., inside a `poll_loop_wait` call) are queued. These pending changes are applied atomically at the beginning of the *next* `poll_loop_wait` call. This means changes made within a callback will only take effect in the subsequent loop iteration.

## Getting Started

### Compilation

The library consists of `poll_loop.h` and `poll_loop.c`. Compile `poll_loop.c` along with your application code.

Example using GCC:

```bash
gcc your_app.c poll_loop.c -o your_app -Wall -Wextra -g
```

### Basic Usage Example

```c
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h> // For pipe, read, write, close
#include <errno.h>
#include <string.h>
#include "poll_loop.h"

// Simple callback for reading data
void my_read_callback(PollEventLoop* loop, int fd, void* user_data) {
    char buffer[100];
    ssize_t n = read(fd, buffer, sizeof(buffer) - 1);
    printf("Callback called for FD %d (user_data: %p)\n", fd, user_data);

    if (n > 0) {
        buffer[n] = '\0';
        printf("Read %zd bytes: %s\n", n, buffer);
    } else if (n == 0) {
        printf("FD %d closed (EOF).\n", fd);
        // Unregister and close FD (deferred if called during dispatch)
        poll_loop_unregister_fd(loop, fd);
        close(fd); // User is responsible for closing the FD
    } else { // n < 0
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            perror("Read error");
            // Unregister and close on error
            poll_loop_unregister_fd(loop, fd);
            close(fd);
        }
        // EAGAIN means try again later (no data now)
    }
}

int main() {
    // Create a pipe for the example
    int pipe_fds[2];
    if (pipe(pipe_fds) == -1) {
        perror("pipe");
        return EXIT_FAILURE;
    }
    int read_fd = pipe_fds[0];
    int write_fd = pipe_fds[1];

    // Make read end non-blocking (optional but common)
    //fcntl(read_fd, F_SETFL, fcntl(read_fd, F_GETFL) | O_NONBLOCK);

    // 1. Create the event loop
    PollEventLoop* loop = poll_loop_create();
    if (!loop) {
        fprintf(stderr, "Failed to create loop: %s\n", strerror(errno));
        close(read_fd); close(write_fd);
        return EXIT_FAILURE;
    }

    // 2. Register the read end of the pipe
    // Pass some dummy user data (e.g., the fd itself)
    if (poll_loop_register_fd(loop, read_fd, (void*)(intptr_t)read_fd) != 0) {
        fprintf(stderr, "Failed to register FD %d: %s\n", read_fd, strerror(errno));
        poll_loop_destroy(loop);
        close(read_fd); close(write_fd);
        return EXIT_FAILURE;
    }

    // 3. Set the read callback for the pipe's read end
    if (poll_loop_set_callback(loop, read_fd, EVENT_READ, my_read_callback) != 0) {
        fprintf(stderr, "Failed to set callback for FD %d: %s\n", read_fd, strerror(errno));
        // Unregister not strictly needed before destroy, but good practice
        // poll_loop_unregister_fd(loop, read_fd);
        poll_loop_destroy(loop);
        close(read_fd); close(write_fd);
        return EXIT_FAILURE;
    }

    printf("Event loop running. Write to the console to send data through the pipe...\n");
    printf("(Type 'quit' to stop the loop via the pipe)\n");

    // Example: Write something to the pipe initially
    write(write_fd, "Hello!\n", 7);

    // 4. Run the event loop (blocks here)
    // Use timeout -1 to block indefinitely per poll cycle until events
    int run_result = poll_loop_run(loop, -1);

    if (run_result < 0) {
        fprintf(stderr, "Loop exited with error: %s\n", strerror(errno));
    } else {
        printf("Loop stopped.\n");
    }

    // 5. Cleanup
    printf("Cleaning up...\n");
    // FDs registered with the loop should ideally be closed by user code
    // (e.g., in callbacks when done, or here if not closed yet)
    // close(read_fd); // Might be closed in callback
    close(write_fd); // Close the write end
    poll_loop_destroy(loop); // Destroy the loop context

    return (run_result == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
```

## API Reference

---

### `PollEventLoop* poll_loop_create()`

* **Description:** Creates and initializes a new, empty `PollEventLoop` context. Sets up internal structures and the self-pipe mechanism.
* **Parameters:** None.
* **Returns:** A pointer to the new `PollEventLoop` on success, or `NULL` on failure (e.g., memory allocation, pipe creation error). Sets `errno` on failure.

---

### `void poll_loop_destroy(PollEventLoop* loop)`

* **Description:** Destroys the event loop context and releases all associated resources. This includes freeing internal memory for FD tracking and pending changes, and closing the self-pipe FDs. It does *not* close the file descriptors registered by the user, nor does it free any `user_data`. Any pending changes are discarded. Handles `NULL` input gracefully.
* **Parameters:**
    * `loop`: The `PollEventLoop` context to destroy.
* **Returns:** `void`.

---

### `int poll_loop_register_fd(PollEventLoop* loop, int fd, void* user_data)`

* **Description:** Registers a file descriptor `fd` with the event loop for monitoring. No events are monitored initially until callbacks are set. The provided `user_data` pointer will be passed to callbacks for this `fd`. If called while the loop is processing events (inside `poll_loop_wait`), the registration is deferred until the start of the next `poll_loop_wait` call.
* **Parameters:**
    * `loop`: The `PollEventLoop` context.
    * `fd`: The non-negative file descriptor to register.
    * `user_data`: An optional user-defined pointer associated with this `fd`.
* **Returns:** `0` on success (or if successfully queued). `-1` on error. Sets `errno` (e.g., `EINVAL` for invalid arguments, `EEXIST` if `fd` is already registered, `ENOMEM` for allocation failure, `EBUSY` is not returned due to deferral but `ENOMEM` could occur when queueing).

---

### `int poll_loop_unregister_fd(PollEventLoop* loop, int fd)`

* **Description:** Unregisters a file descriptor `fd` from the event loop, removing it from monitoring. If called while the loop is processing events, the unregistration is deferred until the start of the next `poll_loop_wait` call. The user is still responsible for closing the `fd` itself.
* **Parameters:**
    * `loop`: The `PollEventLoop` context.
    * `fd`: The file descriptor to unregister.
* **Returns:** `0` on success (or if successfully queued). `-1` on error. Sets `errno` (e.g., `EINVAL` for invalid arguments, `ENOENT` if `fd` was not found).

---

### `int poll_loop_set_callback(PollEventLoop* loop, int fd, PollEventType event_type, EventCallback callback)`

* **Description:** Sets or unsets the callback function for a specific `event_type` on the registered file descriptor `fd`. Setting `callback` to `NULL` disables monitoring for that specific event type. If called while the loop is processing events, the change is deferred until the start of the next `poll_loop_wait` call.
* **Parameters:**
    * `loop`: The `PollEventLoop` context.
    * `fd`: The registered file descriptor to modify.
    * `event_type`: The `PollEventType` enum value indicating which event callback to set.
    * `callback`: The function pointer (`EventCallback`) to be called, or `NULL` to disable the callback for this event type.
* **Returns:** `0` on success (or if successfully queued). `-1` on error. Sets `errno` (e.g., `EINVAL` for invalid arguments or event type, `ENOENT` if `fd` was not found).

---

### `int poll_loop_wait(PollEventLoop* loop, int timeout_ms)`

* **Description:** Waits for events on registered file descriptors.
    1.  Applies any pending changes (register/unregister/set_callback) that were queued in the previous iteration or since the last call.
    2.  Calls `poll()` with the specified `timeout_ms`.
    3.  If events occur, dispatches the corresponding registered callbacks for each triggered event on each active FD. Callbacks executed during dispatch may queue new changes for the *next* iteration.
        Modifications via the API are deferred while this function is executing.
* **Parameters:**
    * `loop`: The `PollEventLoop` context.
    * `timeout_ms`: Maximum time to block in milliseconds. `-1` waits indefinitely, `0` returns immediately (non-blocking poll).
* **Returns:**
    * Number of file descriptors for which events were processed (`> 0`).
    * `0` if the timeout expired before any events occurred.
    * `-1` on error (e.g., `poll()` failed with an error other than `EINTR`, or an error occurred while applying pending changes at the start). Sets `errno`.

---

### `int poll_loop_run(PollEventLoop* loop, int timeout_ms)`

* **Description:** Runs the event loop continuously. It repeatedly calls `poll_loop_wait(loop, timeout_ms)` until `poll_loop_stop()` is called or an unrecoverable error occurs within `poll_loop_wait`.
* **Parameters:**
    * `loop`: The `PollEventLoop` context.
    * `timeout_ms`: The timeout passed to each internal call to `poll_loop_wait`.
* **Returns:** `0` if the loop was stopped gracefully via `poll_loop_stop()`. `-1` if the loop terminated due to an error from `poll_loop_wait()`. Sets `errno` if terminated by error.

---

### `void poll_loop_stop(PollEventLoop* loop)`

* **Description:** Signals the event loop started by `poll_loop_run()` to stop gracefully after the current `poll_loop_wait` iteration completes. It does this by setting an internal flag and writing to the self-pipe to potentially interrupt a blocked `poll()` call. Safe to call from signal handlers or other threads (basic signal/thread safety for the flag set and pipe write).
* **Parameters:**
    * `loop`: The `PollEventLoop` context.
* **Returns:** `void`.

---

## Error Handling

Functions generally return `0` on success and `-1` on error. When an error occurs, the `errno` variable is set to indicate the specific error condition (e.g., `EINVAL`, `ENOENT`, `EEXIST`, `ENOMEM`). Check the documentation for each function for specific error possibilities.

## Thread Safety

This library is **NOT** thread-safe by default. Accessing the same `PollEventLoop` context (calling any API functions) concurrently from multiple threads without external locking will lead to race conditions and undefined behavior.

The only exception is `poll_loop_stop()`, which is designed to be safe to call from another thread or a signal handler to initiate a graceful shutdown of a loop running in a different thread. However, all other functions require explicit synchronization (e.g., using mutexes) if used across threads.

