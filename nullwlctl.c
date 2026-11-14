/* See LICENSE file for copyright and license details. */
/*
 * nullwlctl: talk to a running nullwl over its IPC socket.
 *
 *   nullwlctl version | reload
 *   nullwlctl get monitors|clients|focused|tray|errors|options|actions
 *   nullwlctl dispatch ACTION [ARG...]      (or just: nullwlctl ACTION [ARG...])
 *   nullwlctl set OPTION VALUE
 *   nullwlctl focus ID | close ID
 *   nullwlctl subscribe [EVENT...]          (streams events until interrupted)
 *   nullwlctl raw '{"cmd":"version"}'
 *
 * Prints the reply's "data" as JSON (pipe it to jq). With -j, prints the
 * whole reply line. Exit status is 1 if nullwl reported an error.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

static void
usage(void)
{
	fputs("usage: nullwlctl [-j] [-s socket] command [args...]\n"
	      "  version | reload\n"
	      "  get monitors|clients|focused|tray|errors|options|actions\n"
	      "  dispatch ACTION [ARG...]    (or: nullwlctl ACTION [ARG...])\n"
	      "  set OPTION VALUE\n"
	      "  focus ID | close ID\n"
	      "  popup [state|key N|motion X Y|click X Y|scroll X Y D|dump F|close]  (the tray menu)\n"
	      "  dump-tray FILE.png          (debugging: render the tray to a file)\n"
	      "  subscribe [EVENT...]\n"
	      "  raw JSON\n", stderr);
	exit(2);
}

/* append s to *buf as a JSON string literal */
static void
append_str(char **buf, size_t *len, size_t *cap, const char *s)
{
	size_t need = *len + 6 * strlen(s) + 4;
	if (need > *cap) {
		*cap = need * 2;
		if (!(*buf = realloc(*buf, *cap)))
			exit(1);
	}
	(*buf)[(*len)++] = '"';
	for (; *s; s++) {
		unsigned char c = (unsigned char)*s;
		if (c == '"' || c == '\\') {
			(*buf)[(*len)++] = '\\';
			(*buf)[(*len)++] = (char)c;
		} else if (c == '\n') {
			(*buf)[(*len)++] = '\\'; (*buf)[(*len)++] = 'n';
		} else if (c == '\t') {
			(*buf)[(*len)++] = '\\'; (*buf)[(*len)++] = 't';
		} else if (c < 0x20) {
			*len += (size_t)sprintf(*buf + *len, "\\u%04x", c);
		} else {
			(*buf)[(*len)++] = (char)c;
		}
	}
	(*buf)[(*len)++] = '"';
	(*buf)[*len] = '\0';
}

static void
append_raw(char **buf, size_t *len, size_t *cap, const char *s)
{
	size_t n = strlen(s);
	if (*len + n + 1 > *cap) {
		*cap = (*len + n + 1) * 2;
		if (!(*buf = realloc(*buf, *cap)))
			exit(1);
	}
	memcpy(*buf + *len, s, n + 1);
	*len += n;
}

/* join argv[from..argc) with spaces */
static char *
join(int argc, char **argv, int from)
{
	size_t n = 1;
	char *r, *p;

	for (int i = from; i < argc; i++)
		n += strlen(argv[i]) + 1;
	p = r = malloc(n);
	if (!r)
		exit(1);
	*r = '\0';
	for (int i = from; i < argc; i++)
		p += sprintf(p, "%s%s", i > from ? " " : "", argv[i]);
	return r;
}

