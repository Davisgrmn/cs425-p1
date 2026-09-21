#define _POSIX_C_SOURCE 200809L
#include "harness/unity.h"
#include "../src/lab.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

typedef struct {
    const char *input;
    size_t length, position, chunk, read_calls;
    char output[8192];
    size_t written, write_chunk, fail_write_at;
    int read_error, write_zero;
} scripted_server;

static scripted_server server;
static smtp_session session;
static int sockets[3];

void setUp(void)
{
    for (size_t i = 0; i < 3; ++i) sockets[i] = -1;
}

void tearDown(void)
{
    for (size_t i = 0; i < 3; ++i) {
        if (sockets[i] >= 0) close(sockets[i]);
    }
}

static ssize_t scripted_read(void *context, void *buffer, size_t size)
{
    scripted_server *script = context;
    ++script->read_calls;
    if (script->read_error) {
        errno = EIO;
        return -1;
    }
    size_t count = script->length - script->position;
    if (count > size) count = size;
    if (count > script->chunk) count = script->chunk;
    memcpy(buffer, script->input + script->position, count);
    script->position += count;
    return (ssize_t)count;
}

static ssize_t scripted_write(void *context, const void *buffer, size_t size)
{
    scripted_server *script = context;
    if (script->written >= script->fail_write_at) {
        errno = EPIPE;
        return -1;
    }
    if (script->write_zero) return 0;
    size_t count = size;
    if (count > script->write_chunk) count = script->write_chunk;
    if (count > script->fail_write_at - script->written)
        count = script->fail_write_at - script->written;
    if (count >= sizeof(script->output) - script->written) {
        errno = ENOSPC;
        return -1;
    }
    memcpy(script->output + script->written, buffer, count);
    script->written += count;
    script->output[script->written] = '\0';
    return (ssize_t)count;
}

static void prepare(const char *input)
{
    server = (scripted_server){.input = input, .length = strlen(input),
        .chunk = SIZE_MAX, .write_chunk = SIZE_MAX, .fail_write_at = SIZE_MAX};
    smtp_session_init(&session, (smtp_transport){scripted_read, scripted_write, &server});
}

static void assert_text(const char *expected, char *actual)
{
    TEST_ASSERT_NOT_NULL(actual);
    int equal = strcmp(expected, actual) == 0;
    free(actual);
    TEST_ASSERT_TRUE(equal);
}

static void test_fields_and_reply_helpers(void)
{
    TEST_ASSERT_TRUE(smtp_valid_field(""));
    TEST_ASSERT_TRUE(smtp_valid_field("sender@example.com"));
    TEST_ASSERT_FALSE(smtp_valid_field(NULL));
    TEST_ASSERT_FALSE(smtp_valid_field("x\ry"));
    TEST_ASSERT_FALSE(smtp_valid_field("x\ny"));
    const char *invalid[] = {NULL, "", "2", "25", "250\r\n", "150 bad", "650 bad",
        "2/0 bad", "260 bad", "25/ bad", "25: bad", "250x", "abc"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        TEST_ASSERT_EQUAL_INT(-1, smtp_reply_code(invalid[i]));
        TEST_ASSERT_EQUAL_INT(-1, smtp_reply_final(invalid[i]));
    }
    TEST_ASSERT_EQUAL_INT(220, smtp_reply_code("220 ready"));
    TEST_ASSERT_EQUAL_INT(354, smtp_reply_code("354 go ahead"));
    TEST_ASSERT_EQUAL_INT(550, smtp_reply_code("550 rejected"));
    TEST_ASSERT_EQUAL_INT(250, smtp_reply_code("250"));
    TEST_ASSERT_EQUAL_INT(250, smtp_reply_code("250-continued"));
    TEST_ASSERT_EQUAL_INT(0, smtp_reply_final("250-continued"));
    TEST_ASSERT_EQUAL_INT(1, smtp_reply_final("250 final"));
    TEST_ASSERT_EQUAL_INT(1, smtp_reply_final("250"));
    TEST_ASSERT_EQUAL_INT(1, smtp_reply_final("250 "));
}

