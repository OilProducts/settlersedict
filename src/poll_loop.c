#define _GNU_SOURCE // For pipe2, POLLRDHUP if needed explicitly
#include "poll_loop.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h> // For pipe2/fcntl flags

// --- Constants and Macros ---
#define INITIAL_CAPACITY 16
#define INITIAL_PENDING_CAPACITY 8 // Initial capacity for pending changes
#define SELF_PIPE_MARKER ((void*)0xDEADBEEF) // Special marker for self-pipe user_data

// --- Internal Data Structures ---

// Stores registration details for a single monitored file descriptor.
typedef struct {
    int fd;                 // The file descriptor number.
    void* user_data;        // User-provided context pointer.
    int pollfd_index;       // Index within the main pollfds array.

    // Callback pointers for different event types
    EventCallback on_read_callback;    // POLLIN
    EventCallback on_write_callback;   // POLLOUT
    EventCallback on_error_callback;   // POLLERR
    EventCallback on_hup_callback;     // POLLHUP | POLLRDHUP
    EventCallback on_invalid_callback; // POLLNVAL

} FDEventInfo;

// This struct holds the information necessary to make a modification to the pollfds struct.
// This is necessary because we cannot modify the pollfds array while we are iterating through it.
typedef struct PendingChange {
    enum { CHANGE_ADD, CHANGE_REMOVE, CHANGE_SET_CALLBACK } type;
    int fd;
    void* user_data; // For ADD
    PollEventType event_type; // For SET_CALLBACK
    EventCallback callback;  // For SET_CALLBACK
} PendingChange;

// The main opaque context structure definition.
struct PollEventLoop {
    FDEventInfo** fds_info;        // Dynamic array of pointers to FDEventInfo
    struct pollfd* pollfds;       // Dynamic array mirroring fds_info for poll()
    size_t count;                 // Number of currently registered FDs
    size_t capacity;              // Allocated capacity of the arrays

    bool is_processing;           // True if poll_loop_wait is active
    bool stop_requested;          // Flag for poll_loop_run exit

    int self_pipe_read_fd;        // Read end of the self-pipe
    int self_pipe_write_fd;       // Write end of the self-pipe

    // --- Fields for Deferred Updates ---
    struct PendingChange* pending_changes;
    size_t pending_count;
    size_t pending_capacity;
};

// --- Internal Helper Function Declarations ---

static int find_fd_index(PollEventLoop* loop, int fd);
static int resize_arrays(PollEventLoop* loop);
static int resize_pending_array(PollEventLoop* loop);
static short calculate_events_mask(const FDEventInfo* info);
static void self_pipe_read_handler(PollEventLoop* loop, int fd, void* user_data);
static int add_pending_change(PollEventLoop* loop, PendingChange change);
static int do_register_fd(PollEventLoop* loop, int fd, void* user_data);
static int do_unregister_fd(PollEventLoop* loop, int fd);
static int do_set_callback(PollEventLoop* loop, int fd, PollEventType event_type, EventCallback callback);
static int poll_loop_apply_pending(PollEventLoop* loop);


// --- Internal Helper Functions ---

// Finds the index of an FDEventInfo in the fds_info array by fd number.
// Returns index if found, -1 otherwise.
static int find_fd_index(PollEventLoop* loop, int fd) {
    if (!loop || fd < 0) {
        return -1;
    }
    for (size_t i = 0; i < loop->count; ++i) {
        if (loop->fds_info[i]->fd == fd) {
            return (int)i;
        }
    }
    return -1;
}

// Resizes the internal dynamic arrays (fds_info and pollfds).
// Returns 0 on success, -1 on failure (sets errno).
static int resize_arrays(PollEventLoop* loop) {
    if (!loop) return -1;

    size_t new_capacity = (loop->capacity == 0) ? INITIAL_CAPACITY : loop->capacity * 2;

    FDEventInfo** new_fds_info = realloc(loop->fds_info, new_capacity * sizeof(FDEventInfo*));
    if (!new_fds_info) {
        // errno already set by realloc
        return -1;
    }
    loop->fds_info = new_fds_info;

    struct pollfd* new_pollfds = realloc(loop->pollfds, new_capacity * sizeof(struct pollfd));
    if (!new_pollfds) {
        // errno already set by realloc
        // Note: fds_info was already successfully reallocated. This is awkward.
        // For simplicity here, we don't try to shrink fds_info back.
        // A more robust implementation might use temporary pointers.
        return -1;
    }
    loop->pollfds = new_pollfds;

    loop->capacity = new_capacity;
    return 0;
}

