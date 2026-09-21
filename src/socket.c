#define _POSIX_C_SOURCE 200809L
#include "lab.h"

#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int smtp_socket_connect(const char *server, const char *port,
                        char *error, size_t capacity)
{
    struct addrinfo hints = {.ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM};
    struct addrinfo *addresses;
    int status = getaddrinfo(server, port, &hints, &addresses);
    if (status != 0) {
        snprintf(error, capacity, "Cannot resolve %s:%s: %s", server, port,
                 gai_strerror(status));
        return -1;
    }
    int fd = -1;
    int saved_error = ECONNREFUSED;
    for (struct addrinfo *address = addresses; address != NULL; address = address->ai_next) {
        fd = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (fd < 0) { /* GCOVR_EXCL_START: socket resource/system failure */
            saved_error = errno;
            continue;
        } /* GCOVR_EXCL_STOP */
        if (connect(fd, address->ai_addr, address->ai_addrlen) == 0) {
            break;
        }
        saved_error = errno;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(addresses);
    if (fd < 0) {
        snprintf(error, capacity, "Cannot connect to %s:%s: %s", server, port,
                 strerror(saved_error));
    }
    return fd;
}

ssize_t smtp_socket_read(void *context, void *buffer, size_t size)
{
    int fd = *(int *)context;
    ssize_t count;
    do {
        count = recv(fd, buffer, size, 0);
    } while (count < 0 && errno == EINTR); /* GCOVR_EXCL_BR_LINE: interrupted recv */
    return count;
}

ssize_t smtp_socket_write(void *context, const void *buffer, size_t size)
{
    int fd = *(int *)context;
    ssize_t count;
    do {
        count = send(fd, buffer, size, MSG_NOSIGNAL);
    } while (count < 0 && errno == EINTR); /* GCOVR_EXCL_BR_LINE: interrupted send */
    return count;
}