static void test_commands(void)
{
    assert_text("HELO localhost\r\n", smtp_build_command(SMTP_HELO, "localhost"));
    assert_text("MAIL FROM:<a@b>\r\n", smtp_build_command(SMTP_MAIL, "a@b"));
    assert_text("RCPT TO:<c@d>\r\n", smtp_build_command(SMTP_RCPT, "c@d"));
    assert_text("MAIL FROM:<>\r\n", smtp_build_command(SMTP_MAIL, ""));
    assert_text("DATA\r\n", smtp_build_command(SMTP_DATA, NULL));
    assert_text("QUIT\r\n", smtp_build_command(SMTP_QUIT, NULL));
    TEST_ASSERT_NULL(smtp_build_command((smtp_command)99, ""));
    TEST_ASSERT_NULL(smtp_build_command(SMTP_HELO, NULL));
    TEST_ASSERT_NULL(smtp_build_command(SMTP_MAIL, "x\r\nRCPT TO:<evil>"));
    TEST_ASSERT_NULL(smtp_build_command(SMTP_RCPT, "x\n"));
}

static void test_dot_stuff_and_data(void)
{
    TEST_ASSERT_NULL(smtp_dot_stuff(NULL));
    assert_text("", smtp_dot_stuff(""));
    assert_text("body\r\n", smtp_dot_stuff("body"));
    assert_text("body\r\n", smtp_dot_stuff("body\n"));
    assert_text("body\r\n", smtp_dot_stuff("body\r\n"));
    assert_text("\r\n\r\n", smtp_dot_stuff("\n\n"));
    assert_text("..\r\n...x\r\na.b\r\n..last\r\n", smtp_dot_stuff(".\r..x\na.b\r\n.last"));
    assert_text("..\r\n", smtp_dot_stuff("."));
    assert_text("x\r\n", smtp_dot_stuff("x\r"));
    assert_text("From: a\r\nTo: b\r\nSubject: hello\r\n\r\n..\r\n.\r\n",
                smtp_build_data("a", "b", "hello", "."));
    assert_text("From: a\r\nTo: b\r\nSubject: \r\n\r\n.\r\n",
                smtp_build_data("a", "b", "", ""));
    TEST_ASSERT_NULL(smtp_build_data(NULL, "b", "", ""));
    TEST_ASSERT_NULL(smtp_build_data("a", "b\n", "", ""));
    TEST_ASSERT_NULL(smtp_build_data("a", "b", "s\r", ""));
    TEST_ASSERT_NULL(smtp_build_data("a", "b", "", NULL));
}

static void test_reader_buffering_and_fragmentation(void)
{
    char line[SMTP_LINE_SIZE];
    prepare("220 ready\r\n250 ok\r\n");
    TEST_ASSERT_EQUAL_UINT(0, session.next);
    TEST_ASSERT_EQUAL_UINT(0, session.end);
    TEST_ASSERT_EQUAL_STRING("", session.error);
    TEST_ASSERT_EQUAL_INT(0, smtp_read_line(&session, line, sizeof(line)));
    TEST_ASSERT_EQUAL_STRING("220 ready", line);
    TEST_ASSERT_EQUAL_INT(0, smtp_read_line(&session, line, sizeof(line)));
    TEST_ASSERT_EQUAL_STRING("250 ok", line);
    TEST_ASSERT_EQUAL_UINT(1, server.read_calls);
    for (size_t chunk = 1; chunk <= 5; ++chunk) {
        prepare("220 ready\r\n250 ok\r\n");
        server.chunk = chunk;
        TEST_ASSERT_EQUAL_INT(0, smtp_read_line(&session, line, sizeof(line)));
        TEST_ASSERT_EQUAL_STRING("220 ready", line);
        TEST_ASSERT_EQUAL_INT(0, smtp_read_line(&session, line, sizeof(line)));
        TEST_ASSERT_EQUAL_STRING("250 ok", line);
    }
}