// Calculates the combined poll 'events' mask based on set callbacks.
static short calculate_events_mask(const FDEventInfo* info) {
    short mask = 0;
    if (!info) return 0;

    if (info->on_read_callback)    mask |= POLLIN;
    if (info->on_write_callback)   mask |= POLLOUT;
    if (info->on_error_callback)   mask |= POLLERR;
    // Always listen for HUP/INVALID if *any* callback is set, or specifically if their own callbacks are set.
    // Simpler: Listen if their specific callbacks are set.
    if (info->on_hup_callback)     mask |= (POLLHUP); // ifdef to put POLLRDHUP here on linux
    if (info->on_invalid_callback) mask |= POLLNVAL;

    // Even if no specific error/hup/invalid callback is set, poll() reports
    // these conditions anyway if the fd is being polled for read/write.
    // We might want to *always* include POLLERR, POLLHUP, POLLNVAL
    // if *any* callback is set, to allow generic error reporting even
    // without specific handlers. Let's adopt that:
    if (mask != 0) { // If we are polling for *anything*
        mask |= POLLERR | POLLHUP | POLLNVAL;  // ifdef to put POLLRDHUP here on linux
    }
    // Refined approach: Only add POLLERR/HUP/NVAL if *their specific* callback is set OR if read/write callback is set.
    mask = 0; // Reset for clarity
    if (info->on_read_callback)    mask |= POLLIN;
    if (info->on_write_callback)   mask |= POLLOUT;
    if (info->on_error_callback)   mask |= POLLERR;
    if (info->on_hup_callback)     mask |= (POLLHUP);  // ifdef to put POLLRDHUP here on linux
    if (info->on_invalid_callback) mask |= POLLNVAL;

     // If interested in read or write, also implicitly interested in errors/hangups
     // related to those operations, even if no specific handler is set for them.
    // Let poll() report them; dispatch logic will check for callbacks.
    if (info->on_read_callback || info->on_write_callback) {
         mask |= POLLERR | POLLHUP | POLLNVAL;  // ifdef to put POLLRDHUP here on linux
    }


    return mask;
}

// Internal callback for the self-pipe read end. Just consumes the byte.
static void self_pipe_read_handler(PollEventLoop* loop, int fd, void* user_data) {
    (void)loop;       // Unused parameter
    (void)user_data; // Unused parameter
    char buf[1];
    ssize_t nread;
    // Read until pipe is empty or error (like EAGAIN/EWOULDBLOCK)
    do {
        nread = read(fd, buf, sizeof(buf));
    } while (nread > 0);
    // Ignore errors like EAGAIN, as the purpose was just to wake up.
}

// Resizes the pending_changes array.
static int resize_pending_array(PollEventLoop* loop) {
    if (!loop) return -1;
    size_t new_capacity = (loop->pending_capacity == 0) ? INITIAL_PENDING_CAPACITY : loop->pending_capacity * 2;
    PendingChange* new_pending = realloc(loop->pending_changes, new_capacity * sizeof(PendingChange));
    if (!new_pending) {
        return -1; // errno set by realloc
    }
    loop->pending_changes = new_pending;
    loop->pending_capacity = new_capacity;
    return 0;
}

// Adds a change request to the pending list.
static int add_pending_change(PollEventLoop* loop, PendingChange change) {
    if (!loop) return -1;
    if (loop->pending_count >= loop->pending_capacity) {
        if (resize_pending_array(loop) != 0) {
            return -1; // errno set
        }
    }
    loop->pending_changes[loop->pending_count++] = change;
    return 0;
}

// --- Internal Worker Functions (Core Logic without is_processing check) ---

