/* See LICENSE file for copyright and license details. */
#define _GNU_SOURCE /* accept4, SO_PEERCRED */
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "ipc.h"

#define IPC_LINE_MAX    (64 * 1024)   /* longest accepted request */
#define IPC_OUT_MAX     (1024 * 1024) /* a client this far behind is dropped */
#define IPC_CLIENTS_MAX 64

/* ------------------------------------------------------------------ */
/* JSON output                                                         */
/* ------------------------------------------------------------------ */
void
jb_init(JBuf *b)
{
	b->s = NULL;
	b->len = b->cap = 0;
}

void
jb_free(JBuf *b)
{
	free(b->s);
	jb_init(b);
}

static int
jb_reserve(JBuf *b, size_t extra)
{
	size_t need = b->len + extra + 1, cap;
	char *n;

	if (need <= b->cap)
		return 0;
	cap = b->cap ? b->cap : 256;
	while (cap < need)
		cap *= 2;
	if (!(n = realloc(b->s, cap)))
		return -1;
	b->s = n;
	b->cap = cap;
	return 0;
}

void
jb_printf(JBuf *b, const char *fmt, ...)
{
	va_list ap, ap2;
	int n;

	va_start(ap, fmt);
	va_copy(ap2, ap);
	n = vsnprintf(NULL, 0, fmt, ap);
	va_end(ap);
	if (n >= 0 && jb_reserve(b, (size_t)n) == 0) {
		vsnprintf(b->s + b->len, (size_t)n + 1, fmt, ap2);
		b->len += (size_t)n;
	}
	va_end(ap2);
}

void
jb_str(JBuf *b, const char *s)
{
	if (!s) {
		jb_printf(b, "null");
		return;
	}
	jb_printf(b, "\"");
	for (; *s; s++) {
		unsigned char c = (unsigned char)*s;
		switch (c) {
		case '"':  jb_printf(b, "\\\""); break;
		case '\\': jb_printf(b, "\\\\"); break;
		case '\n': jb_printf(b, "\\n"); break;
		case '\r': jb_printf(b, "\\r"); break;
		case '\t': jb_printf(b, "\\t"); break;
		default:
			if (c < 0x20)
				jb_printf(b, "\\u%04x", c);
			else
				jb_printf(b, "%c", c);
		}
	}
	jb_printf(b, "\"");
}

void
jb_sep(JBuf *b)
{
	if (b->len && !strchr("[{:", b->s[b->len - 1]))
		jb_printf(b, ",");
}

void
jb_key(JBuf *b, const char *key)
{
	jb_sep(b);
	jb_str(b, key);
	jb_printf(b, ":");
}

/* ------------------------------------------------------------------ */
/* request parsing                                                     */
/* ------------------------------------------------------------------ */
static const char *
skipws(const char *p)
{
	while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
		p++;
	return p;
}

static void
put_utf8(JBuf *b, unsigned cp)
{
	if (cp < 0x80)
		jb_printf(b, "%c", (int)cp);
	else if (cp < 0x800)
		jb_printf(b, "%c%c", (int)(0xc0 | cp >> 6), (int)(0x80 | (cp & 0x3f)));
	else
		jb_printf(b, "%c%c%c", (int)(0xe0 | cp >> 12), (int)(0x80 | ((cp >> 6) & 0x3f)), (int)(0x80 | (cp & 0x3f)));
}

/* p points at the opening quote. Returns a malloc'd string, advances *pp. */
static char *
parse_string(const char **pp, char *err, size_t errlen)
{
	const char *p = *pp + 1;
	JBuf b;
	char *out;

	jb_init(&b);
	/* jb_printf can't append a NUL-safe raw byte, so build with it and rely on no embedded NULs */
	for (; *p && *p != '"'; p++) {
		if (*p == '\\') {
			p++;
			switch (*p) {
			case '"': case '\\': case '/': jb_printf(&b, "%c", *p); break;
			case 'b': jb_printf(&b, "\b"); break;
			case 'f': jb_printf(&b, "\f"); break;
			case 'n': jb_printf(&b, "\n"); break;
			case 'r': jb_printf(&b, "\r"); break;
			case 't': jb_printf(&b, "\t"); break;
			case 'u': {
				unsigned cp = 0;
				for (int i = 1; i <= 4; i++) {
					char h = p[i];
					if (!((h >= '0' && h <= '9') || (h >= 'a' && h <= 'f') || (h >= 'A' && h <= 'F'))) {
						snprintf(err, errlen, "bad \\u escape");
						jb_free(&b);
						return NULL;
					}
					cp = cp << 4 | (unsigned)(h <= '9' ? h - '0' : (h | 0x20) - 'a' + 10);
				}
				if (cp)
					put_utf8(&b, cp);
				p += 4;
				break;
			}
			default:
				snprintf(err, errlen, "bad escape '\\%c'", *p ? *p : '?');
				jb_free(&b);
				return NULL;
			}
		} else {
			jb_printf(&b, "%c", *p);
		}
	}
	if (*p != '"') {
		snprintf(err, errlen, "unterminated string");
		jb_free(&b);
		return NULL;
	}
	*pp = p + 1;
	out = b.s ? b.s : strdup("");
	return out;
}

