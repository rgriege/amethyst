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

struct pdf__ctx;

struct pdf
{
	struct pdf__ctx *ctx;
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
	struct pdf_baseobj *obj;
};

enum pdf_objtype
{
	PDF_OBJ_ARR,
	PDF_OBJ_DICT,
	PDF_OBJ_INT,
	PDF_OBJ_NAME,
	PDF_OBJ_REF,
};

struct pdf_obj;
struct pdf_obj_arr
{
	struct pdf_obj *entries;
	size_t sz;
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
		struct pdf_obj_arr arr;
		struct pdf_obj_dict dict;
		struct pdf_obj_int intg;
		struct pdf_obj_name name;
		struct pdf_obj_ref ref;
	};
};

struct pdf_baseobj
{
	struct pdf_obj obj;
	char *stream;
};

struct pdf_dict_entry
{
	char *name;
	struct pdf_obj obj;
};


AMFDEF int pdf_init_from_file(struct pdf *pdf, const char *fname);
AMFDEF int pdf_init_from_stream(struct pdf *pdf, FILE *stream);
AMFDEF struct pdf_baseobj *pdf_get_baseobj(struct pdf *pdf, struct pdf_objid id);
AMFDEF int pdf_page_cnt(struct pdf *pdf);
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

#define PDF_BUF_SZ 256
struct pdf__ctx
{
	char buf[PDF_BUF_SZ];
	size_t ln_sz; // TODO(rgriege): remove me - not worth possible mismatch
	FILE *fp;
	int ints[3], int_cnt;
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

static void pdf__readline(struct pdf__ctx *ctx)
{
	ctx->ln_sz = pdf__getline(ctx->buf, PDF_BUF_SZ, ctx->fp);
	// TODO(rgriege): handle ctx->ln_sz == PDF_BUF_SZ
}

static int pdf__parse_ushort_pair_ex(const char *buf, unsigned short *u0,
                                     unsigned short *u1, char **end)
{
	char *p;
	*u0 = strtoul(buf, &p, 10);
	if (buf == p)
		return 1;
	*u1 = strtoul(p, end, 10);
	if (p == *end)
		return 1;
	return 0;
}

static int pdf__parse_ushort_pair(const char *buf, unsigned short *u0,
                                  unsigned short *u1)
{
	char *end;
	return pdf__parse_ushort_pair_ex(buf, u0, u1, &end);
}


enum pdf__token
{
	PDF_TOK_ARR_BEGIN,
	PDF_TOK_ARR_END,
	PDF_TOK_DICT_BEGIN,
	PDF_TOK_DICT_END,
	PDF_TOK_EOF,
	PDF_TOK_INVALID,
	PDF_TOK_NAME_BEGIN,
	PDF_TOK_NUMERIC,
	PDF_TOK_REF_END,
};

static const char *pdf__token_names[] = {
	"Array begin",
	"Array end",
	"Dictionary begin",
	"Dictionary end",
	"EOF",
	"Invalid",
	"Name begin",
	"Numeric",
	"Ref end",
};

void pdf__reset_buf(struct pdf__ctx *ctx)
{
	ctx->buf[0] = '\0';
	ctx->ln_sz = 0;
}

void pdf__consume_word(struct pdf__ctx *ctx)
{
	unsigned len = ctx->ln_sz;
	int c;
	while ((c = fgetc(ctx->fp)) != EOF && !isspace(c))
		ctx->buf[len++] = c;
	ctx->buf[len] = '\0';
	ctx->ln_sz = len;
}

void pdf__consume_digits(struct pdf__ctx *ctx)
{
	unsigned len = ctx->ln_sz;
	int c;
	while ((c = fgetc(ctx->fp)) != EOF && isdigit(c))
		ctx->buf[len++] = c;
	ungetc(c, ctx->fp);
	ctx->buf[len] = '\0';
	ctx->ln_sz = len;
}

int pdf__consume_int(struct pdf__ctx *ctx, int *val)
{
	PDF_ERRIF(!ctx->int_cnt, 1, "No ints to consume\n");
	*val = ctx->ints[0];
	ctx->ints[0] = ctx->ints[1];
	ctx->ints[1] = ctx->ints[2];
	--ctx->int_cnt;
#ifdef PDF_DEBUG
	PDF_LOG("consume int: %d\n", *val);
#endif
	return 0;
}

#ifdef PDF_DEBUG
enum pdf__token _pdf__next_token(struct pdf__ctx *ctx);
enum pdf__token pdf__next_token(struct pdf__ctx *ctx)
{
	enum pdf__token token = _pdf__next_token(ctx);
	PDF_LOG("token: %s\n", pdf__token_names[token]);
	return token;
}
enum pdf__token _pdf__next_token(struct pdf__ctx *ctx)
#else
enum pdf__token pdf__next_token(struct pdf__ctx *ctx)
#endif
{
	int c;
	pdf__reset_buf(ctx);
	while (1) {
		c = fgetc(ctx->fp);
		if (ctx->int_cnt) {
			switch (c) {
			case '[':
			case ']':
			case '<':
			case '>':
			case EOF:
			case '/':
				ungetc(c, ctx->fp);
				return PDF_TOK_NUMERIC;
			default:
			break;
			}
		}
		switch (c) {
		case '[':
			return PDF_TOK_ARR_BEGIN;
		break;
		case ']':
			return PDF_TOK_ARR_END;
		break;
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
		case '0':
		case '1':
		case '2':
		case '3':
		case '4':
		case '5':
		case '6':
		case '7':
		case '8':
		case '9':
			ctx->buf[ctx->ln_sz++] = c;
			pdf__consume_digits(ctx);
			if (ctx->int_cnt == 3) {
				pdf__consume_int(ctx, &c);
				PDF_LOG("Too many unconsumed ints, skipping\n");
			}
			ctx->ints[ctx->int_cnt++] = atoi(ctx->buf);
			pdf__reset_buf(ctx);
			if (ctx->int_cnt == 3)
				return PDF_TOK_NUMERIC;
		break;
		case 'R':
			PDF_ERRIF(ctx->int_cnt != 2, PDF_TOK_INVALID,
			          "Incorrect int count preceding ref token\n");
			return PDF_TOK_REF_END;
		break;
		default:
			PDF_ERRIF(!isspace(c), PDF_TOK_INVALID,
			          "Unexpected '%c' when looking for token\n", c);
		break;
		}
	}
}

int pdf__read_name(struct pdf__ctx *ctx, char **name)
{
	int ret = 1;
	pdf__consume_word(ctx);
	if (ctx->ln_sz) {
		*name = PDF_MALLOC(ctx->ln_sz+1);
		memcpy(*name, ctx->buf, ctx->ln_sz);
		(*name)[ctx->ln_sz] = '\0';
#ifdef PDF_DEBUG
		PDF_LOG("name: %s\n", *name);
#endif
		ret = 0;
	}
	pdf__reset_buf(ctx);
	return ret;
}

static int pdf__parse_obj_after(struct pdf__ctx *ctx, struct pdf_obj *obj,
                                enum pdf__token token);
static int pdf__parse_arr_body(struct pdf__ctx *ctx,
                               struct pdf_obj_arr *arr)
{
	enum pdf__token token;
	struct pdf_obj *entry;

