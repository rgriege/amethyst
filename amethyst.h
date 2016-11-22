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

/*
 * PDF data types
 */

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
	struct pdf_baseobj *baseobj;
};

enum pdf_objtype
{
	PDF_OBJ_ARR,
	PDF_OBJ_DICT,
	PDF_OBJ_HEX,
	PDF_OBJ_INT,
	PDF_OBJ_NAME,
	PDF_OBJ_REF,
	PDF_OBJ_STR,
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

struct pdf_obj_hex
{
	char *val;
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

struct pdf_obj_str
{
	char *val;
};

struct pdf_obj
{
	enum pdf_objtype type;
	union
	{
		struct pdf_obj_arr arr;
		struct pdf_obj_dict dict;
		struct pdf_obj_int intg;
		struct pdf_obj_hex hex;
		struct pdf_obj_name name;
		struct pdf_obj_ref ref;
		struct pdf_obj_str str;
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

/*
 * PDF public functions
 */

AMFDEF int pdf_init_from_file(struct pdf *pdf, const char *fname);
AMFDEF int pdf_init_from_stream(struct pdf *pdf, FILE *stream);
AMFDEF struct pdf_baseobj *pdf_get_baseobj(struct pdf *pdf, struct pdf_objid id);
AMFDEF int pdf_page_cnt(struct pdf *pdf);
AMFDEF struct pdf_obj *pdf_get_page(struct pdf *pdf, int page);
AMFDEF int pdf_get_page_bounds(struct pdf *pdf, int page, int bounds[4]);
AMFDEF struct pdf_obj* pdf_dict_find(struct pdf_obj_dict *dict,
                                     const char *name);
AMFDEF void pdf_free(struct pdf *pdf);

/*
 * PostScript callbacks
 */

static void ps__show_txt(const char *str);
static void ps__set_font(const char *name, int sz);
static void ps__move_txt(float x, float y);

#ifndef PS_SHOW_TXT
#define PS_SHOW_TXT ps__show_txt
#endif

#ifndef PS_SET_FONT
#define PS_SET_FONT ps__set_font
#endif

#ifndef PS_MOVE_TXT
#define PS_MOVE_TXT ps__move_txt
#endif

/*
 * PostScript public functions
 */

/* Modifiable for performance - reverts any modifications it makes */
AMFDEF int pdf_ps_exec(char *stream);

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

/*
 * PDF parser implementation
 */

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
	PDF_TOK_HEX_BEGIN,
	PDF_TOK_HEX_END,
	PDF_TOK_INVALID,
	PDF_TOK_NAME_BEGIN,
	PDF_TOK_NUMERIC,
	PDF_TOK_REF_END,
	PDF_TOK_STR_BEGIN,
	PDF_TOK_STR_END,
};

static const char *pdf__token_names[] = {
	"Array begin",
	"Array end",
	"Dictionary begin",
	"Dictionary end",
	"EOF",
	"Hex begin",
	"Hex end",
	"Invalid",
	"Name begin",
	"Numeric",
	"Ref end",
	"String begin",
	"String end",
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

int pdf__consume_to(struct pdf__ctx *ctx, int end)
{
	unsigned len = ctx->ln_sz;
	int c;
	while ((c = fgetc(ctx->fp)) != EOF && c != end)
		ctx->buf[len++] = c;
	ctx->buf[len] = '\0';
	ctx->ln_sz = len;
	return c != end;
}

int pdf__consume_to_ignoring_ws(struct pdf__ctx *ctx, int end)
{
	unsigned len = ctx->ln_sz;
	int c;
	while ((c = fgetc(ctx->fp)) != EOF && c != end)
		if (!isspace(c))
			ctx->buf[len++] = c;
	ctx->buf[len] = '\0';
	ctx->ln_sz = len;
	return c != end;
}

#ifdef PDF_DEBUG
enum pdf__token pdf__next_token_dbg(struct pdf__ctx *ctx);
enum pdf__token pdf__next_token(struct pdf__ctx *ctx)
{
	enum pdf__token token = pdf__next_token_dbg(ctx);
	PDF_LOG("token: %s\n", pdf__token_names[token]);
	return token;
}
enum pdf__token pdf__next_token_dbg(struct pdf__ctx *ctx)
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
			if ((c = fgetc(ctx->fp)) != '<') {
				ungetc(c, ctx->fp);
				return PDF_TOK_HEX_BEGIN;
			} else
				return PDF_TOK_DICT_BEGIN;
		break;
		case '>':
			if ((c = fgetc(ctx->fp)) != '>') {
				ungetc(c, ctx->fp);
				return PDF_TOK_HEX_END;
			} else
				return PDF_TOK_DICT_END;
		break;
		case '(':
			return PDF_TOK_STR_BEGIN;
		case ')':
			return PDF_TOK_STR_BEGIN;
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

int pdf__move_buf(struct pdf__ctx *ctx, char **dst)
{
	int ret = 1;
	if (ctx->ln_sz) {
		*dst = PDF_MALLOC(ctx->ln_sz+1);
		memcpy(*dst, ctx->buf, ctx->ln_sz);
		(*dst)[ctx->ln_sz] = '\0';
		ret = 0;
	}
	pdf__reset_buf(ctx);
	return ret;
}

#ifdef PDF_DEBUG
int pdf__read_name_dbg(struct pdf__ctx *ctx, char **name);
int pdf__read_name(struct pdf__ctx *ctx, char **name)
{
	int ret = pdf__read_name_dbg(ctx, name);
	if (PDF_OK(ret))
		PDF_LOG("name: %s\n", *name);
	return ret;
}
int pdf__read_name_dbg(struct pdf__ctx *ctx, char **name)
#else
int pdf__read_name(struct pdf__ctx *ctx, char **name)
#endif
{
	pdf__consume_word(ctx);
	return pdf__move_buf(ctx, name);
}

#ifdef PDF_DEBUG
static int pdf__read_str_dbg(struct pdf__ctx *ctx, char **str);
static int pdf__read_str(struct pdf__ctx *ctx, char **str)
{
	int ret = pdf__read_str_dbg(ctx, str);
	if (PDF_OK(ret))
		PDF_LOG("str: %s\n", *str);
	return ret;
}
static int pdf__read_str_dbg(struct pdf__ctx *ctx, char **str)
#else
static int pdf__read_str(struct pdf__ctx *ctx, char **str)
#endif
{
	if (pdf__consume_to(ctx, ')'))
		PDF_ERR(1, "Error reading string\n");
	return pdf__move_buf(ctx, str);
}

#ifdef PDF_DEBUG
static int pdf__read_hex_dbg(struct pdf__ctx *ctx, struct pdf_obj_hex *hex);
static int pdf__read_hex(struct pdf__ctx *ctx, struct pdf_obj_hex *hex)
{
	int ret = pdf__read_hex_dbg(ctx, hex);
	if (PDF_OK(ret)) {
		PDF_LOG("hex: ");
		for (int i = 0; i < hex->sz; ++i)
			PDF_LOG("%02x", hex->val[i] & 0xff);
		PDF_LOG("\n");
	}
	return ret;
}
static int pdf__read_hex_dbg(struct pdf__ctx *ctx, struct pdf_obj_hex *hex)
#else
static int pdf__read_hex(struct pdf__ctx *ctx, struct pdf_obj_hex *hex)
#endif
{
	int ret = 1;
	if (pdf__consume_to_ignoring_ws(ctx, '>'))
		PDF_ERR(1, "Error reading hex string\n");
	if (ctx->ln_sz) {
		hex->sz = (ctx->ln_sz+1)/2;
		hex->val = PDF_MALLOC(hex->sz);
		for (int i = 0; i < hex->sz; ++i) {
			char c = ctx->buf[2*(i+1)];
			ctx->buf[2*(i+1)] = '\0';
			hex->val[i] = strtol(ctx->buf+2*i, NULL, 16);
			ctx->buf[2*(i+1)] = c;
		}
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
	case PDF_TOK_HEX_BEGIN:
		obj->type = PDF_OBJ_HEX;
		return pdf__read_hex(ctx, &obj->hex);
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
	case PDF_TOK_STR_BEGIN:
		obj->type = PDF_OBJ_STR;
		return pdf__read_str(ctx, &obj->hex.val);
	case PDF_TOK_ARR_END:
	case PDF_TOK_DICT_END:
	case PDF_TOK_HEX_END:
	case PDF_TOK_STR_END:
	case PDF_TOK_EOF:
		PDF_ERR(1, "Unexpected token (%s) when parsing obj\n",
		        pdf__token_names[token]);
	case PDF_TOK_INVALID:
		PDF_ERR(1, "Invalid token (%c) when parsing obj\n", fgetc(ctx->fp));
	}
	return 0;
}

static int pdf__parse_obj(struct pdf__ctx *ctx, struct pdf_obj *obj)
{
	return pdf__parse_obj_after(ctx, obj, pdf__next_token(ctx));
}

static int pdf__validate_trailer(struct pdf *pdf,
                                 struct pdf_obj_dict *trailer)
{
	struct pdf_obj *obj;

	if (pdf__parse_dict(pdf->ctx, trailer))
		PDF_ERR(1, "failed to parse trailer\n");
	obj = pdf_dict_find(trailer, "Root");
	PDF_ERRIF(!obj, 1, "trailer dict has no Root entry\n");
	PDF_ERRIF(obj->type != PDF_OBJ_REF, 1,
	          "trailer dict Root entry is not a ref\n");
	pdf->root = obj->ref.id;
	obj = pdf_dict_find(trailer, "Size");
	PDF_ERRIF(!obj, 1, "trailer dict has no Size entry\n");
	PDF_ERRIF(obj->type != PDF_OBJ_INT, 1,
	          "trailer dict Size entry is not an integer\n");
	PDF_ERRIF(obj->intg.val != pdf->xref_tbl_sz+1, 1,
	          "trailer dict Size (%d) != xref table size (%lu)\n",
		        obj->intg.val, pdf->xref_tbl_sz+1);

	return 0;
}

static void pdf__free_obj(struct pdf_obj *obj);
static void pdf__free_dict(struct pdf_obj_dict *dict)
{
	for (size_t i = 0; i < dict->sz; ++i) {
		struct pdf_dict_entry *entry = dict->entries+i;
		PDF_FREE(entry->name);
		pdf__free_obj(&entry->obj);
	}
	PDF_FREE(dict->entries);
}

AMFDEF int pdf_init_from_stream(struct pdf *pdf, FILE *stream)
{
	char *p;
	int xref_pos, ret;
	struct pdf_obj_dict trailer = {0};

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
			entry->baseobj = NULL;
		}
		pdf->xref_tbl_sz += cnt;
		pdf__readline(pdf->ctx);
	}