/* a bare token: number, true, false, null */
static char *
parse_token(const char **pp, char *err, size_t errlen)
{
	const char *p = *pp, *e = p;
	char *out;

	while (*e && !strchr(",]} \t\r\n", *e))
		e++;
	if (e == p) {
		snprintf(err, errlen, "expected a value");
		return NULL;
	}
	if (!(out = strndup(p, (size_t)(e - p))))
		return NULL;
	*pp = e;
	return out;
}

static char *
parse_value(const char **pp, char *err, size_t errlen)
{
	const char *p = skipws(*pp);
	char *v;

	if (*p == '"') {
		v = parse_string(&p, err, errlen);
	} else if (*p == '[') {
		JBuf b;
		jb_init(&b);
		p = skipws(p + 1);
		while (*p && *p != ']') {
			char *e;
			if (b.len)
				jb_printf(&b, ",");
			if (*p == '[' || *p == '{') {
				snprintf(err, errlen, "nested arrays and objects are not supported");
				jb_free(&b);
				return NULL;
			}
			e = *p == '"' ? parse_string(&p, err, errlen) : parse_token(&p, err, errlen);
			if (!e) { jb_free(&b); return NULL; }
			jb_printf(&b, "%s", e);
			free(e);
			p = skipws(p);
			if (*p == ',')
				p = skipws(p + 1);
			else if (*p != ']') {
				snprintf(err, errlen, "expected ',' or ']'");
				jb_free(&b);
				return NULL;
			}
		}
		if (*p != ']') {
			snprintf(err, errlen, "unterminated array");
			jb_free(&b);
			return NULL;
		}
		p++;
		v = b.s ? b.s : strdup("");
	} else if (*p == '{') {
		snprintf(err, errlen, "nested objects are not supported");
		return NULL;
	} else {
		v = parse_token(&p, err, errlen);
	}
	*pp = p;
	return v;
}

int
ipc_req_parse(IpcReq *req, const char *line, char *err, size_t errlen)
{
	const char *p = skipws(line);

	memset(req, 0, sizeof *req);
	if (*p != '{') {
		snprintf(err, errlen, "request must be a JSON object");
		return -1;
	}
	p = skipws(p + 1);
	while (*p && *p != '}') {
		char *k, *v;
		if (*p != '"') {
			snprintf(err, errlen, "expected a string key");
			goto fail;
		}
		if (!(k = parse_string(&p, err, errlen)))
			goto fail;
		p = skipws(p);
		if (*p != ':') {
			free(k);
			snprintf(err, errlen, "expected ':' after key");
			goto fail;
		}
		p++;
		if (!(v = parse_value(&p, err, errlen))) {
			free(k);
			goto fail;
		}
		if (req->n == IPC_MAX_FIELDS) {
			free(k); free(v);
			snprintf(err, errlen, "too many fields (max %d)", IPC_MAX_FIELDS);
			goto fail;
		}
		req->keys[req->n] = k;
		req->vals[req->n++] = v;
		p = skipws(p);
		if (*p == ',')
			p = skipws(p + 1);
		else if (*p != '}') {
			snprintf(err, errlen, "expected ',' or '}'");
			goto fail;
		}
	}
	if (*p != '}') {
		snprintf(err, errlen, "unterminated object");
		goto fail;
	}
	if (*skipws(p + 1)) {
		snprintf(err, errlen, "trailing characters after the object");
		goto fail;
	}
	return 0;
fail:
	ipc_req_free(req);
	return -1;
}

void
ipc_req_free(IpcReq *req)
{
	for (int i = 0; i < req->n; i++) {
		free(req->keys[i]);
		free(req->vals[i]);
	}
	req->n = 0;
}

const char *
ipc_req_get(const IpcReq *req, const char *key)
{
	for (int i = 0; i < req->n; i++)
		if (!strcmp(req->keys[i], key))
			return req->vals[i];
	return NULL;
}

