#include "DX12Json.h"

#include <stdio.h>

void DX12JsonEscapeString(char *dst, size_t dstSize, const char *src)
{
	if (!dst || dstSize == 0)
		return;

	size_t pos = 0;
	if (pos + 1 < dstSize)
		dst[pos++] = '"';
	if (src) {
		for (; *src && pos + 6 < dstSize; ++src) {
			switch (*src) {
			case '"': dst[pos++] = '\\'; dst[pos++] = '"'; break;
			case '\\': dst[pos++] = '\\'; dst[pos++] = '\\'; break;
			case '\b': dst[pos++] = '\\'; dst[pos++] = 'b'; break;
			case '\f': dst[pos++] = '\\'; dst[pos++] = 'f'; break;
			case '\n': dst[pos++] = '\\'; dst[pos++] = 'n'; break;
			case '\r': dst[pos++] = '\\'; dst[pos++] = 'r'; break;
			case '\t': dst[pos++] = '\\'; dst[pos++] = 't'; break;
			default:
				if (static_cast<unsigned char>(*src) < 0x20) {
					sprintf_s(dst + pos, dstSize - pos, "\\u%04x",
						static_cast<unsigned char>(*src));
					pos += 6;
				} else {
					dst[pos++] = *src;
				}
			}
		}
	}
	if (pos + 1 < dstSize)
		dst[pos++] = '"';
	if (pos < dstSize)
		dst[pos] = '\0';
}

void DX12JsonEscapeWString(char *dst, size_t dstSize, const wchar_t *src)
{
	if (!dst || dstSize == 0)
		return;

	size_t pos = 0;
	if (pos + 1 < dstSize)
		dst[pos++] = '"';
	if (src) {
		for (; *src && pos + 6 < dstSize; ++src) {
			wchar_t c = *src;
			if (c < 0x20) {
				sprintf_s(dst + pos, dstSize - pos, "\\u%04x",
					static_cast<unsigned int>(c));
				pos += 6;
			} else if (c == L'"') {
				dst[pos++] = '\\';
				dst[pos++] = '"';
			} else if (c == L'\\') {
				dst[pos++] = '\\';
				dst[pos++] = '\\';
			} else if (c < 0x80) {
				dst[pos++] = static_cast<char>(c);
			} else if (c < 0x800 && pos + 2 < dstSize) {
				dst[pos++] = static_cast<char>(0xC0 | (c >> 6));
				dst[pos++] = static_cast<char>(0x80 | (c & 0x3F));
			} else if (pos + 3 < dstSize) {
				dst[pos++] = static_cast<char>(0xE0 | (c >> 12));
				dst[pos++] = static_cast<char>(0x80 | ((c >> 6) & 0x3F));
				dst[pos++] = static_cast<char>(0x80 | (c & 0x3F));
			}
		}
	}
	if (pos + 1 < dstSize)
		dst[pos++] = '"';
	if (pos < dstSize)
		dst[pos] = '\0';
}
