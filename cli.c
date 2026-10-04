#include "kernel.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>


int generate_pagefault() {

    const char* filename = "pagefault.bin";

    /*
     * Use several MB so that we have multiple file-backed pages.
     */
    size_t file_size = 16 * 1024 * 1024;

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
     * Give the file physical contents.
     */
    char* buffer = malloc(4096);

    if (buffer == NULL) {
        close(fd);
        return -1;
    }

    memset(buffer, 1, 4096);

    for (size_t offset = 0;
         offset < file_size;
         offset += 4096) {

        if (write(fd, buffer, 4096) != 4096) {

            free(buffer);
            close(fd);
            return -1;
        }
    }

    free(buffer);

    /*
     * Ensure the file data has reached backing storage.
     */
    fsync(fd);

    /*
     * Tell the operating system we no longer need these cached pages.
     * This increases the chance that accessing the mapping below
     * requires actual I/O and therefore causes a major page fault.
     */
    posix_fadvise(
        fd,
        0,
        file_size,
        POSIX_FADV_DONTNEED
    );

    /*
     * Map the file without reading it first.
     */
    char* mapping = mmap(
        NULL,
        file_size,
        PROT_READ,
        MAP_PRIVATE,
        fd,
        0
    );

    if (mapping == MAP_FAILED) {
        perror("mmap");
        close(fd);
        return -1;
    }

    /*
     * Touch pages in the file-backed mapping.
     *
     * volatile prevents the compiler from removing these reads.
     */
    volatile unsigned char value = 0;

    for (size_t offset = 0;
         offset < file_size;
         offset += 4096) {

        value ^= mapping[offset];
    }

    /*
     * Use value so the compiler cannot optimize it away.
     */
    if (value == 255) {
        printf("%u\n", value);
    }

    munmap(mapping, file_size);
    close(fd);

    unlink(filename);

    return 0;
}


int main(int argc, char** argv) {

    if (argc != 6) {
        printf(
            "Incorrect number of arguments. Expected: "
            "./cli <MODE=kernel|mmap|convert|uconvert|fault> "
            "<input_image> <width> <height> <output_image_path>\n"
        );

        return -1;
    }

    char* mode = argv[1];
    char* filepath = argv[2];
    int width = atoi(argv[3]);
    int height = atoi(argv[4]);
    char* output_filepath = argv[5];


    /*
     * MODE 1: kernel
     *
     * Load BMP normally,
     * apply blur kernel,
     * save result as BMP.
     */
    if (strcmp(mode, "kernel") == 0) {

        struct image* image =
            malloc(sizeof(struct image));

        if (image == NULL) {
            return -1;
        }

        image->width = width;
        image->height = height;

        if (loadimage(filepath, image) != 0) {
            free(image);
            return -1;
        }

        int kernel[3][3] = {
            {1, 1, 1},
            {1, 1, 1},
            {1, 1, 1}
        };

        struct image* output =
            apply_kernel(
                image,
                (int*)kernel,
                3,
                1.0f / 9.0f
            );

        if (output == NULL) {

            free(image->pixels);
            free(image);

            return -1;
        }

        int result =
            saveimage(
                output_filepath,
                output
            );

        free(image->pixels);
        free(image);

        free(output->pixels);
        free(output);

        return result;
    }


    /*
     * MODE 2: convert
     *
     * BMP -> mmap-compatible raw binary file
     */
    if (strcmp(mode, "convert") == 0) {

        struct image* image =
            malloc(sizeof(struct image));

        if (image == NULL) {
            return -1;
        }

        image->width = width;
        image->height = height;

        if (loadimage(filepath, image) != 0) {
            free(image);
            return -1;
        }

        int result =
            saveimage_mmap(
                output_filepath,
                image
            );

        free(image->pixels);
        free(image);

        return result;
    }


    /*
     * MODE 3: uconvert
     *
     * mmap-compatible binary file -> BMP
     */
    if (strcmp(mode, "uconvert") == 0) {

        struct image* image =
            malloc(sizeof(struct image));

        if (image == NULL) {
            return -1;
        }

        image->width = width;
        image->height = height;

        if (loadimage_mmap(filepath, image) != 0) {
            free(image);
            return -1;
        }

        /*
         * Remember where the mmap region actually begins.
         * image->pixels points just after the stored struct image.
         */
        void* mapping =
            (char*)image->pixels -
            sizeof(struct image);

        size_t mapping_size =
            sizeof(struct image) +
            sizeof(struct pixel) *
            (size_t)image->width *
            (size_t)image->height;

        int result =
            saveimage(
                output_filepath,
                image
            );

        munmap(mapping, mapping_size);

        free(image);

        return result;
    }


    /*
     * MODE 4: mmap
     *
     * Load raw binary image through mmap,
     * apply blur kernel,
     * save output as BMP.
     */
    if (strcmp(mode, "mmap") == 0) {

        struct image* image =
            malloc(sizeof(struct image));

        if (image == NULL) {
            return -1;
        }

        image->width = width;
        image->height = height;

        if (loadimage_mmap(filepath, image) != 0) {
            free(image);
            return -1;
        }

        void* mapping =
            (char*)image->pixels -
            sizeof(struct image);

        size_t mapping_size =
            sizeof(struct image) +
            sizeof(struct pixel) *
            (size_t)image->width *
            (size_t)image->height;

        int kernel[3][3] = {
            {1, 1, 1},
            {1, 1, 1},
            {1, 1, 1}
        };

        struct image* output =
            apply_kernel(
                image,
                (int*)kernel,
                3,
                1.0f / 9.0f
            );

        if (output == NULL) {

            munmap(mapping, mapping_size);

            free(image);

            return -1;
        }

        int result =
            saveimage(
                output_filepath,
                output
            );

        /*
         * Do NOT free(image->pixels).
         * It belongs to the mmap region.
         */
        munmap(mapping, mapping_size);

        free(image);

        free(output->pixels);
        free(output);

        return result;
    }


    /*
     * MODE 5: fault
     *
     * We will implement this later.
     */
    if (strcmp(mode, "fault") == 0) {
        return generate_pagefault();
    }


    printf("Unknown mode: %s\n", mode);

    return -1;
}