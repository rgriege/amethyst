#define AMETHYST_IMPLEMENTATION
#include "amethyst.h"

int obj_draw(struct pdf *pdf, struct pdf_objid id,
             struct pdf_obj_dict *xobjects, unsigned indent)
{
	struct pdf_baseobj *contents = pdf_get_baseobj(pdf, id);
	struct pdf_obj *xobj;
	struct ps_ctx ctx = {0};
	struct ps_cmd cmd;
	PDF_ERRIF(!contents, -1,
	          "failed to retrive Page Contents base object\n");
	PDF_ERRIF(!contents->stream, -1, "Page Contents has no stream\n");
	switch (contents->stream_type) {
	case PDF_STREAM_JPEG:
		PDF_LOG("%*s<<jpeg>>\n", 2*indent, "");
		return 0;
	case PDF_STREAM_UNKNOWN:
		PDF_ERR(-1, "unknown stream type\n");
	case PDF_STREAM_CMD:
	break;
	}
	ps_init(&ctx, contents->stream);
	while (ps_exec(&ctx, &cmd) == PS_OK) {
		PDF_LOG("%*s%s", 2*indent, "", ps_cmd_name(cmd.type));
		switch (cmd.type) {
		case PS_CMD_DASH:
			PDF_LOG(" ([");
			for (int i = 0; i < PS_DASH_SZ; ++i) {
				if (cmd.dash.arr[i] != -1)
					PDF_LOG("%d ", cmd.dash.arr[i]);
				else
					break;
			}
			PDF_LOG("] %d) \n", cmd.dash.phase);
		break;
		case PS_CMD_FILL_CMYK:
		case PS_CMD_STROKE_CMYK:
			PDF_LOG(" (%f %f %f %f)\n", cmd.cmyk.c, cmd.cmyk.m, cmd.cmyk.y,
			        cmd.cmyk.k);
		break;
		case PS_CMD_FILL_GRAY:
		case PS_CMD_STROKE_GRAY:
			PDF_LOG(" (%f)\n", cmd.gray.val);
		break;
		case PS_CMD_LINE_TO:
		case PS_CMD_MOVE_TO:
			PDF_LOG(" (%f %f)\n", cmd.pos.x, cmd.pos.y);
		break;
		case PS_CMD_LINE_WIDTH:
			PDF_LOG(" (%f)\n", cmd.line_width.val);
		break;
		case PS_CMD_OBJ:
			PDF_ERRIF(!xobjects, -1, " (%s) no resources\n", cmd.obj.name);
			xobj = pdf_dict_find(xobjects, cmd.obj.name);
			PDF_ERRIF(!xobj, -1, " (%s) not found\n", cmd.obj.name);
			PDF_ERRIF(xobj->type != PDF_OBJ_REF, -1,
			          " (%s) invalid type\n", cmd.obj.name);
			PDF_LOG(" (%s)\n", cmd.obj.name);
			if (obj_draw(pdf, xobj->ref.id, NULL, indent+1))
				return -1;
		break;
		case PS_CMD_RECTANGLE:
			PDF_LOG(" (%f %f %f %f)\n", cmd.rectangle.x, cmd.rectangle.y,
			        cmd.rectangle.width, cmd.rectangle.height);
		break;
		case PS_CMD_SHOW_TEXT:
			PDF_LOG(" (%s)\n", cmd.show_text.str);
		break;
		case PS_CMD_SET_FONT:
			PDF_LOG(" (%s, %d)\n", cmd.set_font.font, cmd.set_font.sz);
		break;
		case PS_CMD_MOVE_TEXT:
			PDF_LOG(" (%f, %f)\n", cmd.pos.x, cmd.pos.y);
		break;
		case PS_CMD_TRANSFORM:
			PDF_LOG(" (%f %f %f %f %f %f)\n", cmd.transform.a,
			        cmd.transform.b, cmd.transform.c, cmd.transform.d,
			        cmd.transform.e, cmd.transform.f);
		break;
		case PS_CMD_FILL:
		case PS_CMD_RESTORE_STATE:
		case PS_CMD_SAVE_STATE:
		case PS_CMD_STROKE:
			PDF_LOG("\n");
		break;
		}
	}
	return 0;
}

int page_draw(struct pdf *pdf, int page_idx)
{
	struct pdf_obj *page, *contents_ref, *resources;
	struct pdf_obj_dict *xobjects = NULL;
	int bounds[4];

	page = pdf_get_page(pdf, page_idx);
	PDF_ERRIF(!page, -1, "failed to retrieve page %d\n", page_idx);

	PDF_ERRIF(pdf_get_page_bounds(pdf, page_idx, bounds), -1,
	          "failed to get page %d bounds\n", page_idx);
	PDF_LOG("bounds: [%d %d %d %d]\n", bounds[0], bounds[1], bounds[2],
	        bounds[3]);

	resources = pdf_dict_find_deref(pdf, &page->dict, "Resources");
	if (resources) {
		PDF_ERRIF(resources->type != PDF_OBJ_DICT, -1,
		          "Page Resources is not a dict\n");
		for (size_t i = 0; i < resources->dict.sz; ++i) {
			if (strcmp(resources->dict.entries[i].name, "XObject") == 0) {
				struct pdf_obj *xobjs = &resources->dict.entries[i].obj;
				PDF_ERRIF(xobjs->type != PDF_OBJ_DICT, -1,
				          "Page Resources XObject is not a dict\n");
				xobjects = &xobjs->dict;
				break;
			}
		}
	}

	contents_ref = pdf_dict_find(&page->dict, "Contents");
	PDF_ERRIF(!contents_ref, -1, "failed to retrieve Page Contents\n");
	if (contents_ref->type == PDF_OBJ_ARR) {
		for (size_t i = 0; i < contents_ref->arr.sz; ++i) {
			struct pdf_obj *elem = contents_ref->arr.entries+i;
			PDF_ERRIF(elem->type != PDF_OBJ_REF, -1,
			          "Page Contents array element is not a valid type\n");
			if (!PDF_OK(obj_draw(pdf, elem->ref.id, xobjects, 0)))
				PDF_ERR(-1, "Failed to draw page contents array element\n");
		}
	} else if (contents_ref->type == PDF_OBJ_REF) {
		if (!PDF_OK(obj_draw(pdf, contents_ref->ref.id, xobjects, 0)))
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
