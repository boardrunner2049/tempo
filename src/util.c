#include "tempo_internal.h"

int tempo__grow(void **items, uint32_t *cap, uint32_t need, size_t item_size)
{
    uint32_t newcap;
    void *p;

    if (need <= *cap)
        return TEMPO_OK;
    newcap = *cap ? *cap : 8;
    while (newcap < need) {
        if (newcap > 0x7FFFFFFFu)
            return TEMPO_ERR_OVERFLOW;
        newcap *= 2;
    }
    p = realloc(*items, (size_t)newcap * item_size);
    if (!p)
        return TEMPO_ERR_NO_MEMORY;
    /* Zero the new tail; setup code relies on fresh entries being clean. */
    memset((char *)p + (size_t)*cap * item_size, 0,
           (size_t)(newcap - *cap) * item_size);
    *items = p;
    *cap = newcap;
    return TEMPO_OK;
}

char *tempo__strdup(const char *s)
{
    size_t n;
    char *p;

    if (!s)
        return NULL;
    n = strlen(s) + 1;
    p = (char *)malloc(n);
    if (p)
        memcpy(p, s, n);
    return p;
}
