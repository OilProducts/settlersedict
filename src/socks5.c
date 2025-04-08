#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <signal.h> // For signal handling
#include <stdint.h> // For uint8_t, uint16_t
#include <netdb.h>  // For getaddrinfo (though we start with IPv4)

#include "src/poll_loop.h" // Your library header

#define PROXY_PORT 9050
#define BUFFER_SIZE 4096
#define MAX_CONNECTIONS 100 // Simple limit

// SOCKS5 Constants
#define SOCKS_VERSION 0x05
#define SOCKS_AUTH_NONE 0x00
#define SOCKS_CMD_CONNECT 0x01
#define SOCKS_ATYP_IPV4 0x01
#define SOCKS_ATYP_DOMAIN 0x03 // We won't implement domain resolution initially
#define SOCKS_ATYP_IPV6 0x04 // We won't implement IPv6 initially
#define SOCKS_REP_SUCCESS 0x00
#define SOCKS_REP_GENERAL_FAILURE 0x01
#define SOCKS_REP_CONN_NOT_ALLOWED 0x02 // Not used here
#define SOCKS_REP_NET_UNREACHABLE 0x03
#define SOCKS_REP_HOST_UNREACHABLE 0x04
#define SOCKS_REP_CONN_REFUSED 0x05
#define SOCKS_REP_TTL_EXPIRED 0x06 // Not used here
#define SOCKS_REP_CMD_NOT_SUPPORTED 0x07
#define SOCKS_REP_ATYP_NOT_SUPPORTED 0x08


// Connection States
typedef enum {
    STATE_INITIAL,             // Just accepted
    STATE_NEGOTIATING_METHODS, // Waiting for client method list
    STATE_WAITING_REQUEST,     // Sent method choice, waiting for command request
    STATE_CONNECTING_REMOTE,   // Non-blocking connect to destination in progress
    STATE_RELAYING,            // Client <-> Remote data transfer
    STATE_CLOSING              // Shutting down
} ConnStateEnum;

// Simple buffer structure
typedef struct {
    unsigned char buffer[BUFFER_SIZE];
    size_t len;  // Amount of data currently in buffer
    size_t pos;  // Position of next byte to read/write from buffer
} DataBuffer;

// State for a single client <-> remote connection pair
typedef struct {
    PollEventLoop* loop;
    int client_fd;
    int remote_fd;
    ConnStateEnum state;
    DataBuffer client_to_remote_buf; // Data read from client, to be written to remote
    DataBuffer remote_to_client_buf; // Data read from remote, to be written to client
    struct sockaddr_in dest_addr;   // Only storing IPv4 for now
    // Add other fields as needed (e.g., target host/port before DNS lookup)
} Connection;

// --- Forward Declarations ---
static void cleanup_connection(Connection* conn, const char* reason);
static void client_read_cb(PollEventLoop* loop, int fd, void* user_data);
static void client_write_cb(PollEventLoop* loop, int fd, void* user_data);
static void remote_read_cb(PollEventLoop* loop, int fd, void* user_data);
static void remote_write_cb(PollEventLoop* loop, int fd, void* user_data);
static void remote_connect_cb(PollEventLoop* loop, int fd, void* user_data);
static void error_hup_cb(PollEventLoop* loop, int fd, void* user_data);
static void accept_cb(PollEventLoop* loop, int fd, void* user_data);
static void handle_sigint(int sig);

// --- Global variable for the loop (for signal handler) ---
static PollEventLoop* g_loop = NULL;

// --- Utility Functions ---

// Set FD to non-blocking
static int make_non_blocking(int fd) {
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

/*
 * Creates a listening socket on the specified port.
 * Returns the file descriptor for the listening socket, or -1 on error.
 */
static int create_listener(uint16_t port) {
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("socket");
        return -1;
    }

    int reuse = 1;
    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        perror("setsockopt(SO_REUSEADDR)");
        close(listen_fd);
        return -1;
    }

    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    serv_addr.sin_port = htons(port);

    if (bind(listen_fd, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
        perror("bind");
        close(listen_fd);
        return -1;
    }

    if (listen(listen_fd, SOMAXCONN) < 0) {
        perror("listen");
        close(listen_fd);
        return -1;
    }

    if (make_non_blocking(listen_fd) < 0) {
        close(listen_fd);
        return -1;
    }

    printf("SOCKS5 proxy listening on port %d\n", port);
    return listen_fd;
}