	PDF_ERRIF(pdf->xref_tbl_sz < 4, 1,
	          "too few (%lu) objects found in xref table\n",
	          pdf->xref_tbl_sz);

	ret = pdf__validate_trailer(pdf, &trailer);
	pdf__free_dict(&trailer);
	return ret;
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

	if (!xref_entry->baseobj) {
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
		xref_entry->baseobj = PDF_MALLOC(sizeof(struct pdf_baseobj));
		if (pdf__parse_obj(pdf->ctx, &xref_entry->baseobj->obj)) {
			free(xref_entry->baseobj);
			PDF_ERR(NULL, "failed to parse base object properties\n");
		}
		PDF_ERRIF(fgetc(pdf->ctx->fp) != '\n', NULL,
		          "expected new line after base object obj\n");
		pdf__readline(pdf->ctx);
		if (strcmp(pdf->ctx->buf, "stream") == 0) {
			struct pdf_obj *obj = &xref_entry->baseobj->obj, *length;

			PDF_ERRIF(obj->type != PDF_OBJ_DICT, NULL,
			          "base object has stream but no properties\n");
			length = pdf_dict_find(&obj->dict, "Length");
			PDF_ERRIF(!length, NULL, "base object has stream but no Length\n");
			PDF_ERRIF(length->type != PDF_OBJ_INT, NULL,
			          "base object Length is not an int\n");
			xref_entry->baseobj->stream = PDF_MALLOC(length->intg.val+1);
			fread(xref_entry->baseobj->stream, 1, length->intg.val,
			      pdf->ctx->fp);
			xref_entry->baseobj->stream[length->intg.val] = '\0';
			PDF_ERRIF(fgetc(pdf->ctx->fp) != '\n', NULL,
			          "expected new line after base object stream\n");
			pdf__readline(pdf->ctx);
			PDF_ERRIF(strcmp(pdf->ctx->buf, "endstream"), NULL,
			          "missing endstream token\n");
			pdf__readline(pdf->ctx);
		}
		else
			xref_entry->baseobj->stream = NULL;
		PDF_ERRIF(strcmp(pdf->ctx->buf, "endobj"), NULL,
		          "missing endobj token\n");
	}
	return xref_entry->baseobj;
}

static struct pdf_obj *pdf__pages(struct pdf *pdf)
{
	struct pdf_baseobj *catalog, *pages;
	struct pdf_obj *pages_ref;