	PDF_ERRIF(arr->entries || arr->sz, 1, "arr struct not 0-d\n");

	token = pdf__next_token(ctx);
	while (token != PDF_TOK_ARR_END) {
		arr->entries = PDF_REALLOC(arr->entries,
		                           ++arr->sz*sizeof(struct pdf_obj));
		entry = arr->entries + arr->sz - 1;
		if (pdf__parse_obj_after(ctx, entry, token))
			PDF_ERR(1, "failed to parse arr entry obj\n");
		token = pdf__next_token(ctx);
	}
	return 0;
}

static int pdf__parse_obj(struct pdf__ctx *ctx, struct pdf_obj *obj);
static int pdf__parse_dict_body(struct pdf__ctx *ctx,
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

static int pdf__parse_dict(struct pdf__ctx *ctx, struct pdf_obj_dict *dict)
{
	if (pdf__next_token(ctx) != PDF_TOK_DICT_BEGIN)
		PDF_ERR(1, "incorrect dict begin token\n");
	return pdf__parse_dict_body(ctx, dict);
}

static int pdf__parse_obj_after(struct pdf__ctx *ctx, struct pdf_obj *obj,
                                enum pdf__token token)
{
	switch (token) {
	case PDF_TOK_ARR_BEGIN:
		obj->type = PDF_OBJ_ARR;
		obj->arr.entries = NULL;
		obj->arr.sz = 0;
		return pdf__parse_arr_body(ctx, &obj->arr);
	break;
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
	case PDF_TOK_NUMERIC:
		obj->type = PDF_OBJ_INT;
		return pdf__consume_int(ctx, &obj->intg.val);
	break;
	case PDF_TOK_REF_END:
		PDF_ERRIF(ctx->int_cnt != 2, 1, "Invalid object reference\n");
		obj->type = PDF_OBJ_REF;
		obj->ref.id.num = ctx->ints[0];
		obj->ref.id.gen = ctx->ints[1];
#ifdef PDF_DEBUG
		PDF_LOG("pdf_obj_ref: %u.%u\n", obj->ref.id.num, obj->ref.id.gen);
#endif
		ctx->int_cnt = 0;
	break;
	case PDF_TOK_ARR_END:
	case PDF_TOK_DICT_END:
	case PDF_TOK_EOF:
	case PDF_TOK_INVALID:
		PDF_ERR(1, "Invalid token (%s) when parsing obj\n",
		        pdf__token_names[token]);
	break;
	}
	return 0;
}

static int pdf__parse_obj(struct pdf__ctx *ctx, struct pdf_obj *obj)
{
	return pdf__parse_obj_after(ctx, obj, pdf__next_token(ctx));
}

AMFDEF int pdf_init_from_stream(struct pdf *pdf, FILE *stream)
{
	char *p;
	int xref_pos;
	struct pdf_obj_dict trailer = {0};
	struct pdf_obj *obj;

	pdf->ctx = PDF_MALLOC(sizeof(struct pdf__ctx));
	pdf->ctx->buf[0] = '\0';
	pdf->ctx->ln_sz = 0;
	pdf->ctx->fp = stream;
	pdf->ctx->int_cnt = 0;

	PDF_ERRIF(pdf->xref_tbl || pdf->xref_tbl_sz, 1,
	          "pdf struct data not zero-d\n");

	pdf__readline(pdf->ctx);
	if (pdf->ctx->ln_sz != 8 || strncmp(pdf->ctx->buf, "%PDF-1.", 7))
		PDF_ERR(1, "invalid header line\n");

	pdf->version = atoi(pdf->ctx->buf+7);
	if (pdf->version > 7)
		PDF_ERR(1, "invalid PDF version '%u'\n", pdf->version);

	if (fseek(pdf->ctx->fp, -6, SEEK_END))
		PDF_ERR(1, "failed to lookup EOF comment\n");

	pdf__readline(pdf->ctx);
	if (pdf->ctx->ln_sz != 5 || strcmp(pdf->ctx->buf, "%%EOF"))
		PDF_ERR(1, "invalid EOF comment\n");

	if (fseek(pdf->ctx->fp, -20, SEEK_CUR))
		PDF_ERR(1, "failed to lookup xref table position\n");
	pdf->ctx->ln_sz = fread(pdf->ctx->buf, 1, 256, pdf->ctx->fp);
	p = strstr(pdf->ctx->buf, "startxref");
	PDF_ERRIF(!p, 1, "failed to locate xref table position\n");
	xref_pos = atoi(p+9);
	PDF_ERRIF(!xref_pos, 1, "failed to parse xref table position\n");

	if (fseek(pdf->ctx->fp, xref_pos, SEEK_SET))
		PDF_ERR(1, "failed to lookup xref table\n");

	pdf__readline(pdf->ctx);
	if (strcmp(pdf->ctx->buf, "xref"))
		PDF_ERR(1, "xref table not found in assigned location\n");

	pdf__readline(pdf->ctx);
	while (!strstr(pdf->ctx->buf, "trailer")) {
		unsigned short objnum, cnt;
		if (pdf__parse_ushort_pair(pdf->ctx->buf, &objnum, &cnt))
			PDF_ERR(1, "failed to parse xref table section header\n");
		PDF_ERRIF(cnt == 0, 1, "xref table section has 0 objects\n");

		// The first object is always a NULL object, so skip it
		if (!pdf->xref_tbl_sz) {
			pdf__readline(pdf->ctx);
			++objnum;
			--cnt;
		}
		pdf->xref_tbl = PDF_REALLOC(pdf->xref_tbl,
		                              pdf->xref_tbl_sz
		                            + cnt*sizeof(struct pdf_xref));
		for (unsigned short i = 0; i < cnt; ++i) {
			struct pdf_xref *entry = pdf->xref_tbl + pdf->xref_tbl_sz + i;
			unsigned off, gen;
			char in_use, eol[2];
			pdf__readline(pdf->ctx);
			if (sscanf(pdf->ctx->buf, "%10u %5u %c%2c", &off, &gen, &in_use, eol) != 4)
				PDF_ERR(1, "invalid xref table entry '%s'\n", pdf->ctx->buf);
			entry->id.num = objnum + i;
			entry->id.gen = gen;
			entry->offset = off;
			entry->in_use = in_use == 'n';
			entry->obj = NULL;
		}
		pdf->xref_tbl_sz += cnt;
		pdf__readline(pdf->ctx);
	}

	PDF_ERRIF(pdf->xref_tbl_sz < 4, 1,
	          "too few (%lu) objects found in xref table\n",
	          pdf->xref_tbl_sz);

	if (pdf__parse_dict(pdf->ctx, &trailer))
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
	FILE *fp = fopen(fname, "rb");
	PDF_ERRIF(!fp, 1, "failed to open file '%s'\n", fname);
	return pdf_init_from_stream(pdf, fp);
}

AMFDEF struct pdf_baseobj *pdf_get_baseobj(struct pdf *pdf, struct pdf_objid id)
{
	struct pdf_xref *xref_entry = NULL;
	for (size_t i = 0; i < pdf->xref_tbl_sz; ++i) {
		if (   (pdf->xref_tbl+i)->id.num == id.num
		    && (pdf->xref_tbl+i)->id.gen == id.gen) {
			xref_entry = pdf->xref_tbl+i;
			break;
		}
	}
	PDF_ERRIF(!xref_entry, NULL, "No such object\n");

	if (!xref_entry->obj) {
		struct pdf_objid local_id;
		char *id_end;
		if (fseek(pdf->ctx->fp, xref_entry->offset, SEEK_SET))
			PDF_ERR(NULL, "failed to lookup base object\n");
		pdf__readline(pdf->ctx);
		if (pdf__parse_ushort_pair_ex(pdf->ctx->buf, &local_id.num,
		                              &local_id.gen, &id_end))
			PDF_ERR(NULL, "failed to parse base object header\n");
		PDF_ERRIF(id.num != local_id.num || id.gen != local_id.gen, NULL,
		          "base object id mismatch\n");
		PDF_ERRIF(strcmp(id_end, " obj"), NULL, "invalid base object header\n");
		xref_entry->obj = PDF_MALLOC(sizeof(struct pdf_baseobj));
		if (pdf__parse_obj(pdf->ctx, &xref_entry->obj->obj)) {
			free(xref_entry->obj);
			PDF_ERR(NULL, "failed to parse base object properties\n");
		}
	}
	return xref_entry->obj;
}

AMFDEF int pdf_page_cnt(struct pdf *pdf)
{
	struct pdf_baseobj *catalog, *pages;
	struct pdf_obj *pages_ref, *count;

	catalog = pdf_get_baseobj(pdf, pdf->root);
	PDF_ERRIF(!catalog, -1, "failed to retrive Catalog object\n");
	PDF_ERRIF(catalog->obj.type != PDF_OBJ_DICT, -1,
	          "Catalog object is not a dict\n");
	pages_ref = pdf_dict_find(&catalog->obj.dict, "Pages");
	PDF_ERRIF(!pages_ref, -1, "Catalog dict has no Pages property\n");
	PDF_ERRIF(pages_ref->type != PDF_OBJ_REF, -1, "Catalog Pages not a ref\n");
	pages = pdf_get_baseobj(pdf, pages_ref->ref.id);
	PDF_ERRIF(!pages, -1, "failed to retrive Pages object\n");
	PDF_ERRIF(pages->obj.type != PDF_OBJ_DICT, -1,
	          "Pages object is not a dict\n");
	count = pdf_dict_find(&pages->obj.dict, "Count");
	PDF_ERRIF(!count, -1, "Pages dict has no Count property\n");
	PDF_ERRIF(count->type != PDF_OBJ_INT, -1, "Pages Count not an int\n");
	return count->intg.val;
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
	fclose(pdf->ctx->fp);
	PDF_FREE(pdf->ctx);
	PDF_FREE(pdf->xref_tbl);
}

#endif // AMETHYST_IMPLEMENTATION