// Worker for registering an FD.
static int do_register_fd(PollEventLoop* loop, int fd, void* user_data) {
    // Check if already registered (even if pending removal!)
    // This prevents adding duplicates before pending removals are processed.
    if (find_fd_index(loop, fd) != -1) {
        errno = EEXIST;
        return -1;
    }

    // Ensure capacity in main arrays
    if (loop->count >= loop->capacity) {
        if (resize_arrays(loop) != 0) {
            return -1;
        }
    }

    FDEventInfo* new_info = malloc(sizeof(FDEventInfo));
    if (!new_info) {
        return -1;
    }
    new_info->fd = fd;
    new_info->user_data = user_data;
    new_info->pollfd_index = (int)loop->count; // Its index will be the current count
    new_info->on_read_callback = NULL;
    new_info->on_write_callback = NULL;
    new_info->on_error_callback = NULL;
    new_info->on_hup_callback = NULL;
    new_info->on_invalid_callback = NULL;

    loop->fds_info[loop->count] = new_info;
    loop->pollfds[loop->count].fd = fd;
    loop->pollfds[loop->count].events = 0;
    loop->pollfds[loop->count].revents = 0;

    loop->count++;
    return 0;
}

// Worker for unregistering an FD.
static int do_unregister_fd(PollEventLoop* loop, int fd) {
    int index_to_remove = find_fd_index(loop, fd);
    if (index_to_remove < 0) {
        errno = ENOENT;
        return -1;
    }

    FDEventInfo* info_to_remove = loop->fds_info[index_to_remove];
    size_t last_index = loop->count - 1;

    if ((size_t)index_to_remove < last_index) {
        FDEventInfo* last_info = loop->fds_info[last_index];
        loop->fds_info[index_to_remove] = last_info;
        loop->pollfds[index_to_remove] = loop->pollfds[last_index];
        last_info->pollfd_index = index_to_remove;
    }

    loop->count--;
    free(info_to_remove);
    return 0;
}

// Worker for setting a callback.
static int do_set_callback(PollEventLoop* loop, int fd, PollEventType event_type, EventCallback callback) {
    int index = find_fd_index(loop, fd);
    if (index < 0) {
        errno = ENOENT;
        return -1;
    }

    FDEventInfo* info = loop->fds_info[index];

    switch (event_type) {
        case EVENT_READ:    info->on_read_callback = callback; break;
        case EVENT_WRITE:   info->on_write_callback = callback; break;
        case EVENT_ERROR:   info->on_error_callback = callback; break;
        case EVENT_HUP:     info->on_hup_callback = callback; break;
        case EVENT_INVALID: info->on_invalid_callback = callback; break;
        default:
            errno = EINVAL;
        return -1;
    }

    loop->pollfds[info->pollfd_index].events = calculate_events_mask(info);
    return 0;
}

// --- API Function Implementations ---

PollEventLoop* poll_loop_create() {
    PollEventLoop* loop = malloc(sizeof(PollEventLoop));
    if (!loop) {
        return NULL; // errno set by malloc
    }

    loop->fds_info = NULL;
    loop->pollfds = NULL;
    loop->count = 0;
    loop->capacity = 0;
    loop->is_processing = false;
    loop->stop_requested = false;
    loop->self_pipe_read_fd = -1;
    loop->self_pipe_write_fd = -1;
    loop->pending_changes = NULL;
    loop->pending_count = 0;
    loop->pending_capacity = 0; // Will allocate on first pending change

    // Allocate initial arrays
    if (resize_arrays(loop) != 0) {
        free(loop);
        return NULL; // errno set by resize_arrays (realloc)
    }

    // SELF PIPE
    // pipe2 (preferable for atomicity of flags) or pipe + fcntl. Both ends are set non-blocking
    // and close-on-exec. The read end of the pipe is registered with the loop itself using
    // poll_loop_register_fd and poll_loop_set_callback. It has a simple internal callback
    // (self_pipe_read_handler) that just reads the byte(s) to clear the pipe. poll_loop_stop writes
    // a single byte to the write end of the pipe. This causes the poll() call in poll_loop_wait to
    // wake up with a POLLIN event on the pipe's read end, allowing poll_loop_run to check the
    // stop_requested flag.
    int pipe_fds[2];
    // Use pipe2 for O_CLOEXEC and O_NONBLOCK atomically if available
    #ifdef HAVE_PIPE2
        if (pipe2(pipe_fds, O_CLOEXEC | O_NONBLOCK) == -1) {
            poll_loop_destroy(loop); // Clean up allocated memory
            return NULL; // errno set by pipe2
        }
    #else
        if (pipe(pipe_fds) == -1) {
            poll_loop_destroy(loop);
            return NULL; // errno set by pipe
        }
        // Set CLOEXEC and NONBLOCK manually
        int flags;
        // Read end
        flags = fcntl(pipe_fds[0], F_GETFL, 0);
        if (flags == -1 || fcntl(pipe_fds[0], F_SETFL, flags | O_NONBLOCK) == -1) {
            close(pipe_fds[0]);
            close(pipe_fds[1]);
            poll_loop_destroy(loop);
            return NULL; // errno set by fcntl
        }
        flags = fcntl(pipe_fds[0], F_GETFD, 0);
         if (flags == -1 || fcntl(pipe_fds[0], F_SETFD, flags | FD_CLOEXEC) == -1) {
            close(pipe_fds[0]);
            close(pipe_fds[1]);
            poll_loop_destroy(loop);
            return NULL; // errno set by fcntl
        }
       // Write end (only needs CLOEXEC, non-blocking isn't critical but harmless)
       flags = fcntl(pipe_fds[1], F_GETFL, 0);
       if (flags == -1 || fcntl(pipe_fds[1], F_SETFL, flags | O_NONBLOCK) == -1) {
            // Non-critical error, maybe log it? Proceed anyway.
       }
        flags = fcntl(pipe_fds[1], F_GETFD, 0);
        if (flags == -1 || fcntl(pipe_fds[1], F_SETFD, flags | FD_CLOEXEC) == -1) {
            close(pipe_fds[0]);
            close(pipe_fds[1]);
            poll_loop_destroy(loop);
            return NULL; // errno set by fcntl
        }
    #endif

    loop->self_pipe_read_fd = pipe_fds[0];
    loop->self_pipe_write_fd = pipe_fds[1];

    if (do_register_fd(loop, loop->self_pipe_read_fd, SELF_PIPE_MARKER) != 0) {
        poll_loop_destroy(loop);
        return NULL;
    }
    if (do_set_callback(loop, loop->self_pipe_read_fd, EVENT_READ, self_pipe_read_handler) != 0) {
        poll_loop_destroy(loop);
        return NULL;
    }



    return loop;
}