	catalog = pdf_get_baseobj(pdf, pdf->root);
	PDF_ERRIF(!catalog, NULL, "failed to retrive Catalog object\n");
	PDF_ERRIF(catalog->obj.type != PDF_OBJ_DICT, NULL,
	          "Catalog object is not a dict\n");
	pages_ref = pdf_dict_find(&catalog->obj.dict, "Pages");
	PDF_ERRIF(!pages_ref, NULL, "Catalog dict has no Pages property\n");
	PDF_ERRIF(pages_ref->type != PDF_OBJ_REF, NULL,
	          "Catalog Pages not a ref\n");
	pages = pdf_get_baseobj(pdf, pages_ref->ref.id);
	PDF_ERRIF(!pages, NULL, "failed to retrive Pages object\n");
	PDF_ERRIF(pages->obj.type != PDF_OBJ_DICT, NULL,
	          "Pages object is not a dict\n");
	return &pages->obj;
}

AMFDEF int pdf_page_cnt(struct pdf *pdf)
{
	struct pdf_obj *pages, *count;

	pages = pdf__pages(pdf);
	PDF_ERRIF(!pages, -1, "failed to retrive Pages object\n");
	count = pdf_dict_find(&pages->dict, "Count");
	PDF_ERRIF(!count, -1, "Pages dict has no Count property\n");
	PDF_ERRIF(count->type != PDF_OBJ_INT, -1, "Pages Count not an int\n");
	return count->intg.val;
}

AMFDEF struct pdf_obj *pdf_get_page(struct pdf *pdf, int page_idx)
{
	struct pdf_obj *pages, *kids, *page_ref;
	struct pdf_baseobj *page;

	pages = pdf__pages(pdf);
	PDF_ERRIF(!pages, NULL, "failed to retrive Pages object\n");
	kids = pdf_dict_find(&pages->dict, "Kids");
	PDF_ERRIF(!kids, NULL, "failed to retrieve Pages Kids\n");
	PDF_ERRIF(kids->type != PDF_OBJ_ARR, NULL,
	          "Pages Kids is not an array\n");
	PDF_ERRIF(page_idx < 0 || page_idx >= kids->arr.sz, NULL,
	          "Invalid page num\n");
	page_ref = kids->arr.entries+page_idx;
	PDF_ERRIF(page_ref->type != PDF_OBJ_REF, NULL,
	          "Pages Kids element is not a reference\n");
	page = pdf_get_baseobj(pdf, page_ref->ref.id);
	PDF_ERRIF(!page, NULL, "failed to get Page %i baseobj\n", page_idx);
	return &page->obj;
}

AMFDEF int pdf_get_page_bounds(struct pdf *pdf, int page_idx, int bounds[4])
{
	struct pdf_obj *page, *box;
	page = pdf_get_page(pdf, page_idx);
	PDF_ERRIF(!page, 1, "failed to get Page %i\n", page_idx);
	box = pdf_dict_find(&page->dict, "MediaBox");
	if (!box) {
		struct pdf_obj *parent_ref;
		struct pdf_baseobj *pages;
		parent_ref = pdf_dict_find(&page->dict, "Parent");
		PDF_ERRIF(!parent_ref, 1, "Page %i missing Parent\n", page_idx);
		PDF_ERRIF(parent_ref->type != PDF_OBJ_REF, 1,
		          "Page %i Parent is not a reference\n", page_idx);
		pages = pdf_get_baseobj(pdf, parent_ref->ref.id);
		PDF_ERRIF(!pages, 1, "failed to locate Pages baseobj\n");
		PDF_ERRIF(pages->obj.type != PDF_OBJ_DICT, 1,
		          "Pages baseobj obj is not a dict\n");
		box = pdf_dict_find(&pages->obj.dict, "MediaBox");
		PDF_ERRIF(!box, 1, "No MediaBox in Pages or Page %i\n", page_idx);
	}
	PDF_ERRIF(box->type != PDF_OBJ_ARR, 1, "MediaBox is not an array\n");
	PDF_ERRIF(box->arr.sz != 4, 1, "MediaBox does not have 4 elements\n");
	for (size_t i = 0; i < box->arr.sz; ++i) {
		struct pdf_obj *dim = box->arr.entries+i;
		PDF_ERRIF(dim->type != PDF_OBJ_INT, 1,
		          "MediaBox[%lu] is not an int\n", i);
		bounds[i] = dim->intg.val;
	}
	return 0;
}

AMFDEF struct pdf_obj* pdf_dict_find(struct pdf_obj_dict *dict,
                                     const char *name)
{
	for (size_t i = 0; i < dict->sz; ++i)
		if (strcmp((dict->entries+i)->name, name) == 0)
			return &(dict->entries+i)->obj;
	return NULL;
}

static void pdf__free_obj(struct pdf_obj *obj)
{
	switch (obj->type) {
	case PDF_OBJ_ARR:
		for (size_t i = 0; i < obj->arr.sz; ++i)
			pdf__free_obj(obj->arr.entries+i);
		PDF_FREE(obj->arr.entries);
	break;
	case PDF_OBJ_DICT:
		pdf__free_dict(&obj->dict);
	break;
	case PDF_OBJ_HEX:
		PDF_FREE(obj->hex.val);
	break;
	case PDF_OBJ_INT:
	break;
	case PDF_OBJ_NAME:
		PDF_FREE(obj->name.val);
	break;
	case PDF_OBJ_REF:
	break;
	case PDF_OBJ_STR:
		PDF_FREE(obj->str.val);
	break;
	}
}

AMFDEF void pdf_free(struct pdf *pdf)
{
	fclose(pdf->ctx->fp);
	PDF_FREE(pdf->ctx);
	if (pdf->xref_tbl) {
		for (size_t i = 0; i < pdf->xref_tbl_sz; ++i) {
			struct pdf_xref *xref = pdf->xref_tbl + i;
			if (xref->baseobj) {
				pdf__free_obj(&xref->baseobj->obj);
				PDF_FREE(xref->baseobj->stream);
				PDF_FREE(xref->baseobj);
			}
		}
		PDF_FREE(pdf->xref_tbl);
	}
}

/*
 * Postscript parser implementation
 */

enum ps__argtype
{
	PS_ARG_ARR,
	PS_ARG_NAME,
	PS_ARG_REAL,
	PS_ARG_STR,
};

struct ps__arg_arr
{
	struct ps__arg *entries;
	size_t sz;
	struct ps__arg_arr* parent;
};

struct ps__arg_scalar
{
	char *start, *end;
};

struct ps__arg
{
	enum ps__argtype type;
	union
	{
		struct ps__arg_arr arr;
		struct ps__arg_scalar val;
	};
};

enum ps__txt_cmd
{
	PS_TXT_CMD_END,
	PS_TXT_CMD_EOS,
	PS_TXT_CMD_INVALID,
	PS_TXT_CMD_MOVE,
	PS_TXT_CMD_FONT,
	PS_TXT_CMD_SHOW,
};

static void ps__consume_ws(char **stream)
{
	while (isspace(**stream) && **stream != '\0')
		++*stream;
}

static void ps__consume_word(char **stream)
{
	while (!isspace(**stream) && **stream != '\0')
		++*stream;
}

static void ps__consume_digits(char **stream)
{
	while (isdigit(**stream) && **stream != '\0')
		++*stream;
}

static int ps__consume_to(char **stream, char c)
{
	while (**stream != c && **stream != '\0')
		++*stream;
	return **stream == '\0';
}

static int ps__txt_next_cmd(char **stream, struct ps__arg_arr *args)
{
	struct ps__arg_arr *cur_arr = args;
	while (1) {
		ps__consume_ws(stream);
		if (**stream == '/') {
			struct ps__arg *arg;
			++cur_arr->sz;
			cur_arr->entries = PDF_REALLOC(cur_arr->entries,
			                               cur_arr->sz*sizeof(struct ps__arg));
			arg = cur_arr->entries + cur_arr->sz - 1;
			arg->type = PS_ARG_NAME;
			arg->val.start = *stream;
			ps__consume_word(stream);
			arg->val.end = *stream;
		} else if (**stream >= '0' && **stream <= '9') {
			struct ps__arg *arg;
			++cur_arr->sz;
			cur_arr->entries = PDF_REALLOC(cur_arr->entries,
			                               cur_arr->sz*sizeof(struct ps__arg));
			arg = cur_arr->entries + cur_arr->sz - 1;
			arg->type = PS_ARG_REAL;
			arg->val.start = *stream;
			ps__consume_digits(stream);
			arg->val.end = *stream;
		} else if (**stream == '(') {
			struct ps__arg *arg;
			++cur_arr->sz;
			cur_arr->entries = PDF_REALLOC(cur_arr->entries,
			                               cur_arr->sz*sizeof(struct ps__arg));
			arg = cur_arr->entries + cur_arr->sz - 1;
			arg->type = PS_ARG_STR;
			arg->val.start = ++*stream;
			if (ps__consume_to(stream, ')'))
				PDF_ERR(PS_TXT_CMD_INVALID, "Unterminated text string\n");
			arg->val.end = *stream;
			++*stream;
		} else if (**stream == '[') {
			struct ps__arg *arg;
			++cur_arr->sz;
			cur_arr->entries = PDF_REALLOC(cur_arr->entries,
			                               cur_arr->sz*sizeof(struct ps__arg));
			arg = cur_arr->entries + cur_arr->sz - 1;
			arg->type = PS_ARG_ARR;
			arg->arr.entries = NULL;
			arg->arr.sz = 0;
			arg->arr.parent = cur_arr;
			cur_arr = &arg->arr;
		} else if (**stream == ']') {
			cur_arr = cur_arr->parent;
			PDF_ERRIF(!cur_arr, PS_TXT_CMD_INVALID,
			          "Unexpected end of array text token\n");
		} else {
			const char *start = *stream;
			ps__consume_word(stream);
			if (*stream - start == 2 && strncmp(start, "Td", 2) == 0)
				return PS_TXT_CMD_MOVE;
			else if (*stream - start == 2 && strncmp(start, "Tf", 2) == 0)
				return PS_TXT_CMD_FONT;
			else if (*stream - start == 2 && strncmp(start, "Tj", 2) == 0)
				return PS_TXT_CMD_SHOW;
			else if (*stream - start == 2 && strncmp(start, "ET", 2) == 0)
				return PS_TXT_CMD_END;
			else if (*stream == '\0')
				return PS_TXT_CMD_EOS;
			PDF_ERR(PS_TXT_CMD_INVALID, "Unknown command '%.*s'\n",
			        (int)(*stream - start), start);
		}
	}
}

static int ps__txt_begin(char **stream)
{
	struct ps__arg_arr args = {0};
	int err = 0, quit = 0;
	char c, d;

	while (!quit) {
		switch (ps__txt_next_cmd(stream, &args)) {
		case PS_TXT_CMD_END:
			quit = 1;
		break;
		case PS_TXT_CMD_EOS:
			err = quit = 1;
			PDF_LOG("Unexpected end of text stream\n");
		break;
		case PS_TXT_CMD_FONT:
			if (   args.sz == 2
			    && args.entries[0].type == PS_ARG_NAME
			    && args.entries[1].type == PS_ARG_REAL) {
				c = *args.entries[0].val.end;
				*args.entries[0].val.end = '\0';
				d = *args.entries[1].val.end;
				*args.entries[1].val.end = '\0';
				PS_SET_FONT(args.entries[0].val.start,
				            atoi(args.entries[1].val.start));
				*args.entries[0].val.end = c;
				*args.entries[1].val.end = d;
				args.sz = 0;
			} else {
				PDF_LOG("Set font cmd called with incorrect params\n");
				err = quit = 1;
			}
		break;
		case PS_TXT_CMD_INVALID:
			err = quit = 1;
			PDF_LOG("Error while reading ps text stream\n");
		break;
		case PS_TXT_CMD_MOVE:
			if (   args.sz == 2
			    && args.entries[0].type == PS_ARG_REAL
			    && args.entries[1].type == PS_ARG_REAL) {
				c = *args.entries[0].val.end;
				*args.entries[0].val.end = '\0';
				d = *args.entries[1].val.end;
				*args.entries[1].val.end = '\0';
				PS_MOVE_TXT(strtof(args.entries[0].val.start, NULL),
				            strtof(args.entries[1].val.start, NULL));
				*args.entries[0].val.end = c;
				*args.entries[1].val.end = d;
				args.sz = 0;
			} else {
				PDF_LOG("Set font cmd called with incorrect params\n");
				err = quit = 1;
			}
		break;
		case PS_TXT_CMD_SHOW:
			if (args.sz == 1 && args.entries[0].type == PS_ARG_STR) {
				c = *args.entries[0].val.end;
				*args.entries[0].val.end = '\0';
				PS_SHOW_TXT(args.entries[0].val.start);
				*args.entries[0].val.end = c;
				args.sz = 0;
			} else {
				PDF_LOG("Show text cmd called with incorrect params\n");
				err = quit = 1;
			}
		break;
		}
	}
	PDF_FREE(args.entries);
	return err;
}

enum ps__cmd
{
	PS_CMD_END,
	PS_CMD_INVALID,
	PS_CMD_TXT_BEGIN,
};

static enum ps__cmd ps__next_cmd(char **stream)
{
	const char *start;
	ps__consume_ws(stream);
	start = *stream;
	ps__consume_word(stream);
	if (*stream - start == 2 && strncmp(start, "BT", 2) == 0)
		return PS_CMD_TXT_BEGIN;
	if (**stream == '\0')
		return PS_CMD_END;
	PDF_LOG("Invalid cmd token '%.*s'\n", (int)(*stream - start), start);
	return PS_CMD_INVALID;
}

AMFDEF int pdf_ps_exec(char *str)
{
	char **stream = &str;
	int err = 0, quit = 0;

	while (!quit) {
		switch (ps__next_cmd(stream)) {
		case PS_CMD_END:
			quit = 1;
		break;
		case PS_CMD_TXT_BEGIN:
			if (ps__txt_begin(stream))
				err = quit = 1;
		break;
		case PS_CMD_INVALID:
			err = quit = 1;
			PDF_LOG("Error while reading ps stream\n");
		break;
		}
	}
	return err;
}

static void ps__show_txt(const char *txt)
{
	PDF_LOG("txt: %s\n", txt);
}

static void ps__set_font(const char *name, int sz)
{
	PDF_LOG("Ignoring set font (%s, %d) command\n", name, sz);
}

static void ps__move_txt(float x, float y)
{
	PDF_LOG("Ignoring text move (%f, %f) command\n", x, y);
}

#endif // AMETHYST_IMPLEMENTATION
