#pragma once
void TestFail(const char *message);
#define assert(condition) ((condition) ? (void)0 : TestFail(#condition))