// Send a SOCKS5 reply
// Returns 0 on success queueing, -1 on error
static int send_socks_reply(Connection* conn, uint8_t reply_code, uint8_t atyp, const struct sockaddr* bind_addr) {
    if (conn->state == STATE_CLOSING) return 0; // Don't send if already closing

    unsigned char reply[10]; // Max size for IPv4 reply: VER+REP+RSV+ATYP+ADDR(4)+PORT(2)
    size_t reply_len = 4; // VER, REP, RSV, ATYP

    reply[0] = SOCKS_VERSION;
    reply[1] = reply_code;
    reply[2] = 0x00; // Reserved
    reply[3] = atyp; // Typically SOCKS_ATYP_IPV4

    struct sockaddr_in default_addr;
    if (!bind_addr) {
         // If no bind_addr provided, send 0.0.0.0:0
        memset(&default_addr, 0, sizeof(default_addr));
        default_addr.sin_family = AF_INET;
        default_addr.sin_addr.s_addr = htonl(INADDR_ANY);
        default_addr.sin_port = htons(0);
        bind_addr = (struct sockaddr*)&default_addr;
    }


    if (atyp == SOCKS_ATYP_IPV4 && bind_addr->sa_family == AF_INET) {
        const struct sockaddr_in* sin = (const struct sockaddr_in*)bind_addr;
        memcpy(&reply[reply_len], &sin->sin_addr, 4);
        reply_len += 4;
        memcpy(&reply[reply_len], &sin->sin_port, 2);
        reply_len += 2;
    } else {
        // Handle other ATYPs (Domain, IPv6) or send default 0.0.0.0:0 if mismatch
        reply[3] = SOCKS_ATYP_IPV4; // Force IPv4 reply type
        memset(&reply[reply_len], 0, 6); // 0.0.0.0:0
        reply_len += 6;
    }


    // Check if there's space in the outgoing buffer (remote_to_client)
    if (BUFFER_SIZE - conn->remote_to_client_buf.len < reply_len) {
        fprintf(stderr, "[FD:%d] Error: No buffer space for SOCKS reply.\n", conn->client_fd);
        cleanup_connection(conn, "reply buffer full");
        return -1;
    }

    // Append reply to the buffer
    memcpy(conn->remote_to_client_buf.buffer + conn->remote_to_client_buf.len, reply, reply_len);
    conn->remote_to_client_buf.len += reply_len;

    // Ensure write callback is set for the client FD
    // Note: Due to deferred updates, this might take effect in the next loop iteration
    if (poll_loop_set_callback(conn->loop, conn->client_fd, EVENT_WRITE, client_write_cb) != 0) {
         fprintf(stderr, "[FD:%d] Error setting write callback for reply: %s\n", conn->client_fd, strerror(errno));
         cleanup_connection(conn, "failed setting write callback");
         return -1;
    }

    printf("[FD:%d] Queued SOCKS reply: Code=%d\n", conn->client_fd, reply_code);
    return 0;
}


// --- Callback Implementations ---

