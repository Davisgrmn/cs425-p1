#ifndef LAB_H
#define LAB_H

#include <stddef.h>
#include <sys/types.h>

/* Layer 1: pure protocol helpers. Returned strings belong to the caller and
 * must be freed. NULL means invalid input or allocation failure. */
typedef enum { SMTP_HELO, SMTP_MAIL, SMTP_RCPT, SMTP_DATA, SMTP_QUIT } smtp_command;

/* True for a non-NULL string with no CR or LF (the empty string is valid). */
int smtp_valid_field(const char *text);
/* Reply helpers take a line WITHOUT CRLF. Invalid syntax returns -1.
 * A bare three-digit code is accepted, as recommended by RFC 5321, 4.2. */
int smtp_reply_code(const char *line);
int smtp_reply_final(const char *line); /* 1 final, 0 continuation, -1 invalid */
char *smtp_build_command(smtp_command command, const char *argument);
/* Normalize CR, LF and CRLF to CRLF, escape leading dots, and terminate a
 * nonempty final line. An empty body remains empty. */
char *smtp_dot_stuff(const char *body);
char *smtp_build_data(const char *from, const char *to,
                      const char *subject, const char *body);

/* Layer 2: session over a replaceable byte transport. Callbacks follow
 * read/write semantics: positive byte count, 0 for EOF/no progress, or -1
 * with errno set. They must never return more than the requested size.
 * Initialize a session once per connection. All pointer arguments to this
 * layer must be valid; strings passed to write/command_reply are wire-ready. */
#define SMTP_LINE_SIZE 513 /* RFC 5321: 512 octets including CRLF, plus NUL */
#define SMTP_ERROR_SIZE 1024
typedef ssize_t (*smtp_read_fn)(void *context, void *buffer, size_t size);
typedef ssize_t (*smtp_write_fn)(void *context, const void *buffer, size_t size);
typedef struct {
    smtp_read_fn read;
    smtp_write_fn write;
    void *context;
} smtp_transport;

typedef struct {
    smtp_transport transport;
    char input[1024];
    size_t next;
    size_t end;
    char error[SMTP_ERROR_SIZE];
} smtp_session;

void smtp_session_init(smtp_session *session, smtp_transport transport);
/* All session operations return 0 on success, -1 on failure and put a useful
 * diagnostic in session->error. After a failure, abandon the connection.
 * read_line strips CRLF; read_reply keeps its final line in the output. */
int smtp_read_line(smtp_session *session, char *line, size_t capacity);
int smtp_read_reply(smtp_session *session, int *code, char line[SMTP_LINE_SIZE]);
int smtp_write_all(smtp_session *session, const char *text);
int smtp_command_reply(smtp_session *session, const char *command, int expected);
int smtp_run(smtp_session *session, const char *helo, const char *from,
             const char *to, const char *subject, const char *body);

/* Layer 3: POSIX socket transport. connect returns a descriptor or -1 and
 * fills error (capacity must be positive). The caller owns/closes the fd.
 * Callback context points to that descriptor, which must outlive the session.
 * The callbacks retry interrupted recv/send calls and suppress SIGPIPE. */
int smtp_socket_connect(const char *server, const char *port,
                        char *error, size_t capacity);
ssize_t smtp_socket_read(void *context, void *buffer, size_t size);
ssize_t smtp_socket_write(void *context, const void *buffer, size_t size);

#endif
