#define _POSIX_C_SOURCE 200809L
#include <zipc/zipc.h>

#include <stdio.h>
#include <stdlib.h>

zipc_status_t zipc_topology_load_file(const char *filename,
                                      zipc_topology_policy_t policy)
{
    if (filename == NULL || filename[0] == '\0')
        return ZIPC_ERR_INVALID_ARGUMENT;

    FILE *f = fopen(filename, "rb");
    if (f == NULL)
        return ZIPC_ERR_PLATFORM;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return ZIPC_ERR_PLATFORM;
    }
    long n = ftell(f);
    if (n <= 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return ZIPC_ERR_INVALID_ARGUMENT;
    }
    char *text = malloc((size_t)n + 1U);
    if (text == NULL) {
        fclose(f);
        return ZIPC_ERR_PLATFORM;
    }
    size_t got = fread(text, 1U, (size_t)n, f);
    fclose(f);
    if (got != (size_t)n) {
        free(text);
        return ZIPC_ERR_PLATFORM;
    }
    text[got] = '\0';
    zipc_status_t status = zipc_topology_load_string(text, got, policy);
    free(text);
    return status;
}