// Handles new client connections
static void accept_cb(PollEventLoop* loop, int listen_fd, void* user_data) {
    struct sockaddr_in cli_addr;
    socklen_t cli_len = sizeof(cli_addr);
    int client_fd = accept(listen_fd, (struct sockaddr*)&cli_addr, &cli_len);

    if (client_fd < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            // Expected for non-blocking accept
            return;
        } else {
            perror("accept");
            return; // Continue loop, maybe transient error
        }
    }

    char client_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &cli_addr.sin_addr, client_ip, sizeof(client_ip));
    printf("Accepted connection from %s:%d (FD: %d)\n", client_ip, ntohs(cli_addr.sin_port), client_fd);


    if (make_non_blocking(client_fd) < 0) {
        close(client_fd);
        return;
    }

    // Create connection state
    Connection* conn = (Connection*)malloc(sizeof(Connection));
    if (!conn) {
        fprintf(stderr, "Failed to allocate memory for connection state\n");
        close(client_fd);
        return;
    }
    memset(conn, 0, sizeof(Connection));
    conn->loop = loop;
    conn->client_fd = client_fd;
    conn->remote_fd = -1; // Not connected yet
    conn->state = STATE_INITIAL; // Start in initial state, move to negotiating
    conn->client_to_remote_buf.len = 0; conn->client_to_remote_buf.pos = 0;
    conn->remote_to_client_buf.len = 0; conn->remote_to_client_buf.pos = 0;


    // Register client FD with the loop, using 'conn' as user_data
    if (poll_loop_register_fd(loop, client_fd, conn) != 0) {
        fprintf(stderr, "[FD:%d] Failed to register client FD: %s\n", client_fd, strerror(errno));
        free(conn);
        close(client_fd);
        return;
    }

    // Set read callback to start SOCKS negotiation
    if (poll_loop_set_callback(loop, client_fd, EVENT_READ, client_read_cb) != 0) {
        fprintf(stderr, "[FD:%d] Failed set read callback: %s\n", client_fd, strerror(errno));
        // Unregister (deferred) and cleanup
        poll_loop_unregister_fd(loop, client_fd);
        close(client_fd);
        free(conn);
        return;
    }
     // Also watch for errors/hangups
    if (poll_loop_set_callback(loop, client_fd, EVENT_ERROR, error_hup_cb) != 0 ||
        poll_loop_set_callback(loop, client_fd, EVENT_HUP, error_hup_cb) != 0) {
         fprintf(stderr, "[FD:%d] Failed set error/hup callback: %s\n", client_fd, strerror(errno));
        poll_loop_unregister_fd(loop, client_fd);
        close(client_fd);
        free(conn);
        return;
    }

    conn->state = STATE_NEGOTIATING_METHODS; // Ready for client's first message
    printf("[FD:%d] Registered, waiting for SOCKS method negotiation.\n", client_fd);
}

