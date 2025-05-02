/*
 * ledctl.c – guest-side helper
 *
 * Compile inside the VM with:
 *   gcc -O2 -o ledctl ledctl.c
 *
 * Usage:
 *   ./ledctl /dev/vfio/0 1   // LED on
 *   ./ledctl /dev/vfio/0 0   // LED off
 *
 * The mediated device appears as /dev/vfio/<N> because QEMU passes the
 * bare vfio fd through vhost-user-pci.  We just write a single byte.
 */
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
	if (argc != 3) {
		fprintf(stderr,
		        "usage: %s <vfio_dev> <0|1>\n"
		        "example: %s /dev/vfio/0 1\n",
		        argv[0], argv[0]);
		return 1;
	}

	int fd = open(argv[1], O_WRONLY);
	if (fd < 0) {
		perror("open");
		return 1;
	}

	unsigned char v = (argv[2][0] == '0') ? 0 : 1;
	if (write(fd, &v, 1) != 1) {
		perror("write");
		close(fd);
		return 1;
	}

	close(fd);
	return 0;
}

