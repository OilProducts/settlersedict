#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "src/poll_loop.h" // Include the library header
#include "src/tcp_to_stdout.h"

int main() {
    // 1. Create the event loop
    PollEventLoop* loop = poll_loop_create();
    if (!loop) {
        fprintf(stderr, "Failed to create event loop: %s\n", strerror(errno));
        return EXIT_FAILURE;
    }

    // 2. Set up the listening socket
    int listener_fd = setup_listener(PORT);
    if (listener_fd < 0) {
        poll_loop_destroy(loop);
        return EXIT_FAILURE;
    }

    // 3. Register the listener FD with the loop
    // Pass listener_fd as user_data just as an example, though not used in the callback
    if (poll_loop_register_fd(loop, listener_fd, (void*)(intptr_t)listener_fd) != 0) {
        fprintf(stderr, "Failed to register listener FD %d: %s\n", listener_fd, strerror(errno));
        close(listener_fd);
        poll_loop_destroy(loop);
        return EXIT_FAILURE;
    }

    // 4. Set the callback for handling new connections on the listener FD
    if (poll_loop_set_callback(loop, listener_fd, EVENT_READ, handle_new_connection) != 0) {
         fprintf(stderr, "Failed to set callback for listener FD %d: %s\n", listener_fd, strerror(errno));
         // Don't strictly need to unregister listener here, destroy will handle it
         close(listener_fd);
         poll_loop_destroy(loop);
         return EXIT_FAILURE;
    }

    // 5. Run the event loop
    printf("Starting event loop...\n");
    // Use -1 timeout for poll_loop_wait to block indefinitely until events
    int run_result = poll_loop_run(loop, -1);

    if (run_result < 0) {
        fprintf(stderr, "Event loop exited with error: %s\n", strerror(errno));
    } else {
         printf("Event loop stopped gracefully.\n");
    }

    // 6. Cleanup
    printf("Cleaning up...\n");
    // Note: Client FDs should have been closed and unregistered by their callbacks.
    // We still need to close the listener FD.
    close(listener_fd);
    poll_loop_destroy(loop); // This cleans up remaining internal state

    printf("Server shut down.\n");
    return (run_result == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}