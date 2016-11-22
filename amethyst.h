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
#include <stdio.h>

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

struct pdf_objid
{
	unsigned short num, gen;
};

struct pdf
{
	unsigned short version;
	struct pdf_objid root;
	struct pdf_xref *xref_tbl;
	size_t xref_tbl_sz;
};

struct pdf_xref
{
	struct pdf_objid id;
	size_t offset;
	int in_use;
};

enum pdf_objtype
{
	PDF_OBJ_DICT,
	PDF_OBJ_INT,
	PDF_OBJ_NAME,
	PDF_OBJ_REF,
};

struct pdf_dict_entry;
struct pdf_obj_dict
{
	struct pdf_dict_entry *entries;
	size_t sz;
};

struct pdf_obj_int
{
	int val;
};

struct pdf_obj_name
{
	char *val;
};

struct pdf_obj_ref
{
	struct pdf_objid id;
};

struct pdf_obj
{
	enum pdf_objtype type;
	union
	{
		struct pdf_obj_dict dict;
		struct pdf_obj_int intg;
		struct pdf_obj_name name;
		struct pdf_obj_ref ref;
	};
};

struct pdf_dict_entry
{
	char *name;
	struct pdf_obj obj;
};


AMFDEF int pdf_init_from_file(struct pdf *pdf, const char *fname);
AMFDEF int pdf_init_from_stream(struct pdf *pdf, FILE *stream);
AMFDEF struct pdf_obj* pdf_dict_find(struct pdf_obj_dict *dict,
                                     const char *name);
AMFDEF void pdf_free(struct pdf *pdf);

#ifdef __cplusplus
}
#endif

#endif // AMETHYST_H

#ifdef AMETHYST_IMPLEMENTATION

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#define PDF_ERR(code, ...) { PDF_LOG(__VA_ARGS__); return code; }
#define PDF_ERRIF(cond, code, ...) \
	if (cond) PDF_ERR(code, __VA_ARGS__)

#define BUF_SZ 256
struct pdf_ctx
{
	char buf[BUF_SZ];
	size_t ln_sz;
	FILE *fp;
};

static size_t pdf__getdelim(char buf[], size_t n, int delim, FILE *fp)
{
	size_t i = 0;
	int c;

	while (i < n)
	{
		c = fgetc(fp);
		if (c == EOF || c == delim)
			break;
		buf[i++] = c;
	}

	if (ferror(fp) || feof(fp))
		return -1;

	if (i != n)
		buf[i] = '\0';
	return i;
}

static size_t pdf__getline(char buf[], size_t n, FILE *fp)
{
	return pdf__getdelim(buf, n, '\n', fp);
}

static void pdf__readline(struct pdf_ctx *ctx)
{
	ctx->ln_sz = pdf__getline(ctx->buf, BUF_SZ, ctx->fp);
	// assert(ctx->ln_sz < BUF_SZ);
}

static int pdf__parse_ushort_pair(const char *buf, unsigned short *u0,
                                  unsigned short *u1)
{
	char *p, *q;
	*u0 = strtoul(buf, &p, 10);
	if (buf == p)
		return 1;
	*u1 = strtoul(p, &q, 10);
	if (p == q)
		return 1;
	return 0;
}


enum pdf__token
{
	PDF_TOK_DICT_BEGIN,
	PDF_TOK_DICT_END,
	PDF_TOK_EOF,
	PDF_TOK_INVALID,
	PDF_TOK_NAME_BEGIN,
	PDF_TOK_REF_END,
};

enum pdf__token pdf__next_token(struct pdf_ctx *ctx)
{
	int c = 0;
	ctx->ln_sz = 0;
	while (1) {
		c = fgetc(ctx->fp);
		switch (c) {
		case '<':
			if ((c = fgetc(ctx->fp)) == '<')
				return PDF_TOK_DICT_BEGIN;
			ungetc(c, ctx->fp);
			return PDF_TOK_INVALID;
		break;
		case '>':
			if ((c = fgetc(ctx->fp)) == '>')
				return PDF_TOK_DICT_END;
			ungetc(c, ctx->fp);
			return PDF_TOK_INVALID;
		break;
		case EOF:
			return PDF_TOK_EOF;
		break;
		case '/':
			return PDF_TOK_NAME_BEGIN;
		break;
		case 'R':
			return PDF_TOK_REF_END;
		break;
		default:
			ctx->buf[ctx->ln_sz++] = c;
		break;
		}
	}
}

