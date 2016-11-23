#define AMETHYST_IMPLEMENTATION
#include "amethyst.h"

int obj_draw(struct pdf *pdf, struct pdf_objid id)
{
	struct pdf_baseobj *contents = pdf_get_baseobj(pdf, id);
	struct ps_ctx ctx = {0};
	struct ps_cmd cmd;
	PDF_ERRIF(!contents, -1,
	          "failed to retrive Page Contents base object\n");
	PDF_ERRIF(!contents->stream, -1, "Page Contents has no stream\n");
	ps_init(&ctx, contents->stream);
	PDF_LOG("postscript command stream:\n");
	while (ps_exec(&ctx, &cmd) == PS_OK) {
		PDF_LOG("%s", ps_cmd_names[cmd.type]);
		switch (cmd.type) {
		case PS_CMD_RECTANGLE:
			PDF_LOG(" (%f %f %f %f)\n", cmd.rectangle.x, cmd.rectangle.y,
			        cmd.rectangle.width, cmd.rectangle.height);
		break;
		case PS_CMD_SHOW_TEXT:
			PDF_LOG(" (%s)\n", cmd.show_text.str);
		break;
		case PS_CMD_SET_COLOR_CMYK:
			PDF_LOG(" (%f %f %f %f)\n", cmd.set_color_cmyk.c,
			        cmd.set_color_cmyk.m, cmd.set_color_cmyk.y,
			        cmd.set_color_cmyk.k);
		break;
		case PS_CMD_SET_FONT:
			PDF_LOG(" (%s, %d)\n", cmd.set_font.font, cmd.set_font.sz);
		break;
		case PS_CMD_MOVE_TEXT:
			PDF_LOG(" (%f, %f)\n", cmd.move_text.x, cmd.move_text.y);
		break;
		case PS_CMD_TRANSFORM:
			PDF_LOG(" (%f %f %f %f %f %f)\n", cmd.transform.a,
			        cmd.transform.b, cmd.transform.c, cmd.transform.d,
			        cmd.transform.e, cmd.transform.f);
		break;
		case PS_CMD_FILL:
		case PS_CMD_RESTORE_STATE:
		case PS_CMD_SAVE_STATE:
			PDF_LOG("\n");
		break;
		}
	}
	PDF_LOG("\n");
	return 0;
}

int page_draw(struct pdf *pdf, int page_idx)
{
	struct pdf_obj *page, *contents_ref;
	int bounds[4];

	page = pdf_get_page(pdf, page_idx);
	PDF_ERRIF(!page, -1, "failed to retrieve page %d\n", page_idx);

	PDF_ERRIF(pdf_get_page_bounds(pdf, page_idx, bounds), -1,
	          "failed to get page %d bounds\n", page_idx);
	PDF_LOG("bounds: [%d %d %d %d]\n", bounds[0], bounds[1], bounds[2],
	        bounds[3]);

	contents_ref = pdf_dict_find(&page->dict, "Contents");
	PDF_ERRIF(!contents_ref, -1, "failed to retrieve Page Contents\n");
	if (contents_ref->type == PDF_OBJ_ARR) {
		for (size_t i = 0; i < contents_ref->arr.sz; ++i) {
			struct pdf_obj *elem = contents_ref->arr.entries+i;
			PDF_ERRIF(elem->type != PDF_OBJ_REF, -1,
			          "Page Contents array element is not a valid type\n");
			if (!PDF_OK(obj_draw(pdf, elem->ref.id)))
				PDF_ERR(-1, "Failed to draw page contents array element\n");
		}
	} else if (contents_ref->type == PDF_OBJ_REF) {
		if (!PDF_OK(obj_draw(pdf, contents_ref->ref.id)))
			PDF_ERR(-1, "Failed to draw page contents\n");
	} else
		PDF_ERR(-1, "Page Contents is not a valid type\n");
	return 0;
}

int main(int argc, const char *argv[])
{
	struct pdf pdf = {0};
	int ret = 1, pages;
	if (argc != 2) {
		printf("Usage: parse <file.pdf>\n");
		return 1;
	}
	if (!PDF_OK(pdf_init_from_file(&pdf, argv[1])))
		goto out;

	printf("version: '%u'\n", pdf.version);
	for (size_t i = 0; i < pdf.xref_tbl_sz; ++i) {
		struct pdf_xref *entry = pdf.xref_tbl+i;
		printf("object %u.%u@%lu %s\n", entry->id.num, entry->id.gen,
		       entry->offset, entry->in_use ? "in use" : "free");
	}
	printf("root: %u.%u\n", pdf.root.num, pdf.root.gen);
	pages = pdf_page_cnt(&pdf);
	printf("pages: %d\n", pages);
	for (int i = 0; i < pages; ++i) {
		printf("page %d:\n", i);
		page_draw(&pdf, i);
	}
	ret = 0;

out:
	pdf_free(&pdf);
	return ret;
}
