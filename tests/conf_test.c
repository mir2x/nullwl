/* Standalone test for conf.c: cc -D_POSIX_C_SOURCE=200809L -I. tests/conf_test.c conf.c */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "conf.h"

static int n, bad;
static char seen[8][64];

static int h(const char *k, char *v, const ConfPos *p, void *ud)
{
	snprintf(seen[n++], 64, "%d:%s=%s", p->line, k, v);
	if (!strcmp(k, "bad")) { conf_error(p, "nope"); return 1; }
	return 0;
}

int main(void)
{
	const char *path = "/tmp/nullwl_conf_test.conf";
	FILE *f = fopen(path, "w");
	fputs("# comment\n\n  a = 1  \ncolor=#2e7cf7\nbind = SUPER, Return, spawn, foot -e sh -c 'x,y'\nnoequals\nbad=1\n=nokey\n", f);
	fclose(f);
	bad = conf_load(path, h, NULL);
	assert(bad == 3); /* noequals, bad, =nokey */
	assert(n == 4);
	assert(!strcmp(seen[0], "3:a=1"));
	assert(!strcmp(seen[1], "4:color=#2e7cf7"));
	assert(!strcmp(seen[2], "5:bind=SUPER, Return, spawn, foot -e sh -c 'x,y'"));
	assert(conf_load("/nonexistent/x", h, NULL) == -1);

	uint32_t c; int b; long l; double d;
	assert(!conf_parse_color("#2e7cf7", &c) && c == 0x2e7cf7ff);
	assert(!conf_parse_color("0xdededeff", &c) && c == 0xdededeff);
	assert(!conf_parse_color("deDEde80", &c) && c == 0xdedede80);
	assert(conf_parse_color("#12345", &c) && conf_parse_color("#zzzzzz", &c));
	assert(!conf_parse_bool("Yes", &b) && b == 1 && !conf_parse_bool("off", &b) && b == 0);
	assert(conf_parse_bool("maybe", &b));
	assert(!conf_parse_long("10", 0, 100, &l) && l == 10);
	assert(conf_parse_long("101", 0, 100, &l) && conf_parse_long("1x", 0, 100, &l) && conf_parse_long("", 0, 9, &l));
	assert(!conf_parse_double("0.55", 0.05, 0.95, &d) && d == 0.55 && conf_parse_double("2", 0.05, 0.95, &d));

	char s[] = "SUPER, Return, spawn, foot -e sh -c 'x,y'";
	char *f4[4];
	assert(conf_split(s, ',', f4, 4) == 4);
	assert(!strcmp(f4[0], "SUPER") && !strcmp(f4[1], "Return") && !strcmp(f4[2], "spawn"));
	assert(!strcmp(f4[3], "foot -e sh -c 'x,y'"));
	char s2[] = "a, b"; char *f3[4];
	assert(conf_split(s2, ',', f3, 4) == 2 && !strcmp(f3[1], "b"));
	puts("conf_test: all passed");
	return 0;
}
