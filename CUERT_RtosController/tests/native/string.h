#pragma once
#include <stddef.h>
void *memcpy(void *dest, const void *source, size_t count);
void *memmove(void *dest, const void *source, size_t count);
int memcmp(const void *left, const void *right, size_t count);
int strcmp(const char *left, const char *right);
int strncmp(const char *left, const char *right, size_t count);
