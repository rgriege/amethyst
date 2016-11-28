parse: example.c amethyst.h
	gcc -Wall -g -DPDF_ZLIB -DPDF_JPEG -o parse example.c -lz -ljpeg

.PHONY: clean
clean:
	rm -f parse