// Handles reading data FROM the client
static void client_read_cb(PollEventLoop* loop, int fd, void* user_data) {
    Connection* conn = (Connection*)user_data;
    if (conn->state == STATE_CLOSING) return;

    unsigned char* buffer = NULL;
    size_t buf_avail = 0;
    ssize_t nread = 0;


    if (conn->state == STATE_RELAYING) {
         // Read directly into the client_to_remote buffer if relaying
         buffer = conn->client_to_remote_buf.buffer + conn->client_to_remote_buf.len;
         buf_avail = BUFFER_SIZE - conn->client_to_remote_buf.len;
         if (buf_avail == 0) {
            fprintf(stderr, "[FD:%d] Client -> Remote buffer full, cannot read more.\n", fd);
            // Stop reading from client until remote buffer drains
             poll_loop_set_callback(loop, fd, EVENT_READ, NULL); // Disable reading temporarily
             // Write callback for remote should already be set if buffer was filling
            return;
         }
         nread = read(fd, buffer, buf_avail);
    } else {
         // During handshake, use a temporary buffer to parse protocol messages
         unsigned char temp_buf[512]; // Sufficient for handshake messages
         nread = read(fd, temp_buf, sizeof(temp_buf));
         buffer = temp_buf; // Point buffer to temp_buf for parsing logic below
    }

    // --- Handle read results ---
    if (nread > 0) {
        printf("[FD:%d] Read %zd bytes from client.\n", fd, nread);

        if (conn->state == STATE_NEGOTIATING_METHODS) {
            // Parse Version/Methods: [VER | NMETHODS | METHODS...]
            if (nread < 3 || buffer[0] != SOCKS_VERSION) {
                 fprintf(stderr, "[FD:%d] Invalid SOCKS version or short method request.\n", fd);
                 cleanup_connection(conn, "invalid method request");
                 return;
            }
            uint8_t nmethods = buffer[1];
            if (nread < (size_t)(2 + nmethods)) {
                 fprintf(stderr, "[FD:%d] Incomplete method list received.\n", fd);
                 cleanup_connection(conn, "incomplete method list");
                 return;
            }

            // Check if "No Authentication" (0x00) is supported by client
            int found_no_auth = 0;
            for (int i = 0; i < nmethods; ++i) {
                if (buffer[2 + i] == SOCKS_AUTH_NONE) {
                    found_no_auth = 1;
                    break;
                }
            }

            if (!found_no_auth) {
                 fprintf(stderr, "[FD:%d] Client does not support 'No Authentication'.\n", fd);
                 // Send method rejection [VER | 0xFF] - although not strictly required
                 unsigned char reject_reply[] = { SOCKS_VERSION, 0xFF };
                 // Quick write - ideally queue this too, but simpler for error path
                 write(fd, reject_reply, sizeof(reject_reply));
                 cleanup_connection(conn, "no supported auth method");
                 return;
            }

            // Send method selection reply [VER | METHOD]
            unsigned char method_reply[] = { SOCKS_VERSION, SOCKS_AUTH_NONE };
            // Queue the reply in the remote_to_client buffer
             if (BUFFER_SIZE - conn->remote_to_client_buf.len < sizeof(method_reply)) {
                 fprintf(stderr, "[FD:%d] No buffer space for method reply.\n", fd);
                 cleanup_connection(conn, "method reply buffer full");
                 return;
             }
             memcpy(conn->remote_to_client_buf.buffer + conn->remote_to_client_buf.len, method_reply, sizeof(method_reply));
             conn->remote_to_client_buf.len += sizeof(method_reply);

             // Ensure write callback is set for client to send the reply
             if (poll_loop_set_callback(loop, fd, EVENT_WRITE, client_write_cb) != 0) {
                 fprintf(stderr, "[FD:%d] Failed to set write callback for method reply: %s\n", fd, strerror(errno));
                 cleanup_connection(conn, "failed setting write cb for method");
                 return;
             }
             conn->state = STATE_WAITING_REQUEST;
             printf("[FD:%d] Method negotiation complete (No Auth). Waiting for request.\n", fd);
             // Note: We might have read more than needed (e.g., part of the next request).
             // A robust implementation would handle leftover data in buffer. This basic one assumes separate reads.

        } else if (conn->state == STATE_WAITING_REQUEST) {
            // Parse Command Request: [VER | CMD | RSV | ATYP | DST.ADDR | DST.PORT]
            if (nread < 7) { // Minimum length for IPv4: 1+1+1+1+4+2 = 10, but check header first
                fprintf(stderr, "[FD:%d] Request too short.\n", fd);
                cleanup_connection(conn, "request too short");
                return;
            }
            if (buffer[0] != SOCKS_VERSION || buffer[1] != SOCKS_CMD_CONNECT) {
                fprintf(stderr, "[FD:%d] Invalid version or unsupported command (%d).\n", fd, buffer[1]);
                 send_socks_reply(conn, SOCKS_REP_CMD_NOT_SUPPORTED, SOCKS_ATYP_IPV4, NULL);
                cleanup_connection(conn, "unsupported command");
                return;
            }
            uint8_t atyp = buffer[3];
            uint16_t dest_port = 0;
            struct sockaddr_in dest_addr_in; // Use sockaddr_in for IPv4

            memset(&dest_addr_in, 0, sizeof(dest_addr_in));
            dest_addr_in.sin_family = AF_INET;

            if (atyp == SOCKS_ATYP_IPV4) {
                if (nread < 10) { // 1+1+1+1+4+2
                    fprintf(stderr, "[FD:%d] Incomplete IPv4 request.\n", fd);
                    cleanup_connection(conn, "incomplete ipv4 request");
                    return;
                }
                memcpy(&dest_addr_in.sin_addr, &buffer[4], 4);
                memcpy(&dest_port, &buffer[8], 2);
                dest_addr_in.sin_port = dest_port; // Already in network byte order
                 conn->dest_addr = dest_addr_in; // Store it
                 printf("[FD:%d] Request: CONNECT to %s:%d\n", fd, inet_ntoa(dest_addr_in.sin_addr), ntohs(dest_port));

            }
            // *** Add Domain Name (ATYP=3) or IPv6 (ATYP=4) handling here if needed ***
            /* else if (atyp == SOCKS_ATYP_DOMAIN) { ... handle domain lookup ... } */
            /* else if (atyp == SOCKS_ATYP_IPV6) { ... handle IPv6 ... } */
             else {
                fprintf(stderr, "[FD:%d] Unsupported address type: %d\n", fd, atyp);
                send_socks_reply(conn, SOCKS_REP_ATYP_NOT_SUPPORTED, SOCKS_ATYP_IPV4, NULL);
                cleanup_connection(conn, "unsupported atyp");
                return;
            }

            // --- Attempt to connect to destination ---
            int remote_fd = socket(AF_INET, SOCK_STREAM, 0);
            if (remote_fd < 0) {
                perror("socket (remote)");
                send_socks_reply(conn, SOCKS_REP_GENERAL_FAILURE, SOCKS_ATYP_IPV4, NULL);
                cleanup_connection(conn, "remote socket failed");
                return;
            }

            if (make_non_blocking(remote_fd) < 0) {
                close(remote_fd);
                send_socks_reply(conn, SOCKS_REP_GENERAL_FAILURE, SOCKS_ATYP_IPV4, NULL);
                cleanup_connection(conn, "remote fcntl failed");
                return;
            }

            conn->remote_fd = remote_fd; // Store the FD

            printf("[FD:%d] Connecting to destination (Remote FD: %d)...\n", fd, remote_fd);
            int ret = connect(remote_fd, (struct sockaddr*)&dest_addr_in, sizeof(dest_addr_in));

            if (ret == 0) {
                 // Connected immediately (unlikely but possible)
                 printf("[FD:%d -> %d] Connected immediately.\n", fd, remote_fd);

                 // Send success reply
                 struct sockaddr_in bound_addr; // Get local address client is connected to
                 socklen_t len = sizeof(bound_addr);
                 if (getsockname(conn->client_fd, (struct sockaddr*)&bound_addr, &len) == -1) {
                     perror("getsockname");
                     // Send success but with 0.0.0.0:0
                     send_socks_reply(conn, SOCKS_REP_SUCCESS, SOCKS_ATYP_IPV4, NULL);
                 } else {
                     send_socks_reply(conn, SOCKS_REP_SUCCESS, SOCKS_ATYP_IPV4, (struct sockaddr*)&bound_addr);
                 }


                 conn->state = STATE_RELAYING;

                 // Register remote FD for reading and errors
                 if (poll_loop_register_fd(loop, remote_fd, conn) != 0 ||
                     poll_loop_set_callback(loop, remote_fd, EVENT_READ, remote_read_cb) != 0 ||
                     poll_loop_set_callback(loop, remote_fd, EVENT_ERROR, error_hup_cb) != 0 ||
                     poll_loop_set_callback(loop, remote_fd, EVENT_HUP, error_hup_cb) != 0 )
                 {
                     fprintf(stderr, "[FD:%d] Failed register/set callbacks for remote FD %d: %s\n", fd, remote_fd, strerror(errno));
                     cleanup_connection(conn, "remote cb setup failed");
                     return;
                 }
                 printf("[FD:%d <-> %d] Now relaying.\n", fd, remote_fd);


            } else if (errno == EINPROGRESS) {
                 // Connection attempt is in progress
                 printf("[FD:%d -> %d] Connection in progress...\n", fd, remote_fd);
                 conn->state = STATE_CONNECTING_REMOTE;

                 // Register remote FD and set write callback to detect connection completion
                 // Also watch for errors/hangups during connect
                 if (poll_loop_register_fd(loop, remote_fd, conn) != 0 ||
                     poll_loop_set_callback(loop, remote_fd, EVENT_WRITE, remote_connect_cb) != 0 ||
                     poll_loop_set_callback(loop, remote_fd, EVENT_ERROR, error_hup_cb) != 0 ||
                     poll_loop_set_callback(loop, remote_fd, EVENT_HUP, error_hup_cb) != 0)
                  {
                    fprintf(stderr, "[FD:%d] Failed register/set callbacks for remote FD %d: %s\n", fd, remote_fd, strerror(errno));
                    cleanup_connection(conn, "remote connect cb setup failed");
                    return;
                }

            } else {
                 // Connection failed immediately
                 perror("connect (remote)");
                 uint8_t err_reply = SOCKS_REP_GENERAL_FAILURE;
                 if (errno == EHOSTUNREACH) err_reply = SOCKS_REP_HOST_UNREACHABLE;
                 else if (errno == ENETUNREACH) err_reply = SOCKS_REP_NET_UNREACHABLE;
                 else if (errno == ECONNREFUSED) err_reply = SOCKS_REP_CONN_REFUSED;

                 send_socks_reply(conn, err_reply, SOCKS_ATYP_IPV4, NULL);
                 // Cleanup will close the remote_fd
                 cleanup_connection(conn, "remote connect failed");
                 return;
            }
        } else if (conn->state == STATE_RELAYING) {
            // Data read from client, intended for remote
            conn->client_to_remote_buf.len += nread;
            // Ensure write callback is set for remote end
            if (conn->remote_fd != -1) {
                if (poll_loop_set_callback(loop, conn->remote_fd, EVENT_WRITE, remote_write_cb) != 0) {
                     fprintf(stderr, "[FD:%d] Failed set remote write callback: %s\n", fd, strerror(errno));
                     cleanup_connection(conn, "failed setting remote write cb");
                     return;
                }
            } else {
                 // Should not happen in relaying state
                 fprintf(stderr, "[FD:%d] Error: Remote FD invalid in relaying state.\n", fd);
                 cleanup_connection(conn, "invalid remote fd in relay");
                 return;
            }
        }
    } else if (nread == 0) {
        // Client closed connection (EOF)
        printf("[FD:%d] Client closed connection (EOF).\n", fd);
        cleanup_connection(conn, "client EOF");
        return;
    } else { // nread < 0
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            // No data available right now, try again later
            // The loop will call us again when data is ready
             // printf("[FD:%d] Read EAGAIN/EWOULDBLOCK\n", fd); // Debugging
        } else {
            // Actual read error
            perror("read (client)");
            cleanup_connection(conn, "client read error");
            return;
        }
    }
}

