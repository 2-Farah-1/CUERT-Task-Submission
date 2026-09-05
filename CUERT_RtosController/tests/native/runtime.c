/* Minimal host runtime for the Windows SDK-less MSVC test runner only.
 * Production firmware still uses the toolchain's standard C library.
 */
#include <stddef.h>
static void (*failure_handler)(const char *);
int main(void);
void TestFail(const char *message) { failure_handler(message); }
void abort(void) { TestFail("abort"); }
int puts(const char *message) { (void)message; return 0; }
void *memcpy(void *dest, const void *source, size_t count)
{
    unsigned char *out = dest;
    const unsigned char *in = source;
    for (size_t i = 0; i < count; ++i) { out[i] = in[i]; }
    return dest;
}
void *memmove(void *dest, const void *source, size_t count)
{
    unsigned char *out = dest;
    const unsigned char *in = source;
    if (out < in) {
        for (size_t i = 0; i < count; ++i) { out[i] = in[i]; }
    } else {
        while (count != 0) { --count; out[count] = in[count]; }
    }
    return dest;
}
int memcmp(const void *left, const void *right, size_t count)
{
    const unsigned char *a = left, *b = right;
    for (size_t i = 0; i < count; ++i) {
        if (a[i] != b[i]) { return (int)a[i] - (int)b[i]; }
    }
    return 0;
}
int strcmp(const char *left, const char *right)
{
    while (*left && *left == *right) { ++left; ++right; }
    return (int)(unsigned char)*left - (int)(unsigned char)*right;
}
int strncmp(const char *left, const char *right, size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        if (left[i] != right[i]) {
            return (int)(unsigned char)left[i] - (int)(unsigned char)right[i];
        }
        if (left[i] == 0) { break; }
    }
    return 0;
}
__declspec(dllexport) void RunTests(void (*on_failure)(const char *))
{
    failure_handler = on_failure;
    (void)main();
}
