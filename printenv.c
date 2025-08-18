#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s VAR_NAME\n", argv[0]);
        return 1;
    }

    const char *val = getenv(argv[1]);
    if (val)
        printf("%s\n", val);
    else
        printf("Variable not found\n");

    return 0;
}

