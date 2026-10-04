#ifndef PCSX_VITA_INPUT_CSV_READER_H
#define PCSX_VITA_INPUT_CSV_READER_H

#include <stdio.h>
#include <stddef.h>

/* Read one complete row without letting embedded NUL hide trailing bytes. */
static int vita_read_input_line(FILE *file, char *line, size_t capacity,
                                size_t *length, size_t *raw_length)
{
    size_t used = 0, raw = 0;
    int byte;
    int saw_byte = 0;

    while ((byte = fgetc(file)) != EOF) {
        saw_byte = 1;
        raw++;
        if (byte == '\0') {
            *length = used;
            *raw_length = raw;
            return -1;
        }
        if (byte == '\n') break;
        if (used + 1 >= capacity) {
            *length = used;
            *raw_length = raw;
            return -1;
        }
        line[used++] = (char)byte;
    }
    if (ferror(file)) {
        *length = 0;
        *raw_length = raw;
        return -1;
    }
    if (!saw_byte && !used) {
        *length = 0;
        *raw_length = 0;
        return 0;
    }
    line[used] = '\0';
    *length = used;
    *raw_length = raw;
    return 1;
}

#endif
