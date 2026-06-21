#pragma once

#include <stddef.h>
#include <wchar.h>

void DX12JsonEscapeString(char *dst, size_t dstSize, const char *src);
void DX12JsonEscapeWString(char *dst, size_t dstSize, const wchar_t *src);