int pdf__read_name(struct pdf_ctx *ctx, char **name)
{
	unsigned name_len = 0;
	int c;
	while ((c = fgetc(ctx->fp)) != EOF && !isspace(c))
		ctx->buf[name_len++] = c;
	if (name_len) {
		*name = PDF_MALLOC(name_len+1);
		memcpy(*name, ctx->buf, name_len);
		(*name)[name_len] = '\0';
	}
	return !name_len;
}

static int pdf__parse_obj(struct pdf_ctx *ctx, struct pdf_obj *obj);
static int pdf__parse_dict_body(struct pdf_ctx *ctx,
                                struct pdf_obj_dict *dict)
{
	enum pdf__token token;
	struct pdf_dict_entry *entry;

	PDF_ERRIF(dict->entries || dict->sz, 1, "dict struct not 0-d\n");

	token = pdf__next_token(ctx);
	while (token != PDF_TOK_DICT_END) {
		PDF_ERRIF(token != PDF_TOK_NAME_BEGIN, 1,
		          "dict entry should begin with a name begin token\n");
		dict->entries = PDF_REALLOC(dict->entries,
		                            ++dict->sz*sizeof(struct pdf_dict_entry));
		entry = dict->entries + dict->sz - 1;
		if (pdf__read_name(ctx, &entry->name))
			PDF_ERR(1, "failed to parse dict entry name\n");
		if (pdf__parse_obj(ctx, &entry->obj))
			PDF_ERR(1, "failed to parse dict entry obj\n");
		token = pdf__next_token(ctx);
	}
	return 0;
}

static int pdf__parse_dict(struct pdf_ctx *ctx, struct pdf_obj_dict *dict)
{
	if (pdf__next_token(ctx) != PDF_TOK_DICT_BEGIN)
		PDF_ERR(1, "incorrect dict begin token\n");
	return pdf__parse_dict_body(ctx, dict);
}

static int pdf__parse_obj(struct pdf_ctx *ctx, struct pdf_obj *obj)
{
	enum pdf__token token = pdf__next_token(ctx);
	switch (token) {
	case PDF_TOK_DICT_BEGIN:
		obj->type = PDF_OBJ_DICT;
		obj->dict.entries = NULL;
		obj->dict.sz = 0;
		return pdf__parse_dict_body(ctx, &obj->dict);
	break;
	case PDF_TOK_NAME_BEGIN:
		obj->type = PDF_OBJ_NAME;
		return pdf__read_name(ctx, &obj->name.val);
	break;
	case PDF_TOK_REF_END:
		obj->type = PDF_OBJ_REF;
		return pdf__parse_ushort_pair(ctx->buf, &obj->ref.id.num,
		                              &obj->ref.id.gen);
	break;
	case PDF_TOK_DICT_END:
		fseek(ctx->fp, -2, SEEK_CUR);
		obj->type = PDF_OBJ_INT;
		obj->intg.val = atoi(ctx->buf);
	break;
	case PDF_TOK_EOF:
	case PDF_TOK_INVALID:
		PDF_ERR(1, "Got token %d when parsing obj\n", token);
	break;
	}
	return 0;
}