static void test_reader_boundaries_and_failures(void)
{
    char line[SMTP_LINE_SIZE];
    const char *invalid[] = {"", "220 partial", "\n", "220 bad\n", "220 b\rad\r\n"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        prepare(invalid[i]);
        TEST_ASSERT_EQUAL_INT(-1, smtp_read_line(&session, line, sizeof(line)));
        TEST_ASSERT_NOT_EQUAL(0, session.error[0]);
    }
    prepare("220 ok\r\n");
    TEST_ASSERT_EQUAL_INT(-1, smtp_read_line(&session, line, 0));
    TEST_ASSERT_EQUAL_INT(-1, smtp_read_line(&session, line, 1));
    TEST_ASSERT_EQUAL_INT(-1, smtp_read_line(&session, line, 2));
    prepare("220\0bad\r\n");
    server.length = 10;
    TEST_ASSERT_EQUAL_INT(-1, smtp_read_line(&session, line, sizeof(line)));
    TEST_ASSERT_NOT_NULL(strstr(session.error, "NUL"));
    prepare("");
    server.read_error = 1;
    TEST_ASSERT_EQUAL_INT(-1, smtp_read_line(&session, line, sizeof(line)));
    TEST_ASSERT_NOT_NULL(strstr(session.error, "Read failed"));
    char boundary[SMTP_LINE_SIZE + 1];
    memset(boundary, 'x', sizeof(boundary));
    boundary[510] = '\r'; boundary[511] = '\n'; boundary[512] = '\0';
    prepare(boundary);
    TEST_ASSERT_EQUAL_INT(0, smtp_read_line(&session, line, sizeof(line)));
    TEST_ASSERT_EQUAL_UINT(510, strlen(line));
    boundary[510] = 'x'; boundary[511] = '\r'; boundary[512] = '\n'; boundary[513] = '\0';
    prepare(boundary);
    TEST_ASSERT_EQUAL_INT(-1, smtp_read_line(&session, line, sizeof(line)));
    TEST_ASSERT_NOT_NULL(strstr(session.error, "exceeds"));
}

static void test_multiline_replies(void)
{
    char line[SMTP_LINE_SIZE];
    int code = 0;
    prepare("250-first\r\n250-second\r\n250 last\r\n221 bye\r\n");
    server.chunk = 3;
    TEST_ASSERT_EQUAL_INT(0, smtp_read_reply(&session, &code, line));
    TEST_ASSERT_EQUAL_INT(250, code);
    TEST_ASSERT_EQUAL_STRING("250 last", line);
    TEST_ASSERT_EQUAL_INT(0, smtp_read_reply(&session, &code, line));
    TEST_ASSERT_EQUAL_INT(221, code);
    /* A whole reply may exceed the transport buffer while each line fits. */
    char long_reply[4096] = "";
    for (size_t i = 0; i < 200; ++i) strcat(long_reply, "250-continued\r\n");
    strcat(long_reply, "250 done\r\n");
    prepare(long_reply);
    TEST_ASSERT_EQUAL_INT(0, smtp_read_reply(&session, &code, line));
    TEST_ASSERT_EQUAL_STRING("250 done", line);
    TEST_ASSERT_TRUE(server.read_calls > 1);
    const char *invalid[] = {"not a reply\r\n", "250-first\r\n550 changed\r\n",
                            "250-first\r\n", "250-first\r\n250-unfinished"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        prepare(invalid[i]);
        TEST_ASSERT_EQUAL_INT(-1, smtp_read_reply(&session, &code, line));
    }
}

static void test_writes_and_command_reply(void)
{
    prepare("250 ok\r\n");
    server.write_chunk = 2;
    TEST_ASSERT_EQUAL_INT(0, smtp_write_all(&session, ""));
    TEST_ASSERT_EQUAL_INT(0, smtp_command_reply(&session, "HELO localhost\r\n", 250));
    TEST_ASSERT_EQUAL_STRING("HELO localhost\r\n", server.output);
    prepare("550 no\r\n");
    TEST_ASSERT_EQUAL_INT(-1, smtp_command_reply(&session, "DATA\r\n", 354));
    TEST_ASSERT_NOT_NULL(strstr(session.error, "Expected 354, received 550 no"));
    prepare("");
    server.fail_write_at = 3;
    TEST_ASSERT_EQUAL_INT(-1, smtp_command_reply(&session, "DATA\r\n", 354));
    TEST_ASSERT_EQUAL_STRING("DAT", server.output);
    TEST_ASSERT_NOT_NULL(strstr(session.error, "Write failed"));
    prepare("");
    server.write_zero = 1;
    TEST_ASSERT_EQUAL_INT(-1, smtp_write_all(&session, "QUIT\r\n"));
    TEST_ASSERT_NOT_NULL(strstr(session.error, "no progress"));
}

