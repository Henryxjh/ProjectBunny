#include "MigotoTextureOverride.h"

namespace Bunny {

static bool StartsWithI(const std::wstring &value, const wchar_t *prefix)
{
	std::wstring lower = ToLower(value);
	std::wstring lowerPrefix = ToLower(prefix);
	return lower.rfind(lowerPrefix, 0) == 0;
}

bool ParseTextureOverrideHash(const std::wstring &text, uint32_t *value)
{
	if (!value)
		return false;

	std::wstring trimmed = Trim(text);
	if (trimmed.rfind(L"0x", 0) == 0 || trimmed.rfind(L"0X", 0) == 0)
		trimmed = trimmed.substr(2);
	if (trimmed.empty() || trimmed.size() > 8)
		return false;

	uint32_t parsed = 0;
	for (wchar_t ch : trimmed) {
		unsigned digit = 0;
		if (ch >= L'0' && ch <= L'9')
			digit = ch - L'0';
		else if (ch >= L'a' && ch <= L'f')
			digit = ch - L'a' + 10;
		else if (ch >= L'A' && ch <= L'F')
			digit = ch - L'A' + 10;
		else
			return false;
		parsed = (parsed << 4) | digit;
	}

	*value = parsed;
	return true;
}

static bool ParseUInt(const std::wstring &text, uint32_t *value)
{
	if (!value)
		return false;

	std::wstring trimmed = Trim(text);
	if (trimmed.empty())
		return false;

	wchar_t *end = nullptr;
	unsigned long parsed = std::wcstoul(trimmed.c_str(), &end, 0);
	if (!end || *Trim(end).c_str())
		return false;

	*value = static_cast<uint32_t>(parsed);
	return true;
}

static bool ParseVertexBufferKey(const std::wstring &key, uint32_t *slot)
{
	if (!slot || key.size() < 3 || key[0] != L'v' || key[1] != L'b')
		return false;

	uint32_t parsed = 0;
	for (size_t i = 2; i < key.size(); ++i) {
		if (key[i] < L'0' || key[i] > L'9')
			return false;
		parsed = parsed * 10 + static_cast<uint32_t>(key[i] - L'0');
	}

	*slot = parsed;
	return true;
}

void ParseTextureOverrideSections(
	const IniDocument &ini, TextureOverrideMap *textureOverrides)
{
	if (!textureOverrides)
		return;

	textureOverrides->clear();
	for (const IniSection &section : ini.Sections()) {
		if (!StartsWithI(section.name, L"TextureOverride"))
			continue;

		TextureOverrideConfig config;
		config.section = section.name;
		bool hasHash = false;

		for (const IniEntry &entry : section.entries) {
			std::wstring key = ToLower(entry.key);
			if (!entry.hasEquals)
				continue;

			if (key == L"hash") {
				uint32_t hash = 0;
				if (!ParseTextureOverrideHash(entry.value, &hash))
					continue;
				config.hash = hash;
				hasHash = true;
				continue;
			}

			if (key == L"handling") {
				std::wstring value = ToLower(Trim(entry.value));
				if (value == L"skip")
					config.handlingSkip = true;
				continue;
			}

			if (key == L"ib") {
				config.indexBufferResource = Trim(entry.value);
				continue;
			}

			uint32_t vbSlot = 0;
			if (ParseVertexBufferKey(key, &vbSlot)) {
				config.vertexBufferResources[vbSlot] = Trim(entry.value);
				continue;
			}

			if (key == L"match_vertex_count") {
				uint32_t value = 0;
				if (ParseUInt(entry.value, &value)) {
					config.matchVertexCount = value;
					config.hasMatchVertexCount = true;
				}
				continue;
			}

			if (key == L"match_index_count") {
				uint32_t value = 0;
				if (ParseUInt(entry.value, &value)) {
					config.matchIndexCount = value;
					config.hasMatchIndexCount = true;
				}
				continue;
			}

			if (key == L"match_instance_count") {
				uint32_t value = 0;
				if (ParseUInt(entry.value, &value)) {
					config.matchInstanceCount = value;
					config.hasMatchInstanceCount = true;
				}
				continue;
			}

			ParseCommandListLinksFromEntry(key, entry.value, &config.commandLists);
		}

		if (hasHash)
			(*textureOverrides)[config.hash].push_back(config);
	}
}

} // namespace Bunny
