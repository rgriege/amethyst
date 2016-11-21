#define AMETHYST_IMPLEMENTATION
#include "amethyst.h"

int main(int argc, const char *argv[])
{
	struct pdf pdf = {0};
	int ret = 1;
	if (argc != 2) {
		printf("Usage: parse <file.pdf>\n");
		return 1;
	}
	if (!PDF_OK(pdf_init_from_file(&pdf, argv[1])))
		goto out;

	printf("version: '%u'\n", pdf.version);
	for (size_t i = 0; i < pdf.xref_tbl_sz; ++i) {
		struct pdf_xref *entry = pdf.xref_tbl+i;
		printf("object %u.%u@%lu %s\n", entry->obj_num, entry->generation,
		       entry->offset, entry->in_use ? "in use" : "free");
	}
	ret = 0;

out:
	pdf_free(&pdf);
	return ret;
}