int poll_loop_register_fd(PollEventLoop* loop, int fd, void* user_data) {
    if (!loop || fd < 0) {
        errno = EINVAL;
        return -1;
    }

    if (loop->is_processing) {
        // Defer the operation
        PendingChange change = {
            .type = CHANGE_ADD,
            .fd = fd,
            .user_data = user_data
            // Other fields are irrelevant for ADD
        };
        if (add_pending_change(loop, change) != 0) {
            // Failed to queue (likely ENOMEM)
            return -1; // errno set by add_pending_change/resize
        }
        return 0; // Successfully queued
    } else {
        // Perform immediately
        return do_register_fd(loop, fd, user_data);
    }
}

int poll_loop_unregister_fd(PollEventLoop* loop, int fd) {
    if (!loop || fd < 0) {
        errno = EINVAL;
        return -1;
    }

    if (loop->is_processing) {
        // Defer the operation
        PendingChange change = {
            .type = CHANGE_REMOVE,
            .fd = fd
            // Other fields irrelevant
        };
        if (add_pending_change(loop, change) != 0) {
            return -1;
        }
        return 0; // Successfully queued
    } else {
        // Perform immediately
        return do_unregister_fd(loop, fd);
    }
}

int poll_loop_set_callback(PollEventLoop* loop, int fd, PollEventType event_type, EventCallback callback) {
    if (!loop || fd < 0) {
        errno = EINVAL;
        return -1;
    }

    if (loop->is_processing) {
        // Defer the operation
        PendingChange change = {
            .type = CHANGE_SET_CALLBACK,
            .fd = fd,
            .event_type = event_type,
            .callback = callback
        };
        if (add_pending_change(loop, change) != 0) {
            return -1;
        }
        return 0; // Successfully queued
    } else {
        // Perform immediately
        return do_set_callback(loop, fd, event_type, callback);
    }
}

/**
 * @brief Destroys a PollEventLoop context and releases all associated resources.
 *
 * Frees internal collections (FD info, pollfds, pending changes), closes
 * the self-pipe file descriptors, and frees the loop structure itself.
 * Registered FDs are implicitly unregistered, but user_data is NOT freed
 * (user is responsible for managing that). Any pending changes that were
 * not applied are discarded.
 * Handles NULL input gracefully (no-op).
 *
 * @param loop A pointer to the PollEventLoop context to destroy.
 */
