#define _POSIX_C_SOURCE 200809L
#include "lab.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#ifdef TEST
#define main main_exclude
#endif

static void usage(FILE *stream)
{
    fprintf(stream,
            "Usage: myapp -f <from> -t <to> [-s subject] [-b body] [-p port]\n"
            "          [-H helo-host] <server>\n"
            "  -f <from>       envelope sender\n"
            "  -t <to>         envelope recipient\n"
            "  -s <subject>    subject line (default: empty)\n"
            "  -b <body>       message body (default: read from stdin)\n"
            "  -p <port>       port or service name (default: 25)\n"
            "  -H <helo-host>  host name sent with HELO (default: localhost)\n"
            "  <server>        host name or address of the mail server\n");
}

static char *read_body(void)
{
    /* getdelim grows its buffer safely, including for long input lines. */
    char *body = NULL;
    size_t capacity = 0;
    ssize_t length = getdelim(&body, &capacity, '\0', stdin);
    if (ferror(stdin) || (length < 0 && !feof(stdin))) {
        fprintf(stderr, "Cannot read message body from stdin\n");
        free(body);
        return NULL;
    }
    if (length > 0 && body[length - 1] == '\0') {
        fprintf(stderr, "Message body must be text without NUL bytes\n");
        free(body);
        return NULL;
    }
    if (length < 0) {
        /* Even empty stdin is a valid empty body. */
        free(body);
        body = calloc(1, 1);
        if (body == NULL) {
            fprintf(stderr, "Cannot allocate message body\n");
        }
    }
    return body;
}

int main(int argc, char **argv)
{
    if (argc == 1) {
        usage(stdout);
        return 0;
    }
    const char *from = NULL, *to = NULL, *subject = "", *body = NULL;
    const char *port = "25", *helo = "localhost";
    int option;
    while ((option = getopt(argc, argv, "f:t:s:b:p:H:")) != -1) {
        switch (option) {
        case 'f': from = optarg; break;
        case 't': to = optarg; break;
        case 's': subject = optarg; break;
        case 'b': body = optarg; break;
        case 'p': port = optarg; break;
        case 'H': helo = optarg; break;
        default: usage(stderr); return 1;
        }
    }
    if (!smtp_valid_field(from) || !smtp_valid_field(to) ||
        !smtp_valid_field(subject) || !smtp_valid_field(helo) ||
        *from == '\0' || *to == '\0' || *helo == '\0' || *port == '\0' ||
        optind != argc - 1 || argv[optind][0] == '\0') {
        fprintf(stderr, "Invalid arguments: supply sender, recipient and one server; "
                        "addresses, subject and HELO must not contain CR or LF.\n");
        usage(stderr);
        return 1;
    }
    char *owned_body = NULL;
    if (body == NULL) {
        owned_body = read_body();
        if (owned_body == NULL) {
            return 2;
        }
        body = owned_body;
    }
    char error[SMTP_ERROR_SIZE];
    int fd = smtp_socket_connect(argv[optind], port, error, sizeof(error));
    if (fd < 0) {
        fprintf(stderr, "%s\n", error);
        free(owned_body);
        return 2;
    }
    smtp_session session;
    smtp_transport transport = {smtp_socket_read, smtp_socket_write, &fd};
    smtp_session_init(&session, transport);
    int result = smtp_run(&session, helo, from, to, subject, body);
    if (result < 0) {
        fprintf(stderr, "%s\n", session.error);
    }
    close(fd);
    free(owned_body);
    return result == 0 ? 0 : 2;
}
