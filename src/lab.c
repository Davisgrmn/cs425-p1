#include "lab.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *format_string(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    va_list copy;
    va_copy(copy, args);
    int length = vsnprintf(NULL, 0, format, copy);
    va_end(copy);
    if (length < 0) { /* GCOVR_EXCL_START: vsnprintf failure */
        va_end(args);
        return NULL;
    } /* GCOVR_EXCL_STOP */
    char *result = malloc((size_t)length + 1);
    if (result != NULL) { /* GCOVR_EXCL_BR_LINE: malloc failure */
        vsnprintf(result, (size_t)length + 1, format, args);
    }
    va_end(args);
    return result;
}

int smtp_valid_field(const char *text)
{
    return text != NULL && strpbrk(text, "\r\n") == NULL;
}

int smtp_reply_code(const char *line)
{
    if (!smtp_valid_field(line) || strlen(line) < 3) {
        return -1;
    }
    if (line[0] < '2' || line[0] > '5' || line[1] < '0' ||
        line[1] > '5' || line[2] < '0' || line[2] > '9') {
        return -1;
    }
    if (line[3] != '\0' && line[3] != ' ' && line[3] != '-') {
        return -1;
    }
    return (line[0] - '0') * 100 + (line[1] - '0') * 10 + line[2] - '0';
}

int smtp_reply_final(const char *line)
{
    if (smtp_reply_code(line) < 0) {
        return -1;
    }
    return line[3] != '-';
}

char *smtp_build_command(smtp_command command, const char *argument)
{
    switch (command) {
    case SMTP_HELO:
    case SMTP_MAIL:
    case SMTP_RCPT:
        if (!smtp_valid_field(argument)) {
            return NULL;
        }
        if (command == SMTP_HELO) {
            return format_string("HELO %s\r\n", argument);
        }
        return format_string("%s:<%s>\r\n",
                             command == SMTP_MAIL ? "MAIL FROM" : "RCPT TO",
                             argument);
    case SMTP_DATA:
        return format_string("DATA\r\n");
    case SMTP_QUIT:
        return format_string("QUIT\r\n");
    default:
        return NULL;
    }
}

char *smtp_dot_stuff(const char *body)
{
    if (body == NULL) {
        return NULL;
    }
    /* Each input byte needs at most two output bytes, plus final CRLF and
     * NUL. calloc checks multiplication overflow as well as allocation. */
    char *result = calloc(strlen(body) + 1, 3);
    if (result == NULL) { /* GCOVR_EXCL_START: calloc failure */
        return NULL;
    } /* GCOVR_EXCL_STOP */
    size_t out = 0;
    int start = 1;
    for (size_t i = 0; body[i] != '\0'; ++i) {
        if (body[i] == '\r' || body[i] == '\n') {
            if (body[i] == '\r' && body[i + 1] == '\n') {
                ++i;
            }
            result[out++] = '\r';
            result[out++] = '\n';
            start = 1;
        } else {
            if (start && body[i] == '.') {
                result[out++] = '.';
            }
            result[out++] = body[i];
            start = 0;
        }
    }
    if (!start) {
        result[out++] = '\r';
        result[out++] = '\n';
    }
    result[out] = '\0';
    return result;
}

char *smtp_build_data(const char *from, const char *to,
                      const char *subject, const char *body)
{
    if (!smtp_valid_field(from) || !smtp_valid_field(to) ||
        !smtp_valid_field(subject)) {
        return NULL;
    }
    char *stuffed = smtp_dot_stuff(body);
    if (stuffed == NULL) {
        return NULL;
    }
    char *data = format_string("From: %s\r\nTo: %s\r\nSubject: %s\r\n\r\n%s.\r\n",
                               from, to, subject, stuffed);
    free(stuffed);
    return data;
}

static int fail(smtp_session *session, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    vsnprintf(session->error, sizeof(session->error), format, args);
    va_end(args);
    return -1;
}

void smtp_session_init(smtp_session *session, smtp_transport transport)
{
    *session = (smtp_session){.transport = transport};
}

