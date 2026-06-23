#pragma once

#include <stdint.h>

#include <string>
#include <unordered_map>
#include <vector>

#include "IniDocument.h"

namespace Bunny {

enum class CommandListTargetKind
{
	Unknown,
	IndexBuffer,
	VertexBuffer
};

struct CommandListTarget
{
	CommandListTargetKind kind = CommandListTargetKind::Unknown;
	uint32_t slot = 0;
};

enum class CommandListActionKind
{
	Run,
	CheckTextureOverride,
	HandlingSkip,
	SetIndexBuffer,
	SetVertexBuffer
};

struct CommandListAction
{
	CommandListActionKind kind = CommandListActionKind::Run;
	std::wstring commandList;
	CommandListTarget target;
	std::wstring resource;
};

struct CommandListConfig
{
	std::wstring section;
	std::vector<CommandListAction> actions;
};

struct CommandListLinks
{
	std::vector<std::wstring> pre;
	std::vector<std::wstring> main;
	std::vector<std::wstring> post;
};

using CommandListMap = std::unordered_map<std::wstring, CommandListConfig>;

bool ParseCommandListTarget(const std::wstring &text, CommandListTarget *target);
void ParseCommandListLinksFromEntry(
	const std::wstring &key, const std::wstring &value, CommandListLinks *links);
void ParseCommandListSections(const IniDocument &ini, CommandListMap *commandLists);

} // namespace Bunny