static char *
build_request(int argc, char **argv)
{
	char *b = NULL, *joined;
	size_t len = 0, cap = 0;
	const char *cmd = argv[0];

	if (!strcmp(cmd, "raw")) {
		if (argc != 2)
			usage();
		return strdup(argv[1]);
	}
	append_raw(&b, &len, &cap, "{\"cmd\":");
	if (!strcmp(cmd, "version") || !strcmp(cmd, "reload")) {
		append_str(&b, &len, &cap, cmd);
	} else if (!strcmp(cmd, "get")) {
		if (argc != 2)
			usage();
		append_str(&b, &len, &cap, "get");
		append_raw(&b, &len, &cap, ",\"what\":");
		append_str(&b, &len, &cap, argv[1]);
	} else if (!strcmp(cmd, "set")) {
		if (argc < 3)
			usage();
		joined = join(argc, argv, 2);
		append_str(&b, &len, &cap, "set");
		append_raw(&b, &len, &cap, ",\"option\":");
		append_str(&b, &len, &cap, argv[1]);
		append_raw(&b, &len, &cap, ",\"value\":");
		append_str(&b, &len, &cap, joined);
		free(joined);
	} else if (!strcmp(cmd, "popup")) {
		/* popup [state] | key NAME | motion X Y | click X Y | scroll X Y D | dump FILE | close */
		const char *op = argc > 1 ? argv[1] : "state";
		const char *fields[3] = {0};
		const char *names[3] = {0};
		int n = 0, k;

		if (!strcmp(op, "key")) { names[0] = "name"; n = 1; }
		else if (!strcmp(op, "motion") || !strcmp(op, "click")) { names[0] = "x"; names[1] = "y"; n = 2; }
		else if (!strcmp(op, "scroll")) { names[0] = "x"; names[1] = "y"; names[2] = "delta"; n = 3; }
		else if (!strcmp(op, "dump")) { names[0] = "path"; n = 1; }
		else if (strcmp(op, "state") && strcmp(op, "close"))
			usage();
		if (argc != 2 + n && !(n == 0 && argc <= 2))
			usage();
		append_str(&b, &len, &cap, "popup");
		append_raw(&b, &len, &cap, ",\"op\":");
		append_str(&b, &len, &cap, op);
		for (k = 0; k < n; k++) {
			fields[k] = argv[2 + k];
			append_raw(&b, &len, &cap, ",\"");
			append_raw(&b, &len, &cap, names[k]);
			append_raw(&b, &len, &cap, "\":");
			append_str(&b, &len, &cap, fields[k]);
		}
	} else if (!strcmp(cmd, "dump-tray")) {
		if (argc != 2)
			usage();
		append_str(&b, &len, &cap, "dump-tray");
		append_raw(&b, &len, &cap, ",\"path\":");
		append_str(&b, &len, &cap, argv[1]);
	} else if (!strcmp(cmd, "focus") || !strcmp(cmd, "close")) {
		if (argc != 2)
			usage();
		append_str(&b, &len, &cap, cmd);
		append_raw(&b, &len, &cap, ",\"id\":");
		append_str(&b, &len, &cap, argv[1]);
	} else if (!strcmp(cmd, "subscribe")) {
		append_str(&b, &len, &cap, "subscribe");
		if (argc > 1) {
			joined = join(argc, argv, 1);
			for (char *p = joined; *p; p++)
				if (*p == ' ')
					*p = ',';
			append_raw(&b, &len, &cap, ",\"events\":");
			append_str(&b, &len, &cap, joined);
			free(joined);
		}
	} else {
		int from = !strcmp(cmd, "dispatch") ? 1 : 0;
		if (from && argc < 2)
			usage();
		append_str(&b, &len, &cap, "dispatch");
		append_raw(&b, &len, &cap, ",\"action\":");
		append_str(&b, &len, &cap, argv[from]);
		if (argc > from + 1) {
			joined = join(argc, argv, from + 1);
			append_raw(&b, &len, &cap, ",\"arg\":");
			append_str(&b, &len, &cap, joined);
			free(joined);
		}
	}
	append_raw(&b, &len, &cap, "}");
	return b;
}