// Handles writing data TO the client
static void client_write_cb(PollEventLoop* loop, int fd, void* user_data) {
    Connection* conn = (Connection*)user_data;
     if (conn->state == STATE_CLOSING) return;

    DataBuffer* buf = &conn->remote_to_client_buf;

    if (buf->len == 0 || buf->pos == buf->len) {
         // Nothing to write, or buffer fully written
         buf->len = 0;
         buf->pos = 0;
        // Disable write callback until there's more data
        poll_loop_set_callback(loop, fd, EVENT_WRITE, NULL);

         // If we just finished sending a failure reply, perform cleanup now
        if (conn->state == STATE_CLOSING) {
             cleanup_connection(conn, "finished sending failure reply");
         }
        return;
    }

    ssize_t nwritten = write(fd, buf->buffer + buf->pos, buf->len - buf->pos);

    if (nwritten > 0) {
         // printf("[FD:%d] Wrote %zd bytes to client.\n", fd, nwritten); // Debugging
        buf->pos += nwritten;
        if (buf->pos == buf->len) {
            // Buffer fully written, reset and disable write notifications
            buf->len = 0;
            buf->pos = 0;
            poll_loop_set_callback(loop, fd, EVENT_WRITE, NULL);
             // If we just finished sending a failure reply, perform cleanup
            if (conn->state == STATE_CLOSING) {
                 cleanup_connection(conn, "finished sending failure reply after write");
             }
             // If we just sent the SOCKS success reply, re-enable reads from remote
             // (in case they were disabled due to buffer full)
             else if (conn->state == STATE_RELAYING && conn->remote_fd != -1) {
                  poll_loop_set_callback(loop, conn->remote_fd, EVENT_READ, remote_read_cb);
             }

        } else {
            // Partial write, remain writable
        }
    } else if (nwritten == 0) {
        // Should not happen with regular sockets?
        fprintf(stderr, "[FD:%d] write returned 0 to client.\n", fd);
    } else { // nwritten < 0
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            // Client cannot accept data now, try again later
             // printf("[FD:%d] Write EAGAIN/EWOULDBLOCK to client\n", fd); // Debugging
             // Write callback remains set
        } else {
            perror("write (client)");
            cleanup_connection(conn, "client write error");
            return;
        }
    }
}

