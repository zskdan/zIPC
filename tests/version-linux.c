#include <zipc/zipc.h>

#include <stdio.h>
#include <string.h>

#define ZIPC_ASSERT_COMPILE(cond) typedef char zipc_compile_assert[(cond) ? 1 : -1]

ZIPC_ASSERT_COMPILE(ZIPC_VERSION_MAJOR == 0U);
ZIPC_ASSERT_COMPILE(ZIPC_VERSION_MINOR == 2U);
ZIPC_ASSERT_COMPILE(ZIPC_VERSION_PATCH == 0U);

#define ZIPC_EXPECT(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "CHECK failed: %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        return 1; \
    } \
} while (0)

int main(void)
{
    const char *version = zipc_version_string();
    ZIPC_EXPECT(version != NULL);
    ZIPC_EXPECT(strcmp(version, ZIPC_VERSION_STRING) == 0);
    ZIPC_EXPECT(strcmp(version, "0.2.0") == 0);
    printf("PASS: version API returns '%s'\n", version);
    return 0;
}