static int
connect_socket(const char *path)
{
	struct sockaddr_un a = { .sun_family = AF_UNIX };
	char buf[256];
	int fd;

	if (!path) {
		const char *rt = getenv("XDG_RUNTIME_DIR"), *wd = getenv("WAYLAND_DISPLAY");
		if (!rt || !wd) {
			fputs("nullwlctl: NULLWL_SOCKET is not set and there is no WAYLAND_DISPLAY; is nullwl running?\n", stderr);
			exit(1);
		}
		snprintf(buf, sizeof buf, "%s/nullwl.%s.sock", rt, wd);
		path = buf;
	}
	if (strlen(path) >= sizeof a.sun_path) {
		fprintf(stderr, "nullwlctl: socket path too long\n");
		exit(1);
	}
	strcpy(a.sun_path, path);
	if ((fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0)) < 0 || connect(fd, (struct sockaddr *)&a, sizeof a) < 0) {
		fprintf(stderr, "nullwlctl: cannot connect to %s: %s\n", path, strerror(errno));
		exit(1);
	}
	return fd;
}

static char *rbuf;
static size_t rlen, rcap;

/* Reads one '\n' terminated line (malloc'd, newline stripped). -1 on EOF/error. */
static int
read_line(int fd, char **line)
{
	for (;;) {
		char *nl = rbuf ? memchr(rbuf, '\n', rlen) : NULL;
		ssize_t n;

		if (nl) {
			size_t used = (size_t)(nl - rbuf) + 1;
			*nl = '\0';
			*line = strdup(rbuf);
			if (!*line)
				exit(1);
			memmove(rbuf, rbuf + used, rlen - used);
			rlen -= used;
			return 0;
		}
		if (rlen + 1 >= rcap) {
			rcap = rcap ? rcap * 2 : 8192;
			if (!(rbuf = realloc(rbuf, rcap)))
				exit(1);
		}
		n = read(fd, rbuf + rlen, rcap - rlen - 1);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			return -1;
		rlen += (size_t)n;
		rbuf[rlen] = '\0';
	}
}

static void
print_error(const char *reply)
{
	const char *e = strstr(reply, "\"error\":\"");
	fputs("nullwlctl: ", stderr);
	if (!e) {
		fputs(reply, stderr);
	} else {
		for (e += 9; *e && !(*e == '"' && e[1] == '}'); e++) {
			if (*e == '\\' && e[1])
				e++;
			fputc(*e, stderr);
		}
	}
	fputc('\n', stderr);
}

int
main(int argc, char **argv)
{
	const char *sock = getenv("NULLWL_SOCKET");
	int full = 0, opt, fd, subscribe;
	char *req, *line = NULL;
	struct timeval tv = { 5, 0 };

	while ((opt = getopt(argc, argv, "+js:h")) != -1) {
		if (opt == 'j')
			full = 1;
		else if (opt == 's')
			sock = optarg;
		else
			usage();
	}
	argc -= optind;
	argv += optind;
	if (argc < 1)
		usage();

	signal(SIGPIPE, SIG_IGN);
	subscribe = !strcmp(argv[0], "subscribe");
	req = build_request(argc, argv);
	fd = connect_socket(sock);
	if (!subscribe)
		setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
	if (dprintf(fd, "%s\n", req) < 0) {
		fprintf(stderr, "nullwlctl: write: %s\n", strerror(errno));
		return 1;
	}
	if (read_line(fd, &line)) {
		fprintf(stderr, "nullwlctl: no reply from nullwl\n");
		return 1;
	}
	if (strncmp(line, "{\"ok\":true", 10)) {
		print_error(line);
		return 1;
	}
	if (!subscribe || full) {
		/* {"ok":true,"data":<data>}  ->  <data> */
		size_t n = strlen(line);
		if (!full && !strncmp(line, "{\"ok\":true,\"data\":", 18) && n > 19 && line[n - 1] == '}') {
			line[n - 1] = '\0';
			if (strcmp(line + 18, "null") || !strcmp(argv[0], "get")) /* no data to show for dispatch, set, ... */
				puts(line + 18);
		} else if (full) {
			puts(line);
		}
	}
	free(line);
	if (!subscribe)
		return 0;

	for (;;) { /* events */
		if (read_line(fd, &line))
			return 0;
		puts(line);
		fflush(stdout);
		free(line);
	}
}
