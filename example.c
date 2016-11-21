#define AMETHYST_IMPLEMENTATION
#include "amethyst.h"

int main(int argc, const char *argv[])
{
	return argc == 2 && PDF_OK(pdf_parse_file(argv[1]));
}
