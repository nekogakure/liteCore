#include <stdio.h>
#include <stdlib.h>

int main(void) {
    printf("malloc_printf_test: start\n");

    void *p = malloc(128);
    if (!p) {
        printf("malloc_printf_test: malloc failed\n");
        return 1;
    }
    ((char *)p)[0] = 'H';
    ((char *)p)[1] = 'i';
    ((char *)p)[2] = '\0';

    printf("allocated buffer at %p contains: %s\n", p, (char *)p);

    free(p);

    printf("malloc_printf_test: done\n");
    return 0;
}