/* ------------------------------------------------------------------ */
/* server                                                              */
/* ------------------------------------------------------------------ */
struct IpcClient {
	Ipc *ipc;
	int fd;
	struct wl_event_source *src;
	char *in;
	size_t inlen;
	char *out;
	size_t outlen, outoff, outcap;
	char *filter;     /* NULL = every event */
	int subscribed;
	int closing;      /* destroy as soon as we are out of the handler */
	int in_handler;
	struct wl_list link;
};

struct Ipc {
	struct wl_event_loop *loop;
	int fd;
	struct wl_event_source *src;
	char *path;
	IpcHandler handler;
	void *data;
	struct wl_list clients;
	int nclients, nsubs;
};

static void client_destroy(IpcClient *c);

static void
client_want_write(IpcClient *c, int on)
{
	wl_event_source_fd_update(c->src, WL_EVENT_READABLE | (on ? WL_EVENT_WRITABLE : 0));
}

static void
client_flush(IpcClient *c)
{
	while (c->outoff < c->outlen) {
		ssize_t n = send(c->fd, c->out + c->outoff, c->outlen - c->outoff, MSG_NOSIGNAL);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				client_want_write(c, 1);
				return;
			}
			c->closing = 1;
			return;
		}
		c->outoff += (size_t)n;
	}
	c->outoff = c->outlen = 0;
	client_want_write(c, 0);
}

static void
client_send(IpcClient *c, const char *s, size_t len)
{
	if (c->closing)
		return;
	if (c->outlen + len > c->outcap) {
		size_t cap = c->outcap ? c->outcap : 1024;
		char *n;
		while (cap < c->outlen + len)
			cap *= 2;
		if (!(n = realloc(c->out, cap))) {
			c->closing = 1;
			return;
		}
		c->out = n;
		c->outcap = cap;
	}
	memcpy(c->out + c->outlen, s, len);
	c->outlen += len;
	if (c->outlen - c->outoff > IPC_OUT_MAX) { /* subscriber that never reads */
		c->closing = 1;
		return;
	}
	client_flush(c);
}

static void
client_send_line(IpcClient *c, JBuf *b)
{
	jb_printf(b, "\n");
	if (b->s)
		client_send(c, b->s, b->len);
}

void
ipc_reply_ok(IpcClient *c, const char *data_json)
{
	JBuf b;

	jb_init(&b);
	jb_printf(&b, "{\"ok\":true,\"data\":%s}", data_json ? data_json : "null");
	client_send_line(c, &b);
	jb_free(&b);
}

void
ipc_reply_err(IpcClient *c, const char *fmt, ...)
{
	char msg[512];
	va_list ap;
	JBuf b;

	va_start(ap, fmt);
	vsnprintf(msg, sizeof msg, fmt, ap);
	va_end(ap);
	jb_init(&b);
	jb_printf(&b, "{\"ok\":false,\"error\":");
	jb_str(&b, msg);
	jb_printf(&b, "}");
	client_send_line(c, &b);
	jb_free(&b);
}

void
ipc_subscribe(IpcClient *c, const char *events)
{
	free(c->filter);
	c->filter = events && *events ? strdup(events) : NULL;
	if (!c->subscribed) {
		c->subscribed = 1;
		c->ipc->nsubs++;
	}
}

int
ipc_nsubscribers(const Ipc *ipc)
{
	return ipc ? ipc->nsubs : 0;
}

static int
filter_matches(const char *filter, const char *name)
{
	size_t n = strlen(name);

	if (!filter)
		return 1;
	for (const char *p = filter; *p;) {
		const char *e = strchr(p, ',');
		size_t len = e ? (size_t)(e - p) : strlen(p);
		if (len == n && !strncmp(p, name, n))
			return 1;
		p += len + (e ? 1 : 0);
	}
	return 0;
}

void
ipc_emit(Ipc *ipc, const char *name, const char *data_json)
{
	IpcClient *c, *tmp;
	JBuf b;

	if (!ipc || !ipc->nsubs)
		return;
	jb_init(&b);
	jb_printf(&b, "{\"event\":");
	jb_str(&b, name);
	jb_printf(&b, ",\"data\":%s}\n", data_json ? data_json : "null");
	wl_list_for_each_safe(c, tmp, &ipc->clients, link) {
		if (!c->subscribed || !filter_matches(c->filter, name))
			continue;
		client_send(c, b.s, b.len);
		if (c->closing && !c->in_handler)
			client_destroy(c);
	}
	jb_free(&b);
}

static void
client_destroy(IpcClient *c)
{
	if (c->subscribed)
		c->ipc->nsubs--;
	c->ipc->nclients--;
	wl_list_remove(&c->link);
	wl_event_source_remove(c->src);
	close(c->fd);
	free(c->in);
	free(c->out);
	free(c->filter);
	free(c);
}

