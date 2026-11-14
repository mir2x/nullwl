/* cc -D_POSIX_C_SOURCE=200809L -D_GNU_SOURCE -I. tests/ipc_test.c ipc.c $(pkg-config --cflags --libs wayland-server) */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include "ipc.h"

static struct wl_event_loop *loop;
static Ipc *ipc;

static void handler(IpcClient *c, const IpcReq *req, void *data)
{
	const char *cmd = ipc_req_get(req, "cmd");
	if (!cmd) { ipc_reply_err(c, "no cmd"); return; }
	if (!strcmp(cmd, "echo")) {
		JBuf b; jb_init(&b);
		jb_printf(&b, "{"); jb_key(&b, "arg"); jb_str(&b, ipc_req_get(req, "arg"));
		jb_key(&b, "list"); jb_str(&b, ipc_req_get(req, "list")); jb_printf(&b, "}");
		ipc_reply_ok(c, b.s); jb_free(&b);
	} else if (!strcmp(cmd, "subscribe")) {
		ipc_subscribe(c, ipc_req_get(req, "events"));
		ipc_reply_ok(c, NULL);
	} else ipc_reply_err(c, "unknown command '%s'", cmd);
}

static int dial(const char *path)
{
	struct sockaddr_un a = { .sun_family = AF_UNIX };
	strcpy(a.sun_path, path);
	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	assert(connect(fd, (struct sockaddr *)&a, sizeof a) == 0);
	fcntl(fd, F_SETFL, O_NONBLOCK);
	return fd;
}

/* run the server loop, then read what the client got */
static char *talk(int fd, const char *send_str)
{
	static char buf[1 << 16];
	ssize_t n;
	if (send_str) assert(write(fd, send_str, strlen(send_str)) == (ssize_t)strlen(send_str));
	for (int i = 0; i < 5; i++) wl_event_loop_dispatch(loop, 0);
	n = read(fd, buf, sizeof buf - 1);
	buf[n > 0 ? n : 0] = 0;
	return buf;
}

int main(void)
{
	signal(SIGPIPE, SIG_IGN);
	const char *path = "/tmp/nullwl_ipc_test.sock";
	char err[128]; IpcReq r; char *out;

	/* --- parser --- */
	assert(!ipc_req_parse(&r, "{\"cmd\":\"x\",\"n\":12,\"b\":true,\"z\":null,\"a\":[\"p\",\"q\",3],\"s\":\"a\\\"b\\n\\u00e9\"}", err, sizeof err));
	assert(!strcmp(ipc_req_get(&r, "cmd"), "x") && !strcmp(ipc_req_get(&r, "n"), "12"));
	assert(!strcmp(ipc_req_get(&r, "b"), "true") && !strcmp(ipc_req_get(&r, "a"), "p,q,3"));
	assert(!strcmp(ipc_req_get(&r, "s"), "a\"b\n\xc3\xa9") && !ipc_req_get(&r, "nope"));
	ipc_req_free(&r);
	assert(ipc_req_parse(&r, "not json", err, sizeof err));
	assert(ipc_req_parse(&r, "{\"a\":", err, sizeof err));
	assert(ipc_req_parse(&r, "{\"a\":{\"b\":1}}", err, sizeof err));
	assert(ipc_req_parse(&r, "{\"a\":1} trailing", err, sizeof err));
	assert(ipc_req_parse(&r, "{\"a\":\"\\q\"}", err, sizeof err));
	assert(!ipc_req_parse(&r, "  { }  ", err, sizeof err)); ipc_req_free(&r);

	/* --- server --- */
	loop = wl_event_loop_create();
	ipc = ipc_create(loop, path, handler, NULL);
	assert(ipc);
	int a = dial(path), b = dial(path);

	out = talk(a, "{\"cmd\":\"echo\",\"arg\":\"hi \\\"x\\\"\",\"list\":[1,2]}\n");
	assert(!strcmp(out, "{\"ok\":true,\"data\":{\"arg\":\"hi \\\"x\\\"\",\"list\":\"1,2\"}}\n"));
	out = talk(a, "{\"cmd\":\"nope\"}\n");
	assert(!strcmp(out, "{\"ok\":false,\"error\":\"unknown command 'nope'\"}\n"));
	out = talk(a, "garbage\n");
	assert(strstr(out, "\"ok\":false") && strstr(out, "bad request"));
	/* two requests in one write, and one split across writes */
	out = talk(a, "{\"cmd\":\"x\"}\n{\"cmd\":\"y\"}\n");
	assert(strstr(out, "'x'") && strstr(out, "'y'"));
	talk(a, "{\"cmd\":\"ec"); out = talk(a, "ho\",\"arg\":\"z\"}\n");
	assert(strstr(out, "\"arg\":\"z\""));

	/* --- events --- */
	assert(ipc_nsubscribers(ipc) == 0);
	out = talk(a, "{\"cmd\":\"subscribe\",\"events\":[\"focus\",\"title\"]}\n");
	assert(strstr(out, "\"ok\":true") && ipc_nsubscribers(ipc) == 1);
	talk(b, "{\"cmd\":\"subscribe\"}\n"); /* all events */
	assert(ipc_nsubscribers(ipc) == 2);
	ipc_emit(ipc, "layout", "{\"x\":1}");
	assert(!strcmp(talk(a, NULL), ""));                                   /* filtered out for a */
	assert(!strcmp(talk(b, NULL), "{\"event\":\"layout\",\"data\":{\"x\":1}}\n"));
	ipc_emit(ipc, "focus", "null");
	assert(!strcmp(talk(a, NULL), "{\"event\":\"focus\",\"data\":null}\n"));
	assert(strstr(talk(b, NULL), "focus"));
	close(b);
	for (int i = 0; i < 5; i++) wl_event_loop_dispatch(loop, 0);
	assert(ipc_nsubscribers(ipc) == 1);                                   /* hangup cleaned up */

	/* --- abuse: an endless line, and a subscriber that never reads --- */
	int c = dial(path);
	char junk[4096]; memset(junk, 'x', sizeof junk);
	for (int i = 0; i < 20; i++) { if (write(c, junk, sizeof junk) < 0) break; for (int j = 0; j < 3; j++) wl_event_loop_dispatch(loop, 0); }
	out = talk(c, NULL);
	assert(strstr(out, "request too long") || strlen(out) == 0);
	close(c);
	close(a); /* from here on the only subscriber is the slow one */
	for (int i = 0; i < 5; i++) wl_event_loop_dispatch(loop, 0);
	assert(ipc_nsubscribers(ipc) == 0);
	int slow = dial(path);
	talk(slow, "{\"cmd\":\"subscribe\"}\n");
	assert(ipc_nsubscribers(ipc) == 1);
	char big[8192]; memset(big, 'a', sizeof big - 1); big[sizeof big - 1] = 0;
	JBuf jb; jb_init(&jb); jb_str(&jb, big);
	/* never read from `slow`: the kernel buffer fills, then our queue hits the cap and it is dropped */
	for (int i = 0; i < 400 && ipc_nsubscribers(ipc) == 1; i++) ipc_emit(ipc, "title", jb.s);
	assert(ipc_nsubscribers(ipc) == 0);
	jb_free(&jb);

	ipc_destroy(ipc);
	assert(access(path, F_OK) != 0);
	puts("ipc_test: all passed");
	return 0;
}
