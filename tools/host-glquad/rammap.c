/* rammap: prove the host can read the guest's RAM through the file QEMU backed it with.
 *
 * The transport that removes the copies depends on this: qemu/ram.img is the guest's memory, so a
 * host process can mmap it and read a layer where the guest left it, or write a finished frame
 * straight into the surface the display reads. This checks the premise before any of that is built.
 *
 *   rammap <path to ram.img> [needle ...]
 */
#include <fcntl.h>
#include <stdio.h>
#define _GNU_SOURCE
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

int
main(int argc, char **argv)
{
	const char *path = argc > 1 ? argv[1] : "qemu/ram.img";
	int fd = open(path, O_RDONLY);
	struct stat info;
	unsigned char *base;
	size_t found = 0;

	if (fd < 0) {
		printf("rammap: cannot open %s\n", path);
		return 1;
	}
	if (fstat(fd, &info) != 0 || info.st_size < (1 << 20)) {
		printf("rammap: %s is not a memory image\n", path);
		return 1;
	}
	base = mmap(NULL, (size_t)info.st_size, PROT_READ, MAP_SHARED, fd, 0);
	if (base == MAP_FAILED) {
		printf("rammap: cannot map %s (%zu bytes)\n", path, (size_t)info.st_size);
		return 1;
	}
	printf("rammap: mapped %s, %zu bytes\n", path, (size_t)info.st_size);
	for (int i = 2; i < argc; i++) {
		const char *needle = argv[i];
		size_t length = strlen(needle);
		unsigned char *hit = memmem(base, (size_t)info.st_size, needle, length);

		printf("rammap: \"%s\" %s\n", needle, hit != NULL ? "found" : "not found");
		found += hit != NULL ? 1 : 0;
	}
	munmap(base, (size_t)info.st_size);
	close(fd);
	return (int)(argc > 2 && found == 0 ? 2 : 0);
}
