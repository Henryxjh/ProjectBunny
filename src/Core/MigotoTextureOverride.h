#pragma once

#include <stdint.h>

#include <map>
#include <vector>
#include <string>
#include <unordered_map>

#include "IniDocument.h"
#include "MigotoCommandList.h"

namespace Bunny {

struct TextureOverrideConfig
{
	std::wstring section;
	uint32_t hash = 0;
	bool handlingSkip = false;
	std::wstring indexBufferResource;
	std::map<uint32_t, std::wstring> vertexBufferResources;
	bool hasMatchVertexCount = false;
	bool hasMatchIndexCount = false;
	bool hasMatchInstanceCount = false;
	uint32_t matchVertexCount = 0;
	uint32_t matchIndexCount = 0;
	uint32_t matchInstanceCount = 0;
	CommandListLinks commandLists;
};

using TextureOverrideList = std::vector<TextureOverrideConfig>;
using TextureOverrideMap = std::unordered_map<uint32_t, TextureOverrideList>;

bool ParseTextureOverrideHash(const std::wstring &text, uint32_t *value);
void ParseTextureOverrideSections(
	const IniDocument &ini, TextureOverrideMap *textureOverrides);

} // namespace Bunny