int smtp_read_line(smtp_session *session, char *line, size_t capacity)
{
    size_t used = 0;
    if (capacity < 2) {
        return fail(session, "Reply buffer is too small");
    }
    line[0] = '\0';
    for (;;) {
        if (session->next == session->end) {
            ssize_t count = session->transport.read(session->transport.context,
                                                    session->input,
                                                    sizeof(session->input));
            if (count < 0) {
                return fail(session, "Read failed: %s", strerror(errno));
            }
            if (count == 0) {
                return fail(session, "Server disconnected before a complete reply: %s", line);
            }
            session->next = 0;
            session->end = (size_t)count;
        }
        char c = session->input[session->next++];
        if (c == '\0') {
            return fail(session, "NUL byte in server reply");
        }
        if (used == capacity - 1) {
            return fail(session, "Server reply exceeds the %zu-byte buffer: %s", capacity, line);
        }
        line[used++] = c;
        line[used] = '\0';
        if (c == '\n') {
            if (used < 2 || line[used - 2] != '\r') {
                return fail(session, "Server reply is not CRLF terminated: %s", line);
            }
            line[used - 2] = '\0';
            if (strchr(line, '\r') != NULL) {
                return fail(session, "Bare CR in server reply: %s", line);
            }
            return 0;
        }
    }
}

int smtp_read_reply(smtp_session *session, int *code, char line[SMTP_LINE_SIZE])
{
    int first = -1;
    for (;;) {
        if (smtp_read_line(session, line, SMTP_LINE_SIZE) < 0) {
            return -1;
        }
        int current = smtp_reply_code(line);
        if (current < 0) {
            return fail(session, "Malformed server reply: %s", line);
        }
        if (first != -1 && current != first) {
            return fail(session, "Inconsistent reply code (expected %d): %s", first, line);
        }
        first = current;
        if (smtp_reply_final(line)) {
            *code = current;
            return 0;
        }
    }
}

int smtp_write_all(smtp_session *session, const char *text)
{
    size_t remaining = strlen(text);
    while (remaining > 0) {
        ssize_t count = session->transport.write(session->transport.context,
                                                 text, remaining);
        if (count < 0) {
            return fail(session, "Write failed: %s", strerror(errno));
        }
        if (count == 0) {
            return fail(session, "Write failed: transport made no progress");
        }
        text += count;
        remaining -= (size_t)count;
    }
    return 0;
}

static int expect_reply(smtp_session *session, int expected)
{
    char line[SMTP_LINE_SIZE];
    int code;
    if (smtp_read_reply(session, &code, line) < 0) {
        return -1;
    }
    if (code != expected) {
        return fail(session, "Expected %d, received %s", expected, line);
    }
    return 0;
}

int smtp_command_reply(smtp_session *session, const char *command, int expected)
{
    if (smtp_write_all(session, command) < 0) {
        return -1;
    }
    return expect_reply(session, expected);
}

int smtp_run(smtp_session *session, const char *helo, const char *from,
             const char *to, const char *subject, const char *body)
{
    /* Prepare everything before starting the dialogue. All allocations share
     * one cleanup path, including invalid-input and mid-session failures. */
    char *commands[] = {
        smtp_build_command(SMTP_HELO, helo),
        smtp_build_command(SMTP_MAIL, from),
        smtp_build_command(SMTP_RCPT, to),
        smtp_build_command(SMTP_DATA, NULL),
        smtp_build_data(from, to, subject, body),
        smtp_build_command(SMTP_QUIT, NULL)
    };
    const int expected[] = {250, 250, 250, 354, 250, 221};
    const char *stages[] = {"HELO", "MAIL FROM", "RCPT TO", "DATA", "message", "QUIT"};
    const char *stage = "greeting";
    int result = -1;
    for (size_t i = 0; i < 6; ++i) {
        if (commands[i] == NULL) {
            fail(session, "Invalid message field or memory allocation failure");
            goto cleanup;
        }
    }
    if (expect_reply(session, 220) < 0) {
        goto protocol_error;
    }
    for (size_t i = 0; i < 6; ++i) {
        stage = stages[i];
        if (smtp_command_reply(session, commands[i], expected[i]) < 0) {
            goto protocol_error;
        }
    }
    result = 0;
    goto cleanup;

protocol_error: {
        char detail[SMTP_ERROR_SIZE];
        memcpy(detail, session->error, sizeof(detail));
        fail(session, "%s: %s", stage, detail);
    }
cleanup:
    for (size_t i = 0; i < 6; ++i) {
        free(commands[i]);
    }
    return result;
}
