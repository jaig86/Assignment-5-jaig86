#include "loader.h"

#include <sys/mman.h>
#include <sys/stat.h>
#include <errno.h>
#include <string.h>


/*
 * Load a raw image file using mmap.
 *
 * File layout:
 *
 * [ struct image header ][ pixel array ]
 *
 * The pixels are NOT copied into malloc'd memory.
 * image->pixels points directly into the mmap region.
 */
int loadimage_mmap(char* filename, struct image* image) {

    if (filename == NULL || image == NULL) {
        return -1;
    }

    int fd = open(filename, O_RDONLY);

    if (fd == -1) {
        perror("open");
        return -1;
    }

    struct stat st;

    if (fstat(fd, &st) == -1) {
        perror("fstat");
        close(fd);
        return -1;
    }

    size_t file_size = (size_t)st.st_size;

    if (file_size < sizeof(struct image)) {
        close(fd);
        return -1;
    }

    void* mapping = mmap(
        NULL,
        file_size,
        PROT_READ,
        MAP_SHARED,
        fd,
        0
    );

    close(fd);

    if (mapping == MAP_FAILED) {
        perror("mmap");
        return -1;
    }

    /*
     * Read the image information stored at the beginning
     * of the mapped file.
     */
    struct image* stored_image =
        (struct image*)mapping;

    image->width = stored_image->width;
    image->height = stored_image->height;

    /*
     * IMPORTANT:
     * Do not malloc pixels.
     *
     * The pixel array begins immediately after
     * the stored struct image header.
     */
    image->pixels =
        (struct pixel*)
        ((char*)mapping + sizeof(struct image));

    return 0;
}


/*
 * Save an image into a raw binary mmap-compatible file.
 *
 * File layout:
 *
 * [ struct image header ][ pixel array ]
 */
int saveimage_mmap(char* filename, struct image* image) {

    if (filename == NULL ||
        image == NULL ||
        image->pixels == NULL ||
        image->width <= 0 ||
        image->height <= 0) {

        return -1;
    }

    size_t pixel_bytes =
        sizeof(struct pixel) *
        (size_t)image->width *
        (size_t)image->height;

    size_t file_size =
        sizeof(struct image) + pixel_bytes;

    int fd = open(
        filename,
        O_RDWR | O_CREAT | O_TRUNC,
        0644
    );

    if (fd == -1) {
        perror("open");
        return -1;
    }

    /*
     * Make the file large enough for the
     * header and all image pixels.
     */
    if (ftruncate(fd, (off_t)file_size) == -1) {
        perror("ftruncate");
        close(fd);
        return -1;
    }

    void* mapping = mmap(
        NULL,
        file_size,
        PROT_READ | PROT_WRITE,
        MAP_SHARED,
        fd,
        0
    );

    if (mapping == MAP_FAILED) {
        perror("mmap");
        close(fd);
        return -1;
    }

    /*
     * Copy the image header.
     *
     * Do not save the original pixels pointer because
     * a memory address is meaningless in another process.
     */
    struct image header = *image;
    header.pixels = NULL;

    memcpy(
        mapping,
        &header,
        sizeof(struct image)
    );

    /*
     * Copy pixels immediately after the header.
     */
    memcpy(
        (char*)mapping + sizeof(struct image),
        image->pixels,
        pixel_bytes
    );

    /*
     * Flush changes to disk.
     */
    if (msync(mapping, file_size, MS_SYNC) == -1) {
        perror("msync");
    }

    munmap(mapping, file_size);
    close(fd);

    return 0;
}


/*
 * Read exactly count bytes unless EOF or an error occurs.
 */
static int read_exact(int fd, void *buffer, size_t count) {

    unsigned char *p = buffer;

    while (count > 0) {

        ssize_t n = read(fd, p, count);

        if (n <= 0) {
            return -1;
        }

        p += n;
        count -= (size_t)n;
    }

    return 0;
}


/*
 * Load an uncompressed 24-bit BMP file.
 */
