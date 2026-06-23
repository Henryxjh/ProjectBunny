#include "MigotoCommandList.h"

namespace Bunny {

static bool StartsWithI(const std::wstring &value, const wchar_t *prefix)
{
	std::wstring lower = ToLower(value);
	std::wstring lowerPrefix = ToLower(prefix);
	return lower.rfind(lowerPrefix, 0) == 0;
}

static bool ParseVertexBufferTarget(const std::wstring &text, uint32_t *slot)
{
	if (!slot || text.size() < 3 || text[0] != L'v' || text[1] != L'b')
		return false;

	uint32_t parsed = 0;
	for (size_t i = 2; i < text.size(); ++i) {
		if (text[i] < L'0' || text[i] > L'9')
			return false;
		parsed = parsed * 10 + static_cast<uint32_t>(text[i] - L'0');
	}

	*slot = parsed;
	return true;
}

bool ParseCommandListTarget(const std::wstring &text, CommandListTarget *target)
{
	if (!target)
		return false;
	*target = CommandListTarget();

	std::wstring value = ToLower(Trim(text));
	if (value == L"ib") {
		target->kind = CommandListTargetKind::IndexBuffer;
		return true;
	}

	uint32_t slot = 0;
	if (ParseVertexBufferTarget(value, &slot)) {
		target->kind = CommandListTargetKind::VertexBuffer;
		target->slot = slot;
		return true;
	}
	return false;
}

static void AppendRunList(const std::wstring &value, std::vector<std::wstring> *lists)
{
	if (!lists)
		return;
	std::wstring list = Trim(value);
	if (!list.empty())
		lists->push_back(list);
}

void ParseCommandListLinksFromEntry(
	const std::wstring &key, const std::wstring &value, CommandListLinks *links)
{
	if (!links)
		return;

	std::wstring lower = ToLower(Trim(key));
	if (lower == L"run") {
		AppendRunList(value, &links->main);
		return;
	}
	if (lower == L"pre" || lower == L"pre run" || lower == L"prerun") {
		AppendRunList(value, &links->pre);
		return;
	}
	if (lower == L"post" || lower == L"post run" || lower == L"postrun") {
		AppendRunList(value, &links->post);
		return;
	}
}

void ParseCommandListSections(const IniDocument &ini, CommandListMap *commandLists)
{
	if (!commandLists)
		return;

	commandLists->clear();
	for (const IniSection &section : ini.Sections()) {
		if (!StartsWithI(section.name, L"CommandList"))
			continue;

		CommandListConfig config;
		config.section = section.name;

		for (const IniEntry &entry : section.entries) {
			if (!entry.hasEquals)
				continue;

			std::wstring key = ToLower(Trim(entry.key));
			std::wstring value = Trim(entry.value);
			if (key == L"run") {
				CommandListAction action;
				action.kind = CommandListActionKind::Run;
				action.commandList = value;
				if (!action.commandList.empty())
					config.actions.push_back(action);
				continue;
			}

			if (key == L"checktextureoverride") {
				CommandListAction action;
				action.kind = CommandListActionKind::CheckTextureOverride;
				if (ParseCommandListTarget(value, &action.target))
					config.actions.push_back(action);
				continue;
			}

			if (key == L"handling" && ToLower(value) == L"skip") {
				CommandListAction action;
				action.kind = CommandListActionKind::HandlingSkip;
				config.actions.push_back(action);
				continue;
			}

			CommandListTarget target;
			if (ParseCommandListTarget(key, &target)) {
				CommandListAction action;
				action.kind = target.kind == CommandListTargetKind::IndexBuffer ?
					CommandListActionKind::SetIndexBuffer :
					CommandListActionKind::SetVertexBuffer;
				action.target = target;
				action.resource = value;
				if (!action.resource.empty())
					config.actions.push_back(action);
				continue;
			}
		}

		(*commandLists)[config.section] = config;
	}
}

} // namespace Bunny
