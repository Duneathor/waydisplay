#ifndef WAYDISPLAY_TEST_CHECK_H
#define WAYDISPLAY_TEST_CHECK_H

#include <stdio.h>
#include <stdlib.h>

#define WD_TEST_CHECK(expr)                                                                                                                \
    do                                                                                                                                     \
    {                                                                                                                                      \
        if (!(expr))                                                                                                                       \
        {                                                                                                                                  \
            fprintf(stderr, "FAIL:%s:%d: %s\n", __FILE__, __LINE__, #expr);                                                             \
            exit(EXIT_FAILURE);                                                                                                            \
        }                                                                                                                                  \
    } while (0)

#endif
