/*
 * Generic line-based config parser. No knowledge of nullwl itself.
 *
 * Format:
 *   # comment (only a '#' at the start of a line)
 *   key = value
 *
 * Whitespace around the key and the value is ignored. Values run to the end
 * of the line, so they may contain '#' and ',' freely.
 */
#ifndef CONF_H
#define CONF_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
	const char *file;
	int line;
} ConfPos;

/* Called once per "key = value" line. Return 0 on success, non-zero if the
 * line was rejected (the handler should have called conf_error()). */
typedef int (*ConfHandler)(const char *key, char *val, const ConfPos *pos, void *ud);

/* Parse a file. Returns the number of rejected lines, or -1 if the file
 * could not be opened (errno is set). */
int conf_load(const char *path, ConfHandler handler, void *ud);

/* Log "file:line: message" to stderr and count it as a config error. */
void conf_error(const ConfPos *pos, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));

/* Messages from conf_error() are also kept (the most recent 32) so they can be
 * shown later, e.g. over IPC. */
void conf_errors_clear(void);
int conf_errors_count(void);
const char *conf_errors_get(int i);
/* While capturing, conf_error() writes the bare message into buf instead of
 * logging it. Pass NULL to stop. */
void conf_capture(char *buf, size_t len);

/* In-place helpers. All return 0 on success, -1 on failure. */
char *conf_trim(char *s);
int conf_parse_bool(const char *s, int *out);
int conf_parse_long(const char *s, long lo, long hi, long *out);
int conf_parse_double(const char *s, double lo, double hi, double *out);
/* "#rrggbb", "#rrggbbaa", "0xrrggbbaa" or "rrggbbaa" -> 0xRRGGBBAA */
int conf_parse_color(const char *s, uint32_t *rgba);

/* Split s on `sep` into at most `max` fields; the last field keeps the rest
 * of the string (separators included). Each field is trimmed. Returns the
 * number of fields. */
size_t conf_split(char *s, char sep, char **out, size_t max);

#endif /* CONF_H */