int loadimage(char *filename, struct image *image) {

    if (image == NULL ||
        image->width <= 0 ||
        image->height <= 0) {

        return -1;
    }

    int fd = open(filename, O_RDONLY);

    if (fd == -1) {
        return -1;
    }

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

        fprintf(
            stderr,
            "Invalid BMP or unexpected dimensions: %s\n",
            filename
        );

        close(fd);
        return -1;
    }

    size_t row_bytes =
        (size_t)image->width * 3;

    size_t padding =
        (4 - row_bytes % 4) % 4;

    size_t stride =
        row_bytes + padding;

    size_t pixel_count =
        (size_t)image->width *
        (size_t)image->height;

    row = malloc(stride);

    image->pixels =
        malloc(
            pixel_count *
            sizeof(struct pixel)
        );

    if (row == NULL ||
        image->pixels == NULL ||
        lseek(fd, header.offset, SEEK_SET) == (off_t)-1) {

        free(row);
        free(image->pixels);

        image->pixels = NULL;

        close(fd);

        return -1;
    }

    /*
     * BMP stores rows from bottom to top.
     */
    for (int y = image->height - 1;
         y >= 0;
         y--) {

        if (read_exact(fd, row, stride) != 0) {

            fprintf(
                stderr,
                "Could not read BMP pixel data: %s\n",
                filename
            );

            free(row);
            free(image->pixels);

            image->pixels = NULL;

            close(fd);

            return -1;
        }

        for (int x = 0;
             x < image->width;
             x++) {

            struct pixel *p =
                &image->pixels[
                    x + y * image->width
                ];

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
 * Write exactly count bytes.
 */
static int write_exact(
    int fd,
    const void *buffer,
    size_t count
) {

    const unsigned char *p = buffer;

    while (count > 0) {

        ssize_t n = write(fd, p, count);

        if (n <= 0) {
            return -1;
        }

        p += n;
        count -= (size_t)n;
    }

    return 0;
}


/*
 * Save an image as an uncompressed 24-bit BMP file.
 */
int saveimage(char *filename, struct image *image) {

    if (image == NULL ||
        image->pixels == NULL ||
        image->width <= 0 ||
        image->height <= 0) {

        return 1;
    }

    int fd = open(
        filename,
        O_WRONLY | O_CREAT | O_TRUNC,
        0644
    );

    if (fd == -1) {
        return 1;
    }

    size_t row_bytes =
        (size_t)image->width * 3;

    size_t padding =
        (4 - row_bytes % 4) % 4;

    size_t stride =
        row_bytes + padding;

    unsigned char *row =
        calloc(stride, 1);

    if (row == NULL) {
        close(fd);
        return 1;
    }

    BMPHeader header = {
        0x4D42,
        (uint32_t)(54 + stride * image->height),
        0,
        0,
        54
    };

    BMPInfoHeader info = {
        40,
        (uint32_t)image->width,
        (uint32_t)image->height,
        1,
        24,
        0,
        (uint32_t)(stride * image->height),
        0,
        0,
        0,
        0
    };

    if (write_exact(
            fd,
            &header,
            sizeof(header)
        ) != 0 ||

        write_exact(
            fd,
            &info,
            sizeof(info)
        ) != 0) {

        free(row);
        close(fd);

        return 1;
    }

    /*
     * BMP stores rows from bottom to top.
     */
    for (int y = image->height - 1;
         y >= 0;
         y--) {

        for (int x = 0;
             x < image->width;
             x++) {

            struct pixel p =
                image->pixels[
                    x + y * image->width
                ];

            row[x * 3] =
                (unsigned char)p.r;

            row[x * 3 + 1] =
                (unsigned char)p.g;

            row[x * 3 + 2] =
                (unsigned char)p.b;
        }

        if (write_exact(
                fd,
                row,
                stride
            ) != 0) {

            free(row);
            close(fd);

            return 1;
        }
    }

    free(row);

    return close(fd) == 0 ? 0 : 1;
}