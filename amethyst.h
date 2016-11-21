/*
 * Amethyst PDF parsing library
 */

#ifndef AMETHYST_H
#define AMETHYST_H

#ifdef AMETHYST_STATIC
#define AMFDEF static
#else
#define AMFDEF extern
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define PDF_OK(x) ((x) == 0)

#ifndef PDF_LOG
#define PDF_LOG printf
#endif

AMFDEF int pdf_parse_file(const char *fname);

#ifdef __cplusplus
}
#endif

#endif // AMETHYST_H

#ifdef AMETHYST_IMPLEMENTATION

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

AMFDEF size_t pdf__getdelim(char buf[], size_t n, int delim, FILE *fp)
{
	size_t i = 0;
	int c;

	while (1)
	{
		c = fgetc(fp);
		if (c == EOF || c == delim)
			break;
		buf[i++] = c;
	}

	if (ferror(fp) || feof(fp))
		return -1;

	buf[i] = '\0';
	return i;
}

AMFDEF size_t pdf__getline(char buf[], size_t n, FILE *fp)
{
	return pdf__getdelim(buf, n, '\n', fp);
}

AMFDEF int pdf_parse_file(const char *fname)
{
	FILE *fp;
	char buf[256];
	size_t ln_sz;
	int ret = 1, i;

	fp = fopen(fname, "rb");
	if (!fp) {
		PDF_LOG("failed to open file '%s'\n", fname);
		return 1;
	}

	ln_sz = pdf__getline(buf, 256, fp);
	if (ln_sz == -1) {
		PDF_LOG("failed to read header line\n");
		goto close;
	}

	if (ln_sz != 8 || strncmp(buf, "%PDF-1.", 7)) {
		PDF_LOG("invalid header line\n");
		goto close;
	}

	i = atoi(buf+7);
	if (i <= 0 || i > 7) {
		PDF_LOG("invalid PDF version '%d'\n", i);
		goto close;
	}
	PDF_LOG("PDF version: '%d'\n", i);

	ret = 0;

close:
	fclose(fp);
	return ret;
}

#endif // AMETHYST_IMPLEMENTATION