void poll_loop_destroy(PollEventLoop* loop) {
    if (!loop) {
        return; // Handle NULL input gracefully
    }

    // 1. Free individual FDEventInfo structures
    // These hold the user_data and callback pointers for each registered FD.
    // The loop owns these structures.
    if (loop->fds_info != NULL) {
        for (size_t i = 0; i < loop->count; ++i) {
            // Check if the pointer itself is valid before freeing, although
            // it should be if registration logic is correct.
            if (loop->fds_info[i] != NULL) {
                free(loop->fds_info[i]);
                // loop->fds_info[i] = NULL; // Optional: Good practice
            }
        }
    }

    // 2. Free the main dynamic arrays used for active polling
    free(loop->fds_info);       // Free the array of pointers
    free(loop->pollfds);        // Free the array of pollfd structs

    // 3. Free the pending changes dynamic array
    // This releases the memory holding any queued changes that might not
    // have been applied if destroy is called abruptly.
    free(loop->pending_changes); // Free the array of PendingChange structs

    // 4. Close self-pipe file descriptors (if they were successfully created)
    // Check against -1 which is a common indicator for invalid/uninitialized FDs.
    if (loop->self_pipe_read_fd != -1) {
        close(loop->self_pipe_read_fd);
        // loop->self_pipe_read_fd = -1; // Optional: Mark as closed
    }
    if (loop->self_pipe_write_fd != -1) {
        close(loop->self_pipe_write_fd);
        // loop->self_pipe_write_fd = -1; // Optional: Mark as closed
    }

    // 5. Free the PollEventLoop structure itself
    // This should be the last step after all resources owned by the struct
    // have been released.
    free(loop);
}

// --- Function to Apply Pending Changes ---

/**
 * @brief Applies all queued changes (add, remove, set_callback).
 *
 * Iterates through the pending changes list and applies them using the
 * internal do_* worker functions. Clears the pending list afterwards.
 * If multiple changes affect the same FD, they are processed in the order
 * they were added.
 *
 * @param loop The PollEventLoop context.
 * @return 0 on success. -1 if *any* change failed to apply. Sets errno
 * to the value associated with the *first* error encountered.
 */
static int poll_loop_apply_pending(PollEventLoop* loop) {
    if (!loop) return -1; // Should not happen internally

    int first_error = 0; // Store the errno of the first failure
    int overall_rc = 0;  // Overall return code for this function

    for (size_t i = 0; i < loop->pending_count; ++i) {
        PendingChange* change = &loop->pending_changes[i];
        int rc = 0;

        switch (change->type) {
            case CHANGE_ADD:
                rc = do_register_fd(loop, change->fd, change->user_data);
                break;
            case CHANGE_REMOVE:
                rc = do_unregister_fd(loop, change->fd);
                 // It's NOT necessarily an error if ENOENT occurs here,
                 // because a previous change in *this same batch* might have
                 // already removed it. We could choose to ignore ENOENT.
                 // Let's ignore ENOENT for REMOVE operations during apply.
                 if (rc == -1 && errno == ENOENT) {
                    rc = 0; // Treat as success if already gone
                    errno = 0; // Clear errno
                 }
                break;
            case CHANGE_SET_CALLBACK:
                rc = do_set_callback(loop, change->fd, change->event_type, change->callback);
                 // Similarly, ignore ENOENT for set_callback
                 if (rc == -1 && errno == ENOENT) {
                    rc = 0; // Treat as success if already gone
                    errno = 0; // Clear errno
                 }
                break;
        }

        // Handle errors from do_* functions
        if (rc == -1) {
             fprintf(stderr, "[PollLoop] Failed to apply pending change (type: %d, fd: %d): %s\n",
                     change->type, change->fd, strerror(errno));
            if (overall_rc == 0) {
                // Store the first error encountered
                first_error = errno;
                overall_rc = -1; // Mark that at least one error occurred
            }
            // Continue processing remaining changes (Strategy 2)
        }
    }

    // Clear the pending list regardless of errors
    loop->pending_count = 0;

    // Set errno to the first error that occurred, if any
    if (overall_rc == -1) {
        errno = first_error;
    }

    return overall_rc; // 0 if all succeeded, -1 if any failed
}

