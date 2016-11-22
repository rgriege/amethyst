#define AMETHYST_IMPLEMENTATION
#include "amethyst.h"

int page_draw(struct pdf *pdf, int page_idx)
{
	struct pdf_obj *page, *contents_ref;
	struct pdf_baseobj *contents;
	int bounds[4];

	page = pdf_get_page(pdf, page_idx);
	PDF_ERRIF(!page, -1, "failed to retrieve page %d\n", page_idx);

	PDF_ERRIF(pdf_get_page_bounds(pdf, page_idx, bounds), -1,
	          "failed to get page %d bounds\n", page_idx);
	PDF_LOG("bounds: [%d %d %d %d]\n", bounds[0], bounds[1], bounds[2],
	        bounds[3]);

	contents_ref = pdf_dict_find(&page->dict, "Contents");
	PDF_ERRIF(!contents_ref, -1, "failed to retrieve Page Contents\n");
	PDF_ERRIF(contents_ref->type != PDF_OBJ_REF, -1,
	          "Page Contents is not a reference\n");
	contents = pdf_get_baseobj(pdf, contents_ref->ref.id);
	PDF_ERRIF(!contents, -1,
	          "failed to retrive Page Contents base object\n");
	PDF_ERRIF(!contents->stream, -1, "Page Contents has no stream\n");
	PDF_LOG("stream:\n%s\nendstream\n", contents->stream);
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