AMFDEF int pdf_init_from_stream(struct pdf *pdf, FILE *stream)
{
	struct pdf_ctx ctx = {0};
	char *p;
	int xref_pos;
	struct pdf_obj_dict trailer = {0};
	struct pdf_obj *obj;

	PDF_ERRIF(pdf->xref_tbl || pdf->xref_tbl_sz, 1,
	          "pdf struct data not zero-d\n");

	ctx.fp = stream;

	pdf__readline(&ctx);
	if (ctx.ln_sz != 8 || strncmp(ctx.buf, "%PDF-1.", 7))
		PDF_ERR(1, "invalid header line\n");

	pdf->version = atoi(ctx.buf+7);
	if (pdf->version > 7)
		PDF_ERR(1, "invalid PDF version '%u'\n", pdf->version);

	if (fseek(ctx.fp, -6, SEEK_END))
		PDF_ERR(1, "failed to lookup EOF comment\n");

	pdf__readline(&ctx);
	if (ctx.ln_sz != 5 || strcmp(ctx.buf, "%%EOF"))
		PDF_ERR(1, "invalid EOF comment\n");

	if (fseek(ctx.fp, -20, SEEK_CUR))
		PDF_ERR(1, "failed to lookup xref table position\n");
	ctx.ln_sz = fread(ctx.buf, 1, 256, ctx.fp);
	p = strstr(ctx.buf, "startxref");
	PDF_ERRIF(!p, 1, "failed to locate xref table position\n");
	xref_pos = atoi(p+9);
	PDF_ERRIF(!xref_pos, 1, "failed to parse xref table position\n");

	if (fseek(ctx.fp, xref_pos, SEEK_SET))
		PDF_ERR(1, "failed to lookup xref table\n");

	pdf__readline(&ctx);
	if (strcmp(ctx.buf, "xref"))
		PDF_ERR(1, "xref table not found in assigned location\n");

	pdf__readline(&ctx);
	while (!strstr(ctx.buf, "trailer")) {
		unsigned short objnum, cnt;
		if (pdf__parse_ushort_pair(ctx.buf, &objnum, &cnt))
			PDF_ERR(1, "failed to parse xref table section header\n");
		PDF_ERRIF(cnt == 0, 1, "xref table section has 0 objects\n");

		// The first object is always a NULL object, so skip it
		if (!pdf->xref_tbl_sz) {
			pdf__readline(&ctx);
			--cnt;
		}
		pdf->xref_tbl = PDF_REALLOC(pdf->xref_tbl,
		                              pdf->xref_tbl_sz
		                            + cnt*sizeof(struct pdf_xref));
		for (unsigned short i = 0; i < cnt; ++i) {
			struct pdf_xref *entry = pdf->xref_tbl + pdf->xref_tbl_sz + i;
			unsigned off, gen;
			char in_use, eol[2];
			pdf__readline(&ctx);
			if (sscanf(ctx.buf, "%10u %5u %c%2c", &off, &gen, &in_use, eol) != 4)
				PDF_ERR(1, "invalid xref table entry '%s'\n", ctx.buf);
			entry->id.num = objnum + i;
			entry->id.gen = gen;
			entry->offset = off;
			entry->in_use = in_use == 'n';
		}
		pdf->xref_tbl_sz += cnt;
		pdf__readline(&ctx);
	}

	PDF_ERRIF(pdf->xref_tbl_sz < 4, 1,
	          "too few (%lu) objects found in xref table\n",
	          pdf->xref_tbl_sz);

	if (pdf__parse_dict(&ctx, &trailer))
		PDF_ERR(1, "failed to parse trailer\n");
	obj = pdf_dict_find(&trailer, "Root");
	PDF_ERRIF(!obj, 1, "trailer dict has no Root entry\n");
	PDF_ERRIF(obj->type != PDF_OBJ_REF, 1,
	          "trailer dict Root entry is not a ref\n");
	pdf->root = obj->ref.id;
	obj = pdf_dict_find(&trailer, "Size");
	PDF_ERRIF(!obj, 1, "trailer dict has no Size entry\n");
	PDF_ERRIF(obj->type != PDF_OBJ_INT, 1,
	          "trailer dict Size entry is not an integer\n");
	PDF_ERRIF(obj->intg.val != pdf->xref_tbl_sz+1, 1,
	          "trailer dict Size (%d) != xref table size (%lu)\n",
		        obj->intg.val, pdf->xref_tbl_sz+1);

	return 0;
}

AMFDEF int pdf_init_from_file(struct pdf *pdf, const char *fname)
{
	int ret;
	FILE *fp = fopen(fname, "rb");
	PDF_ERRIF(!fp, 1, "failed to open file '%s'\n", fname);
	ret = pdf_init_from_stream(pdf, fp);
	fclose(fp);
	return ret;
}

AMFDEF struct pdf_obj* pdf_dict_find(struct pdf_obj_dict *dict,
                                     const char *name)
{
	for (size_t i = 0; i < dict->sz; ++i)
		if (strcmp((dict->entries+i)->name, name) == 0)
			return &(dict->entries+i)->obj;
	return NULL;
}

AMFDEF void pdf_free(struct pdf *pdf)
{
	PDF_FREE(pdf->xref_tbl);
}

#endif // AMETHYST_IMPLEMENTATION