// Callback for when remote connection attempt completes (or fails)
static void remote_connect_cb(PollEventLoop* loop, int remote_fd, void* user_data) {
    Connection* conn = (Connection*)user_data;
    if (conn->state != STATE_CONNECTING_REMOTE) {
        // Should not happen, but safety check
        fprintf(stderr,"[FD:%d] Warning: remote_connect_cb called in unexpected state %d\n", conn->client_fd, conn->state);
         // Might already be closed or relaying if connect was immediate/raced
        return;
    }


    int sock_err = 0;
    socklen_t err_len = sizeof(sock_err);

    // Check socket error status to see if connect() succeeded
    if (getsockopt(remote_fd, SOL_SOCKET, SO_ERROR, &sock_err, &err_len) == -1) {
        perror("getsockopt(SO_ERROR)");
        send_socks_reply(conn, SOCKS_REP_GENERAL_FAILURE, SOCKS_ATYP_IPV4, NULL);
        cleanup_connection(conn, "getsockopt failed");
        return;
    }

    if (sock_err == 0) {
        // Connection successful!
        printf("[FD:%d -> %d] Remote connection established.\n", conn->client_fd, remote_fd);

        // Send SOCKS success reply to client
         struct sockaddr_in bound_addr; // Get local address client is connected to
         socklen_t len = sizeof(bound_addr);
         if (getsockname(conn->client_fd, (struct sockaddr*)&bound_addr, &len) == -1) {
             perror("getsockname");
             send_socks_reply(conn, SOCKS_REP_SUCCESS, SOCKS_ATYP_IPV4, NULL); // Send 0.0.0.0:0
         } else {
             send_socks_reply(conn, SOCKS_REP_SUCCESS, SOCKS_ATYP_IPV4, (struct sockaddr*)&bound_addr);
         }

        conn->state = STATE_RELAYING;

        // Change remote FD monitoring: stop watching for WRITE (connect done), start watching for READ
        poll_loop_set_callback(loop, remote_fd, EVENT_WRITE, NULL); // Disable connect callback
        poll_loop_set_callback(loop, remote_fd, EVENT_READ, remote_read_cb); // Enable read

         // Also ensure client read is enabled (might have been disabled if buffers were full)
         poll_loop_set_callback(loop, conn->client_fd, EVENT_READ, client_read_cb);

        printf("[FD:%d <-> %d] Now relaying.\n", conn->client_fd, remote_fd);

    } else {
        // Connection failed
        fprintf(stderr, "[FD:%d -> %d] Remote connection failed: %s\n", conn->client_fd, remote_fd, strerror(sock_err));
        uint8_t err_reply = SOCKS_REP_GENERAL_FAILURE;
         if (sock_err == EHOSTUNREACH) err_reply = SOCKS_REP_HOST_UNREACHABLE;
         else if (sock_err == ENETUNREACH) err_reply = SOCKS_REP_NET_UNREACHABLE;
         else if (sock_err == ECONNREFUSED) err_reply = SOCKS_REP_CONN_REFUSED;

        send_socks_reply(conn, err_reply, SOCKS_ATYP_IPV4, NULL);
        cleanup_connection(conn, "remote connect failed async");
        return;
    }
}