int poll_loop_wait(PollEventLoop* loop, int timeout_ms) {
    if (!loop) {
        errno = EINVAL;
        return -1;
    }

    // *** 1. Apply pending changes from the previous iteration ***
    if (poll_loop_apply_pending(loop) != 0) {
        // An error occurred while applying changes.
        // errno is set by poll_loop_apply_pending to the first error.
        // Propagate this error upwards.
        fprintf(stderr, "[PollLoop] Error applying pending changes before poll: %s\n", strerror(errno));
        // We do NOT set is_processing=true here, as we are erroring out before polling.
        return -1;
    }

    // *** 2. Set processing flag ***
    // Set AFTER applying changes and BEFORE polling
    loop->is_processing = true;

    int num_events;
    do {
        num_events = poll(loop->pollfds, loop->count, timeout_ms);
    } while (num_events == -1 && errno == EINTR); // Retry on interrupt

    // Error or Timeout check
    if (num_events <= 0) {
        loop->is_processing = false; // Clear flag before returning
        // Note: errno is set by poll() if num_events < 0
        return num_events; // Return poll() error or 0 for timeout
    }

    // --- Dispatch events ---
    int processed_count = 0;
    // Iterate up to the current count *before* dispatching, in case a
    // callback modifies the loop (which it shouldn't, but defensively)
    size_t current_count = loop->count;
    for (size_t i = 0; i < current_count; ++i) {
        short revents = loop->pollfds[i].revents;
        if (revents == 0) {
            continue; // No events for this FD
        }

        // Defensively check if the index is still valid, though it should be
        // if is_processing prevents modification during this loop.
        if (i >= loop->count) {
            fprintf(stderr, "[PollLoop] Index %zu out of bounds during dispatch (count=%zu), indicates modification during dispatch?\n", i, loop->count);
            continue;
        }

        FDEventInfo* info = loop->fds_info[i];
        int current_fd = info->fd; // Store fd in case info becomes invalid

        // If the FD was unregistered *during* callback dispatch from a previous
        // iteration (violating the 'is_processing' rule), info might be invalid.
        // The design *explicitly disallows* this, so we assume info is valid.

        // Check and dispatch each event type
        // Use & operator for checking specific flags
        if ((revents & POLLIN) && info->on_read_callback) {
            info->on_read_callback(loop, info->fd, info->user_data);
        }
        if ((revents & POLLOUT) && info->on_write_callback) {
            info->on_write_callback(loop, info->fd, info->user_data);
        }
         // Combine POLLHUP and POLLRDHUP for the HUP callback
        if ((revents & (POLLHUP)) && info->on_hup_callback) { // ifdef to put POLLRDHUP here on linux
            info->on_hup_callback(loop, info->fd, info->user_data);
        }
        if ((revents & POLLERR) && info->on_error_callback) {
             info->on_error_callback(loop, info->fd, info->user_data);
        }
        if ((revents & POLLNVAL) && info->on_invalid_callback) {
            info->on_invalid_callback(loop, info->fd, info->user_data);
        }
         // Clear revents after processing (optional, but good practice)
        if (i < loop->count && loop->pollfds[i].fd == current_fd) {
            loop->pollfds[i].revents = 0;
        }
        processed_count++;
    }

    // Modification safety: Clear flag
    loop->is_processing = false;

    return processed_count; // Return number of FDs that had events
}


int poll_loop_run(PollEventLoop* loop, int timeout_ms) {
    if (!loop) {
        errno = EINVAL;
        return -1;
    }

    loop->stop_requested = false;
    int result = 0;

    while (!loop->stop_requested) {
        int wait_result = poll_loop_wait(loop, timeout_ms);
        if (wait_result < 0) {
            // A real error occurred in poll_loop_wait
            result = -1; // Store error indicator
            // errno is already set by poll_loop_wait
            break;
        }
        // If wait_result >= 0 (timeout or events processed), continue loop
    }

    // Loop exited, either by stop_requested or error
    return result;
}

// This is necessary to stop the loop from outside. (signal handler or another thread)
void poll_loop_stop(PollEventLoop* loop) {
    if (!loop) {
        return;
    }

    loop->stop_requested = true;

    // Wake up the poll() call if it's blocked
    if (loop->self_pipe_write_fd != -1) {
        char dummy_byte = 'S'; // Content doesn't matter
        ssize_t w_res;
        do {
            w_res = write(loop->self_pipe_write_fd, &dummy_byte, 1);
        } while (w_res == -1 && errno == EINTR);
        // Ignore other errors like EAGAIN (pipe full) or EPIPE (read end closed).
        // The goal is just to signal; if it fails, poll will eventually time out.
    }
}