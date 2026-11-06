/* Minimal test assertions that stay active regardless of NDEBUG. */
#ifndef MC_CHECK_H
#define MC_CHECK_H

#include <stdio.h>
#include <stdlib.h>

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__,   \
                    #cond);                                                    \
            exit(1);                                                           \
        }                                                                      \
    } while (0)

#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

#endif