// Handles reading data FROM the remote server
static void remote_read_cb(PollEventLoop* loop, int fd, void* user_data) {
    Connection* conn = (Connection*)user_data;
     if (conn->state == STATE_CLOSING) return;
     if (conn->state != STATE_RELAYING) {
          fprintf(stderr,"[FD:%d] Warning: remote_read_cb called in unexpected state %d\n", conn->client_fd, conn->state);
          return;
     }

    DataBuffer* buf = &conn->remote_to_client_buf;
    size_t buf_avail = BUFFER_SIZE - buf->len;

    if (buf_avail == 0) {
         fprintf(stderr, "[FD:%d <- %d] Remote -> Client buffer full.\n", conn->client_fd, fd);
          // Stop reading from remote until client buffer drains
         poll_loop_set_callback(loop, fd, EVENT_READ, NULL);
         return;
    }

    ssize_t nread = read(fd, buf->buffer + buf->len, buf_avail);

    if (nread > 0) {
         // printf("[FD:%d <- %d] Read %zd bytes from remote.\n", conn->client_fd, fd, nread); // Debugging
        buf->len += nread;
        // Ensure write callback is set for client FD
        if (poll_loop_set_callback(loop, conn->client_fd, EVENT_WRITE, client_write_cb) != 0) {
             fprintf(stderr, "[FD:%d] Failed set client write callback: %s\n", conn->client_fd, strerror(errno));
             cleanup_connection(conn, "failed setting client write cb");
             return;
        }
    } else if (nread == 0) {
        // Remote server closed connection (EOF)
        printf("[FD:%d <- %d] Remote server closed connection (EOF).\n", conn->client_fd, fd);
        cleanup_connection(conn, "remote EOF");
        return;
    } else { // nread < 0
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            // No data available right now
             // printf("[FD:%d <- %d] Read EAGAIN/EWOULDBLOCK from remote\n", conn->client_fd, fd); // Debugging
        } else {
            perror("read (remote)");
            cleanup_connection(conn, "remote read error");
            return;
        }
    }
}