static const char *replies[] = {"220 ready\r\n", "250 hello\r\n", "250 sender\r\n",
    "250 recipient\r\n", "354 data\r\n", "250 queued\r\n", "221 bye\r\n"};
static const char *commands[] = {"HELO localhost\r\n", "MAIL FROM:<a@b>\r\n",
    "RCPT TO:<c@d>\r\n", "DATA\r\n",
    "From: a@b\r\nTo: c@d\r\nSubject: hello\r\n\r\n..body\r\n.\r\n", "QUIT\r\n"};

static int run_session(void)
{
    return smtp_run(&session, "localhost", "a@b", "c@d", "hello", ".body");
}

static void test_complete_session(void)
{
    char input[1024] = "", output[1024] = "";
    for (size_t i = 0; i < 7; ++i) strcat(input, replies[i]);
    for (size_t i = 0; i < 6; ++i) strcat(output, commands[i]);
    prepare(input);
    TEST_ASSERT_EQUAL_INT(0, run_session());
    TEST_ASSERT_EQUAL_STRING(output, server.output);
    TEST_ASSERT_EQUAL_UINT(1, server.read_calls);
    prepare("220-first greeting\r\n220 ready\r\n250-first\r\n250-last\r\n250 hello\r\n"
            "250 sender\r\n250 recipient\r\n354 data\r\n250 queued\r\n221 bye\r\n");
    server.chunk = 1;
    server.write_chunk = 1;
    TEST_ASSERT_EQUAL_INT(0, run_session());
    TEST_ASSERT_EQUAL_STRING(output, server.output);
}

static void test_every_wrong_code_stops_session(void)
{
    const char *stages[] = {"greeting", "HELO", "MAIL FROM", "RCPT TO", "DATA", "message", "QUIT"};
    for (size_t bad = 0; bad < 7; ++bad) {
        char input[1024] = "", output[1024] = "";
        for (size_t i = 0; i < 7; ++i)
            strcat(input, i == bad ? "550-denied\r\n550 refused\r\n" : replies[i]);
        for (size_t i = 0; i < bad; ++i) strcat(output, commands[i]);
        prepare(input);
        server.chunk = 2;
        TEST_ASSERT_EQUAL_INT(-1, run_session());
        TEST_ASSERT_EQUAL_STRING(output, server.output);
        TEST_ASSERT_NOT_NULL(strstr(session.error, "550 refused"));
        TEST_ASSERT_NOT_NULL(strstr(session.error, stages[bad]));
    }
}

static void test_disconnects_and_write_failures_at_every_stage(void)
{
    char full[1024] = "";
    for (size_t i = 0; i < 7; ++i) strcat(full, replies[i]);
    /* Every truncated prefix, including in the middle of a CRLF. */
    for (size_t cut = 0; cut < strlen(full); ++cut) {
        prepare(full);
        server.length = cut;
        server.chunk = 3;
        TEST_ASSERT_EQUAL_INT(-1, run_session());
        TEST_ASSERT_NOT_NULL(strstr(session.error, "disconnected"));
    }
    size_t sent = 0;
    for (size_t stage = 0; stage < 6; ++stage) {
        prepare(full);
        server.fail_write_at = sent;
        TEST_ASSERT_EQUAL_INT(-1, run_session());
        TEST_ASSERT_EQUAL_UINT(sent, server.written);
        TEST_ASSERT_NOT_NULL(strstr(session.error, "Write failed"));
        sent += strlen(commands[stage]);
    }
    prepare("220 ready\r\n");
    server.read_error = 1;
    TEST_ASSERT_EQUAL_INT(-1, run_session());
    TEST_ASSERT_EQUAL_UINT(0, server.written);
}

