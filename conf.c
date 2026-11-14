/* See LICENSE file for copyright and license details. */
#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "conf.h"

#define ERRLOG_MAX 32
static char errlog[ERRLOG_MAX][256];
static int nerrlog;
static char *capbuf;
static size_t caplen;

void
conf_capture(char *buf, size_t len)
{
	capbuf = buf;
	caplen = len;
	if (buf && len)
		buf[0] = '\0';
}

void
conf_errors_clear(void)
{
	nerrlog = 0;
}

int
conf_errors_count(void)
{
	return nerrlog;
}

const char *
conf_errors_get(int i)
{
	return i >= 0 && i < nerrlog ? errlog[i] : NULL;
}

void
conf_error(const ConfPos *pos, const char *fmt, ...)
{
	char msg[200];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(msg, sizeof msg, fmt, ap);
	va_end(ap);
	if (capbuf) {
		snprintf(capbuf, caplen, "%s", msg);
		return;
	}
	fprintf(stderr, "nullwl: config: %s:%d: %s\n", pos->file, pos->line, msg);
	if (nerrlog < ERRLOG_MAX)
		snprintf(errlog[nerrlog++], sizeof errlog[0], "%s:%d: %s", pos->file, pos->line, msg);
}

char *
conf_trim(char *s)
{
	char *e;

	while (isspace((unsigned char)*s))
		s++;
	e = s + strlen(s);
	while (e > s && isspace((unsigned char)e[-1]))
		*--e = '\0';
	return s;
}

int
conf_load(const char *path, ConfHandler handler, void *ud)
{
	FILE *f;
	char *buf = NULL, *line, *eq, *key, *val;
	size_t cap = 0;
	ssize_t len;
	ConfPos pos = { path, 0 };
	int errors = 0;

	if (!(f = fopen(path, "r")))
		return -1;

	while ((len = getline(&buf, &cap, f)) >= 0) {
		pos.line++;
		line = conf_trim(buf);
		if (!*line || *line == '#')
			continue;
		if (!(eq = strchr(line, '='))) {
			conf_error(&pos, "expected 'key = value', got '%s'", line);
			errors++;
			continue;
		}
		*eq = '\0';
		key = conf_trim(line);
		val = conf_trim(eq + 1);
		if (!*key) {
			conf_error(&pos, "missing key before '='");
			errors++;
			continue;
		}
		if (handler(key, val, &pos, ud))
			errors++;
	}
	free(buf);
	fclose(f);
	return errors;
}

int
conf_parse_bool(const char *s, int *out)
{
	if (!strcasecmp(s, "1") || !strcasecmp(s, "true") || !strcasecmp(s, "yes") || !strcasecmp(s, "on"))
		*out = 1;
	else if (!strcasecmp(s, "0") || !strcasecmp(s, "false") || !strcasecmp(s, "no") || !strcasecmp(s, "off"))
		*out = 0;
	else
		return -1;
	return 0;
}

int
conf_parse_long(const char *s, long lo, long hi, long *out)
{
	char *end;
	long v;

	if (!*s)
		return -1;
	errno = 0;
	v = strtol(s, &end, 0);
	if (errno || *end || v < lo || v > hi)
		return -1;
	*out = v;
	return 0;
}

int
conf_parse_double(const char *s, double lo, double hi, double *out)
{
	char *end;
	double v;

	if (!*s)
		return -1;
	errno = 0;
	v = strtod(s, &end);
	if (errno || *end || v < lo || v > hi)
		return -1;
	*out = v;
	return 0;
}

int
conf_parse_color(const char *s, uint32_t *rgba)
{
	char *end;
	size_t n;
	unsigned long v;

	if (*s == '#')
		s++;
	else if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
		s += 2;
	n = strlen(s);
	if (n != 6 && n != 8)
		return -1;
	errno = 0;
	v = strtoul(s, &end, 16);
	if (errno || *end)
		return -1;
	*rgba = n == 6 ? (uint32_t)(v << 8 | 0xff) : (uint32_t)v;
	return 0;
}

size_t
conf_split(char *s, char sep, char **out, size_t max)
{
	size_t n = 0;
	char *p;

	if (!max)
		return 0;
	while (n < max) {
		out[n++] = s;
		if (n == max || !(p = strchr(s, sep)))
			break;
		*p = '\0';
		s = p + 1;
	}
	for (size_t i = 0; i < n; i++)
		out[i] = conf_trim(out[i]);
	return n;
}