static void
client_line(IpcClient *c, char *line)
{
	IpcReq req;
	char err[128];

	if (!*skipws(line))
		return;
	if (ipc_req_parse(&req, line, err, sizeof err)) {
		ipc_reply_err(c, "bad request: %s", err);
		return;
	}
	c->ipc->handler(c, &req, c->ipc->data);
	ipc_req_free(&req);
}

static int
client_event(int fd, uint32_t mask, void *data)
{
	IpcClient *c = data;
	ssize_t n;

	if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) {
		client_destroy(c);
		return 0;
	}
	if (mask & WL_EVENT_WRITABLE) {
		client_flush(c);
		if (c->closing) {
			client_destroy(c);
			return 0;
		}
	}
	if (!(mask & WL_EVENT_READABLE))
		return 0;

	n = read(fd, c->in + c->inlen, IPC_LINE_MAX - c->inlen);
	if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) {
		client_destroy(c);
		return 0;
	}
	if (n > 0)
		c->inlen += (size_t)n;

	c->in_handler = 1;
	for (;;) {
		char *nl = memchr(c->in, '\n', c->inlen);
		size_t used;
		if (!nl)
			break;
		*nl = '\0';
		client_line(c, c->in);
		used = (size_t)(nl - c->in) + 1;
		memmove(c->in, c->in + used, c->inlen - used);
		c->inlen -= used;
		if (c->closing)
			break;
	}
	c->in_handler = 0;
	if (c->inlen == IPC_LINE_MAX) { /* a line that never ends */
		ipc_reply_err(c, "request too long");
		c->closing = 1;
	}
	if (c->closing)
		client_destroy(c);
	return 0;
}

static int
accept_event(int fd, uint32_t mask, void *data)
{
	Ipc *ipc = data;
	struct ucred cred;
	socklen_t len = sizeof cred;
	IpcClient *c;
	int cfd = accept4(fd, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);

	if (cfd < 0)
		return 0;
	if (ipc->nclients >= IPC_CLIENTS_MAX
			|| getsockopt(cfd, SOL_SOCKET, SO_PEERCRED, &cred, &len) < 0
			|| cred.uid != geteuid()) {
		close(cfd);
		return 0;
	}
	if (!(c = calloc(1, sizeof *c)) || !(c->in = malloc(IPC_LINE_MAX))) {
		free(c);
		close(cfd);
		return 0;
	}
	c->ipc = ipc;
	c->fd = cfd;
	if (!(c->src = wl_event_loop_add_fd(ipc->loop, cfd, WL_EVENT_READABLE, client_event, c))) {
		free(c->in);
		free(c);
		close(cfd);
		return 0;
	}
	wl_list_insert(&ipc->clients, &c->link);
	ipc->nclients++;
	return 0;
}

Ipc *
ipc_create(struct wl_event_loop *loop, const char *path, IpcHandler handler, void *data)
{
	struct sockaddr_un addr = { .sun_family = AF_UNIX };
	Ipc *ipc;
	mode_t old;
	int fd;

	if (strlen(path) >= sizeof addr.sun_path) {
		fprintf(stderr, "nullwl: ipc: socket path too long: %s\n", path);
		return NULL;
	}
	strcpy(addr.sun_path, path);
	if ((fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0)) < 0)
		return NULL;
	unlink(path); /* stale socket from a crashed instance of the same display */
	old = umask(0177);
	if (bind(fd, (struct sockaddr *)&addr, sizeof addr) < 0 || listen(fd, 8) < 0) {
		fprintf(stderr, "nullwl: ipc: cannot listen on %s: %s\n", path, strerror(errno));
		umask(old);
		close(fd);
		return NULL;
	}
	umask(old);
	if (!(ipc = calloc(1, sizeof *ipc))) {
		close(fd);
		return NULL;
	}
	ipc->loop = loop;
	ipc->fd = fd;
	ipc->path = strdup(path);
	ipc->handler = handler;
	ipc->data = data;
	wl_list_init(&ipc->clients);
	if (!(ipc->src = wl_event_loop_add_fd(loop, fd, WL_EVENT_READABLE, accept_event, ipc))) {
		close(fd);
		unlink(path);
		free(ipc->path);
		free(ipc);
		return NULL;
	}
	return ipc;
}

void
ipc_destroy(Ipc *ipc)
{
	IpcClient *c, *tmp;

	if (!ipc)
		return;
	wl_list_for_each_safe(c, tmp, &ipc->clients, link)
		client_destroy(c);
	wl_event_source_remove(ipc->src);
	close(ipc->fd);
	unlink(ipc->path);
	free(ipc->path);
	free(ipc);
}

Ipc *
ipc_client_server(IpcClient *c)
{
	return c->ipc;
}
