#include <sys/fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/errno.h>

#include "poll_loop.h" // Include the library header
#include "tcp_to_stdout.h"



/**
 * @brief Sets a file descriptor to non-blocking mode.
 * @param fd The file descriptor.
 * @return 0 on success, -1 on error.
 */
int make_non_blocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) {
        perror("fcntl(F_GETFL)");
        return -1;
    }
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
        perror("fcntl(F_SETFL)");
        return -1;
    }
    return 0;
}

/**
 * @brief Creates, binds, and listens on a TCP socket.
 * @param port The port number to listen on.
 * @return The listening file descriptor on success, -1 on error.
 */
int setup_listener(uint16_t port) {
    int listener_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listener_fd < 0) {
        perror("socket creation failed");
        return -1;
    }

    // Allow reuse of local addresses
    int opt = 1;
    if (setsockopt(listener_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt(SO_REUSEADDR) failed");
        close(listener_fd);
        return -1;
    }

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY; // Listen on all interfaces
    server_addr.sin_port = htons(port);

    if (bind(listener_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind failed");
        close(listener_fd);
        return -1;
    }

    if (listen(listener_fd, BACKLOG) < 0) {
        perror("listen failed");
        close(listener_fd);
        return -1;
    }

    // Make listener non-blocking (optional but good practice)
    if (make_non_blocking(listener_fd) == -1) {
         close(listener_fd);
         return -1;
    }


    printf("Server listening on port %d\n", port);
    return listener_fd;
}

// --- Callback Implementations ---

/**
 * @brief Callback triggered when the listening socket has incoming connections.
 */
void handle_new_connection(PollEventLoop* loop, int listener_fd, void* user_data) {
    (void)user_data; // Unused in this simple example
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);

    int client_fd = accept(listener_fd, (struct sockaddr*)&client_addr, &client_len);

    if (client_fd < 0) {
        // EAGAIN/EWOULDBLOCK means no connection ready right now, which shouldn't
        // happen if poll reported POLLIN, but handle defensively.
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            fprintf(stderr, "accept returned EAGAIN/EWOULDBLOCK unexpectedly\n");
        } else {
            perror("accept failed");
        }
        return; // Cannot accept this connection
    }

    char client_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, INET_ADDRSTRLEN);
    printf("Accepted connection from %s:%d (FD: %d)\n", client_ip, ntohs(client_addr.sin_port), client_fd);

    // Make the new client socket non-blocking IMPORTANT for event loops!
    if (make_non_blocking(client_fd) == -1) {
        fprintf(stderr, "Failed to make client FD %d non-blocking\n", client_fd);
        close(client_fd);
        return;
    }

    // Register the new client FD with the event loop
    // Pass NULL as user_data for simplicity, could pass a client struct pointer
    if (poll_loop_register_fd(loop, client_fd, NULL) != 0) {
        fprintf(stderr, "Failed to register client FD %d: %s\n", client_fd, strerror(errno));
        close(client_fd);
        return;
    }

    // Set callbacks for the client FD
    // Read callback: handle incoming data
    if (poll_loop_set_callback(loop, client_fd, EVENT_READ, handle_client_read) != 0) {
        fprintf(stderr, "Failed to set read callback for client FD %d: %s\n", client_fd, strerror(errno));
        // Need to unregister too before closing
        poll_loop_unregister_fd(loop, client_fd);
        close(client_fd);
        return;
    }
    // Hangup/Error callback: handle disconnection
    if (poll_loop_set_callback(loop, client_fd, EVENT_HUP, handle_client_close) != 0) {
        fprintf(stderr, "Failed to set hup callback for client FD %d: %s\n", client_fd, strerror(errno));
        poll_loop_unregister_fd(loop, client_fd);
        close(client_fd);
        return;
    }
     if (poll_loop_set_callback(loop, client_fd, EVENT_ERROR, handle_client_close) != 0) {
        fprintf(stderr, "Failed to set error callback for client FD %d: %s\n", client_fd, strerror(errno));
        poll_loop_unregister_fd(loop, client_fd);
        close(client_fd);
        return;
    }
}

/**
 * @brief Callback triggered when a client socket has data to read (POLLIN).
 */
void handle_client_read(PollEventLoop* loop, int client_fd, void* user_data) {
    (void)user_data; // Unused
    char buffer[READ_BUFFER_SIZE];
    ssize_t bytes_read;

    bytes_read = read(client_fd, buffer, sizeof(buffer) - 1); // Leave space for null terminator

    if (bytes_read > 0) {
        // Data received, print it to stdout
        buffer[bytes_read] = '\0'; // Null-terminate the buffer
        printf("Received from FD %d: %s", client_fd, buffer);
        // Could add write logic here for a true echo server (register for POLLOUT etc.)
    } else if (bytes_read == 0) {
        // Connection closed gracefully by the client (EOF)
        printf("Client FD %d disconnected gracefully.\n", client_fd);
        poll_loop_unregister_fd(loop, client_fd); // Remove from loop
        close(client_fd);                       // Close the socket
    } else {
        // Read error
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            // This is expected for non-blocking sockets when no data is available
            // We were woken up, but data was consumed before we read it? Or spurious wakeup?
            // No action needed, just wait for the next POLLIN.
        } else {
            // Actual error
            fprintf(stderr, "Error reading from client FD %d: %s\n", client_fd, strerror(errno));
            poll_loop_unregister_fd(loop, client_fd); // Remove from loop
            close(client_fd);                       // Close the socket
        }
    }
}

/**
 * @brief Callback triggered when a client socket has HUP or ERR events.
 */
void handle_client_close(PollEventLoop* loop, int client_fd, void* user_data) {
     (void)user_data; // Unused
     // We might get HUP/ERR slightly before or after read returns 0 or -1.
     // Unregistering might fail if handle_client_read already did it.
     printf("Hangup or Error detected on client FD %d. Closing.\n", client_fd);
     int unreg_ret = poll_loop_unregister_fd(loop, client_fd);
     if (unreg_ret != 0 && errno != ENOENT) {
        // Log if unregister failed for a reason other than "already gone"
        fprintf(stderr, "Error unregistering closing client FD %d: %s\n", client_fd, strerror(errno));
     }
     // Ensure close happens even if unregister failed or was already done
     close(client_fd);
}