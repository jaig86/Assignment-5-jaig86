#include "loader.h"
#include <sys/mman.h>
#include <errno.h>


/*
 * Loads an image from a raw image file using memory-mapped I/O.
 *
 * The file is expected to be in the raw format written by
 * saveimage_mmap: a struct image header followed immediately by the
 * pixel data. This is not a BMP file. The file is mapped into memory,
 * the header is copied into image, and the pixel data is copied into
 * a newly allocated buffer.
 *
 * The size of the mapping is computed from image->width and
 * image->height, so the caller must set these to the expected
 * dimensions before calling. They are then overwritten with the
 * values stored in the file.
 *
 * The pixel buffer is allocated here and must be freed by the caller.
 *
 * Returns 0 on success, or -1 if the file cannot be opened or mapped.
 */
int loadimage_mmap(char* filename, struct image* image) {
	return 0;
}

/*
 * Saves an image to a raw image file using memory-mapped I/O.
 *
 * Creates the file with permissions 0644, or truncates it if it
 * already exists, then resizes it to hold a struct image header
 * followed by the pixel data. The file is mapped into memory, the
 * header and pixels are copied into the mapping, and the mapping is
 * synchronously flushed to disk.
 *
 * The output is a raw dump of in-memory structures, not a BMP file,
 * and is intended to be read back with loadimage_mmap on the same
 * platform.
 *
 * Returns 0 on success, or -1 if the file cannot be opened or mapped.
 * A failed flush to disk is reported but still returns 0.
 */
int saveimage_mmap(char* filename, struct image* image) {
	return 0;
}


/* Read exactly count bytes, unless EOF or an error occurs. */
static int read_exact(int fd, void *buffer, size_t count) {
    unsigned char *p = buffer;

    while (count > 0) {
        ssize_t n = read(fd, p, count);
        if (n <= 0) return -1;
        p += n;
        count -= (size_t)n;
    }
    return 0;
}

/*
 * Loads an uncompressed 24-bit BMP file into an image.
 *
 * Returns 0 on success, or -1 if the file cannot be opened or is not
 * a 24-bit BMP.
 */
int loadimage(char *filename, struct image *image) {
    if (image == NULL || image->width <= 0 || image->height <= 0) {
        return -1;
    }

    int fd = open(filename, O_RDONLY);
    if (fd == -1) return -1;

    BMPHeader header;
    BMPInfoHeader info;
    unsigned char *row = NULL;
    image->pixels = NULL;

    if (read_exact(fd, &header, sizeof(header)) != 0 ||
        read_exact(fd, &info, sizeof(info)) != 0 ||
        header.type != 0x4D42 ||
        info.bits != 24 ||
        info.compression != 0 ||
        info.width != (uint32_t)image->width ||
        info.height != (uint32_t)image->height) {
        fprintf(stderr, "Invalid BMP or unexpected dimensions: %s\n", filename);
        close(fd);
        return -1;
    }

    size_t row_bytes = (size_t)image->width * 3;
    size_t padding = (4 - row_bytes % 4) % 4;
    size_t stride = row_bytes + padding;
    size_t pixel_count = (size_t)image->width * image->height;

    row = malloc(stride);
    image->pixels = malloc(pixel_count * sizeof(struct pixel));
    if (row == NULL || image->pixels == NULL ||
        lseek(fd, header.offset, SEEK_SET) == (off_t)-1) {
        free(row);
        free(image->pixels);
        image->pixels = NULL;
        close(fd);
        return -1;
    }

    /* BMP stores its rows from bottom to top. */
    for (int y = image->height - 1; y >= 0; y--) {
        if (read_exact(fd, row, stride) != 0) {
            fprintf(stderr, "Could not read BMP pixel data: %s\n", filename);
            free(row);
            free(image->pixels);
            image->pixels = NULL;
            close(fd);
            return -1;
        }

        for (int x = 0; x < image->width; x++) {
            struct pixel *p = &image->pixels[x + y * image->width];
            p->r = row[x * 3];
            p->g = row[x * 3 + 1];
            p->b = row[x * 3 + 2];
        }
    }

    free(row);
    close(fd);
    return 0;
}

/*
 * Writes all count bytes to fd, retrying if write() writes only part of
 * the buffer. Returns 0 on success and -1 on error.
 */
static int write_exact(int fd, const void *buffer, size_t count) {
    const unsigned char *p = buffer;

    while (count > 0) {
        ssize_t n = write(fd, p, count);
        if (n <= 0) return -1;
        p += n;
        count -= (size_t)n;
    }
    return 0;
}

/*
 * Saves an image to disk as an uncompressed 24-bit BMP file.
 * Returns 0 on success, or 1 if the file cannot be opened.
 */
int saveimage(char *filename, struct image *image) {
    if (image == NULL || image->pixels == NULL ||
        image->width <= 0 || image->height <= 0) {
        return 1;
    }

    int fd = open(filename, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd == -1) return 1;

    size_t row_bytes = (size_t)image->width * 3;
    size_t padding = (4 - row_bytes % 4) % 4;
    size_t stride = row_bytes + padding;
    unsigned char *row = calloc(stride, 1);

    if (row == NULL) {
        close(fd);
        return 1;
    }

    BMPHeader header = {
        0x4D42, (uint32_t)(54 + stride * image->height), 0, 0, 54
    };
    BMPInfoHeader info = {
        40, (uint32_t)image->width, (uint32_t)image->height,
        1, 24, 0, (uint32_t)(stride * image->height),
        0, 0, 0, 0
    };

    if (write_exact(fd, &header, sizeof(header)) != 0 ||
        write_exact(fd, &info, sizeof(info)) != 0) {
        free(row);
        close(fd);
        return 1;
    }

    /* BMP stores its rows from bottom to top. */
    for (int y = image->height - 1; y >= 0; y--) {
        for (int x = 0; x < image->width; x++) {
            struct pixel p = image->pixels[x + y * image->width];
            row[x * 3]     = (unsigned char)p.r;
            row[x * 3 + 1] = (unsigned char)p.g;
            row[x * 3 + 2] = (unsigned char)p.b;
        }

        if (write_exact(fd, row, stride) != 0) {
            free(row);
            close(fd);
            return 1;
        }
    }

    free(row);
    return close(fd) == 0 ? 0 : 1;
}