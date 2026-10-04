#include <stdio.h>
#include <string.h>
#include "../frontend/vita/input_csv_reader.h"

int main(void)
{
    char line[5];
    size_t length, raw;
    FILE *input = tmpfile();
    if (!input) return 10;

    if (fwrite("a\0hidden\n", 1, 9, input) != 9) return 11;
    rewind(input);
    if (vita_read_input_line(input, line, sizeof(line), &length, &raw) != -1 ||
        raw != 2) return 12;
    fclose(input);

    input = tmpfile();
    if (!input) return 13;
    if (fwrite("1234", 1, 4, input) != 4) return 14;
    rewind(input);
    if (vita_read_input_line(input, line, sizeof(line), &length, &raw) != 1 ||
        length != 4 || raw != 4 || strcmp(line, "1234")) return 15;
    if (vita_read_input_line(input, line, sizeof(line), &length, &raw) != 0) return 16;
    fclose(input);

    input = tmpfile();
    if (!input) return 17;
    if (fwrite("12345", 1, 5, input) != 5) return 18;
    rewind(input);
    if (vita_read_input_line(input, line, sizeof(line), &length, &raw) != -1 ||
        raw != 5) return 19;
    fclose(input);
    return 0;
}