static void test_invalid_session_fields_do_no_io(void)
{
    const char *args[] = {"localhost", "a", "b", "s", "body"};
    for (size_t bad = 0; bad < 5; ++bad) {
        const char *saved = args[bad];
        args[bad] = NULL;
        prepare("");
        TEST_ASSERT_EQUAL_INT(-1, smtp_run(&session, args[0], args[1], args[2], args[3], args[4]));
        TEST_ASSERT_EQUAL_UINT(0, server.written);
        TEST_ASSERT_EQUAL_UINT(0, server.read_calls);
        args[bad] = saved;
    }
    prepare("");
    TEST_ASSERT_EQUAL_INT(-1, smtp_run(&session, "local\r\nQUIT", "a", "b", "", ""));
    TEST_ASSERT_EQUAL_UINT(0, server.read_calls);
}

static void test_socket_callbacks(void)
{
    TEST_ASSERT_EQUAL_INT(0, socketpair(AF_UNIX, SOCK_STREAM, 0, sockets));
    char buffer[32] = {0};
    TEST_ASSERT_EQUAL_INT(5, smtp_socket_write(&sockets[0], "hello", 5));
    TEST_ASSERT_EQUAL_INT(5, smtp_socket_read(&sockets[1], buffer, sizeof(buffer)));
    TEST_ASSERT_EQUAL_STRING("hello", buffer);
    close(sockets[0]); sockets[0] = -1;
    TEST_ASSERT_EQUAL_INT(0, smtp_socket_read(&sockets[1], buffer, sizeof(buffer)));
    TEST_ASSERT_EQUAL_INT(-1, smtp_socket_write(&sockets[1], "closed", 6));
    TEST_ASSERT_EQUAL_INT(-1, smtp_socket_read(&sockets[0], buffer, sizeof(buffer)));
    TEST_ASSERT_EQUAL_INT(-1, smtp_socket_write(&sockets[0], "invalid", 7));
}

static void test_socket_connect(void)
{
    char error[SMTP_ERROR_SIZE] = "", port[16];
    sockets[0] = socket(AF_INET, SOCK_STREAM, 0);
    TEST_ASSERT_TRUE(sockets[0] >= 0);
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    TEST_ASSERT_EQUAL_INT(0, bind(sockets[0], (struct sockaddr *)&address, sizeof(address)));
    socklen_t size = sizeof(address);
    TEST_ASSERT_EQUAL_INT(0, getsockname(sockets[0], (struct sockaddr *)&address, &size));
    snprintf(port, sizeof(port), "%u", (unsigned int)ntohs(address.sin_port));
    /* Bound but not listening: deterministic connection refusal. */
    TEST_ASSERT_EQUAL_INT(-1, smtp_socket_connect("127.0.0.1", port, error, sizeof(error)));
    TEST_ASSERT_NOT_NULL(strstr(error, "Cannot connect"));
    TEST_ASSERT_EQUAL_INT(-1, smtp_socket_connect("127.0.0.1", "invalid/service", error, sizeof(error)));
    TEST_ASSERT_NOT_NULL(strstr(error, "Cannot resolve"));
    TEST_ASSERT_EQUAL_INT(0, listen(sockets[0], 1));
    sockets[1] = smtp_socket_connect("localhost", port, error, sizeof(error));
    TEST_ASSERT_TRUE_MESSAGE(sockets[1] >= 0, error);
    sockets[2] = accept(sockets[0], NULL, NULL);
    TEST_ASSERT_TRUE(sockets[2] >= 0);
    TEST_ASSERT_EQUAL_INT(3, smtp_socket_write(&sockets[1], "TCP", 3));
    char buffer[4] = {0};
    TEST_ASSERT_EQUAL_INT(3, smtp_socket_read(&sockets[2], buffer, 3));
    TEST_ASSERT_EQUAL_STRING("TCP", buffer);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_fields_and_reply_helpers);
    RUN_TEST(test_commands);
    RUN_TEST(test_dot_stuff_and_data);
    RUN_TEST(test_reader_buffering_and_fragmentation);
    RUN_TEST(test_reader_boundaries_and_failures);
    RUN_TEST(test_multiline_replies);
    RUN_TEST(test_writes_and_command_reply);
    RUN_TEST(test_complete_session);
    RUN_TEST(test_every_wrong_code_stops_session);
    RUN_TEST(test_disconnects_and_write_failures_at_every_stage);
    RUN_TEST(test_invalid_session_fields_do_no_io);
    RUN_TEST(test_socket_callbacks);
    RUN_TEST(test_socket_connect);
    return UNITY_END();
}