// Handles writing data TO the remote server
static void remote_write_cb(PollEventLoop* loop, int fd, void* user_data) {
    Connection* conn = (Connection*)user_data;
     if (conn->state == STATE_CLOSING) return;
     if (conn->state != STATE_RELAYING) {
          // Might happen if connect callback hasn't processed failure yet, but data arrived from client
          // Or if cleanup started but write event fires before unregister takes effect.
         fprintf(stderr,"[FD:%d -> %d] Warning: remote_write_cb called in unexpected state %d\n", conn->client_fd, fd, conn->state);
         poll_loop_set_callback(loop, fd, EVENT_WRITE, NULL); // Try to disable further writes
         return;
     }


    DataBuffer* buf = &conn->client_to_remote_buf;

    if (buf->len == 0 || buf->pos == buf->len) {
        buf->len = 0; buf->pos = 0;
        poll_loop_set_callback(loop, fd, EVENT_WRITE, NULL); // Disable write notification
        return;
    }

    ssize_t nwritten = write(fd, buf->buffer + buf->pos, buf->len - buf->pos);

    if (nwritten > 0) {
         // printf("[FD:%d -> %d] Wrote %zd bytes to remote.\n", conn->client_fd, fd, nwritten); // Debugging
        buf->pos += nwritten;
        if (buf->pos == buf->len) {
            // Buffer fully written, reset and disable notifications
            buf->len = 0;
            buf->pos = 0;
            poll_loop_set_callback(loop, fd, EVENT_WRITE, NULL);
             // Since buffer is now empty, we can potentially read more from the client
             poll_loop_set_callback(loop, conn->client_fd, EVENT_READ, client_read_cb);
        } else {
             // Partial write, remain writable
        }
    } else if (nwritten == 0) {
        fprintf(stderr, "[FD:%d -> %d] write returned 0 to remote.\n", conn->client_fd, fd);
    } else { // nwritten < 0
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            // Remote cannot accept data now
             // printf("[FD:%d -> %d] Write EAGAIN/EWOULDBLOCK to remote\n", conn->client_fd, fd); // Debugging
        } else {
            perror("write (remote)");
            cleanup_connection(conn, "remote write error");
            return;
        }
    }
}


// Handles HUP or ERROR events on either socket
static void error_hup_cb(PollEventLoop* loop, int fd, void* user_data) {
    Connection* conn = (Connection*)user_data;

    if (fd == conn->client_fd) {
         printf("[FD:%d] Error/Hup event on client socket.\n", fd);
         cleanup_connection(conn, "client error/hup");
    } else if (fd == conn->remote_fd) {
         printf("[FD:%d <- %d] Error/Hup event on remote socket.\n", conn->client_fd, fd);
         cleanup_connection(conn, "remote error/hup");
    } else {
        // Should not happen
        fprintf(stderr, "Error/Hup event on unexpected FD %d\n", fd);
        // Cannot easily cleanup without conn state if user_data is wrong...
        // Maybe just try unregistering the problem FD directly
        poll_loop_unregister_fd(loop, fd);
        close(fd);
    }
}


// --- Cleanup ---

// Unregister FDs, close them, and free state
// Note: Because of deferred updates, unregister might not happen immediately if
// called from within poll_loop_wait. Closing FDs is immediate.
static void cleanup_connection(Connection* conn, const char* reason) {
    if (conn->state == STATE_CLOSING) {
         // Already cleaning up, avoid recursion or double free
         return;
    }
    printf("[FD:%d] Cleaning up connection. Reason: %s\n", conn->client_fd, reason);
    conn->state = STATE_CLOSING;

    if (conn->client_fd != -1) {
        poll_loop_unregister_fd(conn->loop, conn->client_fd);
        close(conn->client_fd);
        conn->client_fd = -1;
    }
    if (conn->remote_fd != -1) {
        poll_loop_unregister_fd(conn->loop, conn->remote_fd);
        close(conn->remote_fd);
        conn->remote_fd = -1;
    }
    free(conn); // Free the connection state
}
