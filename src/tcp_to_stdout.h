#include "poll_loop.h" // Include the library header

#define PORT 8080
#define BACKLOG 10      // Max pending connections for listen()
#define READ_BUFFER_SIZE 1024

// Forward declarations of callback functions
void handle_new_connection(PollEventLoop* loop, int listener_fd, void* user_data);
void handle_client_read(PollEventLoop* loop, int client_fd, void* user_data);
void handle_client_close(PollEventLoop* loop, int client_fd, void* user_data); // For HUP/ERR events

// Function to set up a TCP listener socket
int setup_listener(uint16_t port);