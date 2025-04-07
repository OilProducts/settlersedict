#include <stdio.h>
#include <stdlib.h>

#include "src/poll_loop.h"
#include "src/socks5.c"

// --- Signal Handling ---
static void handle_sigint(int sig) {
    printf("\nCaught signal %d. Stopping event loop...\n", sig);
    if (g_loop) {
        poll_loop_stop(g_loop);
    }
}


// --- Main Function ---
int main() {
    int listen_fd = create_listener(PROXY_PORT);
    if (listen_fd < 0) {
        return EXIT_FAILURE;
    }

    g_loop = poll_loop_create();
    if (!g_loop) {
        fprintf(stderr, "Failed to create event loop: %s\n", strerror(errno));
        close(listen_fd);
        return EXIT_FAILURE;
    }

    // Register the listening socket
    // Pass NULL user data for the listener itself, or maybe a special marker
    if (poll_loop_register_fd(g_loop, listen_fd, NULL) != 0) {
        fprintf(stderr, "Failed to register listening FD %d: %s\n", listen_fd, strerror(errno));
        poll_loop_destroy(g_loop);
        close(listen_fd);
        return EXIT_FAILURE;
    }

    // Set the accept callback for the listening socket
    if (poll_loop_set_callback(g_loop, listen_fd, EVENT_READ, accept_cb) != 0) {
        fprintf(stderr, "Failed to set accept callback for FD %d: %s\n", listen_fd, strerror(errno));
        poll_loop_destroy(g_loop);
        close(listen_fd);
        return EXIT_FAILURE;
    }

    // Setup signal handler for graceful shutdown
    struct sigaction sa;
    sa.sa_handler = handle_sigint;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0; // Or SA_RESTART? Depends on desired interaction with syscalls
    if (sigaction(SIGINT, &sa, NULL) == -1 || sigaction(SIGTERM, &sa, NULL) == -1) {
        perror("sigaction");
        // Continue, but won't shut down gracefully on signal
    }


    // Run the event loop indefinitely (or until stopped)
    // Using -1 timeout means poll() blocks until events or interruption
    int run_result = poll_loop_run(g_loop, -1);

    if (run_result < 0) {
        fprintf(stderr, "Event loop exited with error: %s\n", strerror(errno));
    } else {
        printf("Event loop stopped normally.\n");
    }

    // --- Cleanup ---
    printf("Shutting down...\n");
    // Note: Active connections might not be fully cleaned up if loop stops abruptly
    // A more robust shutdown would iterate through tracked connections and call cleanup.
    // However, poll_loop_destroy frees its internal structures. User FDs are the main concern.
    poll_loop_unregister_fd(g_loop, listen_fd); // Unregister listener
    close(listen_fd);                          // Close listener
    poll_loop_destroy(g_loop);                 // Destroy loop context
    g_loop = NULL;

    printf("Proxy finished.\n");
    return (run_result == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}