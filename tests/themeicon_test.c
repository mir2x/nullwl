/* cc -D_POSIX_C_SOURCE=200809L -Isystray tests/themeicon_test.c systray/themeicon.c $(pkg-config --cflags --libs cairo librsvg-2.0) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "themeicon.h"

int main(int argc, char **argv)
{
	if (argc > 1 && !strncmp(argv[1], "--theme=", 8)) { themeicon_set_theme(argv[1] + 8); argv++; argc--; }
	for (int i = 1; i < argc; i++) {
		char *p = themeicon_find(argv[i], NULL, 32);
		ThemeBitmap *bm = themeicon_load(argv[i], NULL, 32);
		printf("%-28s %s  [%s%dx%d]\n", argv[i], p ? p : "(not found)", bm ? "" : "load failed ", bm ? bm->w : 0, bm ? bm->h : 0);
		free(p); themeicon_free(bm);
	}
	return 0;
}
