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

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PDF_OK(x) ((x) == 0)

#ifndef PDF_MALLOC
#define PDF_MALLOC malloc
#endif

#ifndef PDF_REALLOC
#define PDF_REALLOC realloc
#endif

#ifndef PDF_FREE
#define PDF_FREE free
#endif

#ifndef PDF_LOG
#define PDF_LOG printf
#endif

struct pdf
{
	unsigned short version;
	struct pdf_xref *xref_tbl;
	size_t xref_tbl_sz;
};

struct pdf_xref
{
	unsigned short obj_num;
	size_t offset;
	unsigned short generation;
	int in_use;
};

AMFDEF int pdf_init_from_file(struct pdf *pdf, const char *fname);

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

AMFDEF int pdf_init_from_file(struct pdf *pdf, const char *fname)
{
	FILE *fp;
	char buf[256], *p;
	size_t ln_sz;
	int ret = 1, version, xref_pos;

	if (pdf->xref_tbl || pdf->xref_tbl_sz) {
		PDF_LOG("struct data not zero-d\n");
		return 1;
	}

	fp = fopen(fname, "rb");
	if (!fp) {
		PDF_LOG("failed to open file '%s'\n", fname);
		return 1;
	}

	ln_sz = pdf__getline(buf, 256, fp);
	if (ln_sz != 8 || strncmp(buf, "%PDF-1.", 7)) {
		PDF_LOG("invalid header line\n");
		goto close;
	}

	version = atoi(buf+7);
	if (version <= 0 || version > 7) {
		PDF_LOG("invalid PDF version '%d'\n", version);
		goto close;
	}
	pdf->version = version;
	PDF_LOG("PDF version: '%u'\n", pdf->version);

	if (fseek(fp, -6, SEEK_END)) {
		PDF_LOG("failed to lookup EOF comment\n");
		goto close;
	}

	ln_sz = pdf__getline(buf, 256, fp);
	if (ln_sz != 5 || strcmp(buf, "%%EOF")) {
		PDF_LOG("invalid EOF comment\n");
		goto close;
	}

	if (fseek(fp, -20, SEEK_CUR)) {
		PDF_LOG("failed to lookup xref table position\n");
		goto close;
	}
	ln_sz = fread(buf, 1, 256, fp);
	p = strstr(buf, "startxref");
	if (!p) {
		PDF_LOG("failed to locate xref table position\n");
		goto close;
	}
	xref_pos = atoi(p+9);
	if (!xref_pos) {
		PDF_LOG("failed to parse xref table position\n");
		goto close;
	}

	if (fseek(fp, xref_pos, SEEK_SET)) {
		PDF_LOG("failed to lookup xref table\n");
		goto close;
	}

	pdf__getline(buf, 256, fp);
	if (strcmp(buf, "xref")) {
		PDF_LOG("xref table not found in assigned location\n");
		goto close;
	}

	pdf__getline(buf, 256, fp);
	while (!strstr(buf, "trailer")) {
		unsigned objnum, cnt;
		objnum = strtoul(buf, &p, 10);
		if (buf == p) {
			PDF_LOG("failed to parse xref table section object number\n");
			goto close;
		}
		cnt = strtoul(p, NULL, 10);
		if (!cnt) {
			PDF_LOG("xref table section has 0 objects\n");
			goto close;
		}

		// The first object is always a NULL object, so skip it
		if (!pdf->xref_tbl_sz) {
			pdf__getline(buf, 256, fp);
			--cnt;
		}
		pdf->xref_tbl = realloc(pdf->xref_tbl,
		                        pdf->xref_tbl_sz + cnt*sizeof(struct pdf_xref));
		for (int i = 0; i < cnt; ++i) {
			struct pdf_xref *entry = pdf->xref_tbl + pdf->xref_tbl_sz + i;
			unsigned off, gen;
			char in_use, eol[2];
			pdf__getline(buf, 256, fp);
			sscanf(buf, "%10u %5u %c%2c", &off, &gen, &in_use, eol);
			entry->obj_num = objnum + i;
			entry->offset = off;
			entry->generation = gen;
			entry->in_use = in_use == 'n';
		}
		pdf->xref_tbl_sz += cnt;
		pdf__getline(buf, 256, fp);
	}

	if (pdf->xref_tbl_sz < 4) {
		PDF_LOG("too few (%lu) objects found in xref table\n", pdf->xref_tbl_sz);
		goto close;
	}

	ret = 0;

close:
	fclose(fp);
	return ret;
}

#endif // AMETHYST_IMPLEMENTATION
