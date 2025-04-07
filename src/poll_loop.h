#ifndef POLL_LOOP_H
#define POLL_LOOP_H

#include <poll.h>   // For POLL* constants
#include <stdbool.h> // For bool type
#include <stddef.h> // For size_t

// --- Opaque Structure Definition ---

// Forward declare the main context struct. The implementation details are hidden.
typedef struct PollEventLoop PollEventLoop;

// --- Enums and Typedefs ---

// Represents the specific event types for registering callbacks.
typedef enum {
    EVENT_READ,   // Corresponds to POLLIN
    EVENT_WRITE,  // Corresponds to POLLOUT
    EVENT_ERROR,  // Corresponds to POLLERR
    EVENT_HUP,    // Corresponds to POLLHUP | POLLRDHUP
    EVENT_INVALID // Corresponds to POLLNVAL
    // Add more specific events if needed in the future
} PollEventType;

// Callback function pointer type.
// loop: Pointer to the PollEventLoop instance.
// fd: The file descriptor that triggered the event.
// user_data: The user-provided data associated with this fd during registration.
typedef void (*EventCallback)(PollEventLoop* loop, int fd, void* user_data);

// --- API Function Declarations ---

/**
 * @brief Creates and initializes a new PollEventLoop context.
 *
 * Allocates necessary internal structures and sets up the self-pipe mechanism
 * for interruption.
 *
 * @return A pointer to the newly created PollEventLoop context, or NULL on
 * failure (e.g., memory allocation error, pipe creation error).
 * Sets errno on failure.
 */
PollEventLoop* poll_loop_create();

/**
 * @brief Destroys a PollEventLoop context and releases all associated resources.
 *
 * Frees internal collections, closes the self-pipe file descriptors, and frees
 * the loop structure itself. Registered FDs are implicitly unregistered, but
 * user_data is NOT freed (user is responsible for managing that).
 * Handles NULL input gracefully (no-op).
 *
 * @param loop A pointer to the PollEventLoop context to destroy.
 */
void poll_loop_destroy(PollEventLoop* loop);

/**
 * @brief Registers a file descriptor with the event loop.
 *
 * Associates the given fd with the loop for monitoring. Initially, no event
 * callbacks are set. The user_data pointer will be passed to any callbacks
 * triggered for this fd.
 *
 * @param loop The PollEventLoop context.
 * @param fd The file descriptor to register. Must be non-negative.
 * @param user_data A pointer to user-specific data associated with this fd.
 * Can be NULL.
 * @return 0 on success.
 * -1 on error (e.g., invalid arguments, fd already registered,
 * memory allocation failure, loop is currently processing).
 * Sets errno on failure (e.g., EINVAL, EEXIST, ENOMEM, EBUSY).
 */
int poll_loop_register_fd(PollEventLoop* loop, int fd, void* user_data);

/**
 * @brief Unregisters a file descriptor from the event loop.
 *
 * Removes the fd from monitoring. Any associated callbacks are cleared.
 * The user is responsible for closing the file descriptor itself if necessary.
 *
 * @param loop The PollEventLoop context.
 * @param fd The file descriptor to unregister.
 * @return 0 on success.
 * -1 on error (e.g., invalid arguments, fd not found,
 * loop is currently processing).
 * Sets errno on failure (e.g., EINVAL, ENOENT, EBUSY).
 */
int poll_loop_unregister_fd(PollEventLoop* loop, int fd);

/**
 * @brief Sets or unsets a callback function for a specific event type on a
 * registered file descriptor.
 *
 * Associates a callback function with a particular event (read, write, etc.)
 * for the given fd. Setting callback to NULL effectively disables monitoring
 * for that specific event type for that fd.
 *
 * @param loop The PollEventLoop context.
 * @param fd The file descriptor to modify.
 * @param event_type The type of event to set the callback for.
 * @param callback The callback function pointer, or NULL to unset.
 * @return 0 on success.
 * -1 on error (e.g., invalid arguments, fd not found, invalid event type,
 * loop is currently processing).
 * Sets errno on failure (e.g., EINVAL, ENOENT, EBUSY).
 */
int poll_loop_set_callback(PollEventLoop* loop, int fd, PollEventType event_type, EventCallback callback);

/**
 * @brief Waits for events on registered file descriptors and dispatches callbacks.
 *
 * Blocks until at least one event occurs on a monitored fd, the timeout expires,
 * or an error occurs. Dispatches registered callbacks for triggered events.
 * Modifications to the loop (register/unregister/set_callback) are NOT allowed
 * while this function is executing (within a single call).
 *
 * @param loop The PollEventLoop context.
 * @param timeout_ms The maximum time to wait in milliseconds.
 * -1 means wait indefinitely.
 * 0 means return immediately (poll non-blockingly).
 * @return The number of file descriptors for which events were processed (> 0).
 * 0 if the timeout expired before any events occurred.
 * -1 on error (e.g., poll() failed, excluding EINTR).
 * Sets errno on failure. EINTR is handled internally and does not
 * result in an error return.
 */
int poll_loop_wait(PollEventLoop* loop, int timeout_ms);

/**
 * @brief Runs the event loop continuously until explicitly stopped.
 *
 * Repeatedly calls poll_loop_wait() with the specified timeout.
 * The loop continues until poll_loop_stop() is called or an unrecoverable
 * error occurs in poll_loop_wait().
 *
 * @param loop The PollEventLoop context.
 * @param timeout_ms The timeout for each internal call to poll_loop_wait().
 * -1 means wait indefinitely in each poll cycle.
 * @return 0 if the loop was stopped gracefully via poll_loop_stop().
 * -1 if the loop terminated due to an error from poll_loop_wait().
 * Sets errno if terminated by error.
 */
int poll_loop_run(PollEventLoop* loop, int timeout_ms);

/**
 * @brief Signals the event loop run by poll_loop_run() to stop gracefully.
 *
 * Sets a flag that poll_loop_run() checks. If the loop is currently blocked
 * in poll_loop_wait(), this function attempts to interrupt it using the
 * internal self-pipe mechanism. This function is safe to call from a signal
 * handler or another thread (basic signal safety, assumes write is atomic enough).
 *
 * @param loop The PollEventLoop context.
 */
void poll_loop_stop(PollEventLoop* loop);


#endif // POLL_LOOP_H