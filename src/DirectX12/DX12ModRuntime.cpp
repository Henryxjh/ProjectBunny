#include "DX12ModRuntime.h"

#include <Shlwapi.h>
#include <stdint.h>
#include <stdio.h>

#include <algorithm>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "DX12State.h"
#include "DX12DeviceHooks.h"
#include "DX12ShaderDump.h"
#include "IniDocument.h"
#include "MigotoCommandList.h"
#include "MigotoIniLoader.h"
#include "MigotoResource.h"
#include "MigotoShaderOverride.h"
#include "MigotoTextureOverride.h"

static SRWLOCK gModLock = SRWLOCK_INIT;
static bool gLoaded = false;
static std::wstring gConfigPath;
static std::wstring gBaseDir;
static std::wstring gShaderFixesDir;
static Bunny::ShaderOverrideMap gShaderOverrides;
static Bunny::TextureOverrideMap gTextureOverrides;
static Bunny::ResourceMap gResources;
static Bunny::CommandListMap gCommandLists;
static UINT64 gReloadGeneration = 1;
static volatile LONG gHasShaderOverrides = 0;
static volatile LONG gHasTextureOverrides = 0;
static std::unordered_map<uint64_t, bool> gIaSkipCache;

struct DX12LoadedResource
{
	ID3D12Resource *resource = nullptr;
	UINT64 byteWidth = 0;
	UINT stride = 0;
	std::wstring name;
	std::wstring path;
};

static std::unordered_map<std::wstring, DX12LoadedResource> gLoadedResources;

enum class DX12PsoKind
{
	Graphics,
	Compute
};

struct DX12StoredPso
{
	DX12PsoKind kind = DX12PsoKind::Graphics;
	ID3D12Device *device = nullptr;
	ID3D12RootSignature *graphicsRootSignature = nullptr;
	ID3D12RootSignature *computeRootSignature = nullptr;
	D3D12_GRAPHICS_PIPELINE_STATE_DESC graphicsDesc = {};
	D3D12_COMPUTE_PIPELINE_STATE_DESC computeDesc = {};
	std::vector<unsigned char> vsBytecode;
	std::vector<unsigned char> psBytecode;
	std::vector<unsigned char> dsBytecode;
	std::vector<unsigned char> hsBytecode;
	std::vector<unsigned char> gsBytecode;
	std::vector<unsigned char> csBytecode;
	ID3D12PipelineState *replacement = nullptr;
	UINT64 replacementGeneration = 0;
	bool skipDraw = false;
	bool skipDispatch = false;
	UINT64 skipGeneration = 0;
};

static std::unordered_map<ID3D12PipelineState*, DX12StoredPso> gPsoRecords;

static void ReleaseLoadedResourcesLocked();

static void SetBasePaths(const wchar_t *configPath)
{
	gConfigPath = configPath ? configPath : L"";
	gBaseDir = gConfigPath;
	if (!gBaseDir.empty()) {
		wchar_t path[MAX_PATH];
		wcsncpy_s(path, gBaseDir.c_str(), _TRUNCATE);
		PathRemoveFileSpecW(path);
		gBaseDir = path;
	}
	if (gBaseDir.empty())
		gBaseDir = L".";

	wchar_t shaderFixes[MAX_PATH];
	wcsncpy_s(shaderFixes, gBaseDir.c_str(), _TRUNCATE);
	PathAppendW(shaderFixes, L"ShaderFixes");
	gShaderFixesDir = shaderFixes;
}

static bool ReadFileBytes(const wchar_t *path, std::vector<unsigned char> *data)
{
	if (!path || !data)
		return false;

	FILE *file = _wfsopen(path, L"rb", _SH_DENYNO);
	if (!file)
		return false;

	if (fseek(file, 0, SEEK_END) != 0) {
		fclose(file);
		return false;
	}
	long size = ftell(file);
	if (size <= 0) {
		fclose(file);
		return false;
	}
	if (fseek(file, 0, SEEK_SET) != 0) {
		fclose(file);
		return false;
	}

	data->resize(static_cast<size_t>(size));
	size_t read = fread(data->data(), 1, data->size(), file);
	fclose(file);
	if (read != data->size()) {
		data->clear();
		return false;
	}
	return true;
}

static void ReleaseStoredPso(DX12StoredPso *record)
{
	if (!record)
		return;
	if (record->replacement) {
		record->replacement->Release();
		record->replacement = nullptr;
	}
	if (record->device) {
		record->device->Release();
		record->device = nullptr;
	}
	if (record->graphicsRootSignature) {
		record->graphicsRootSignature->Release();
		record->graphicsRootSignature = nullptr;
	}
	if (record->computeRootSignature) {
		record->computeRootSignature->Release();
		record->computeRootSignature = nullptr;
	}
}

static void StoreShaderBytecode(
	const D3D12_SHADER_BYTECODE &source, std::vector<unsigned char> *storage,
	D3D12_SHADER_BYTECODE *target)
{
	if (!storage || !target)
		return;

	*target = {};
	storage->clear();
	if (!source.pShaderBytecode || source.BytecodeLength == 0)
		return;

	storage->resize(source.BytecodeLength);
	memcpy(storage->data(), source.pShaderBytecode, source.BytecodeLength);
	target->pShaderBytecode = storage->data();
	target->BytecodeLength = storage->size();
}

static void DeepCopyGraphicsDesc(
	const D3D12_GRAPHICS_PIPELINE_STATE_DESC *source, DX12StoredPso *record)
{
	record->graphicsDesc = *source;
	record->graphicsRootSignature = source->pRootSignature;
	if (record->graphicsRootSignature)
		record->graphicsRootSignature->AddRef();
	StoreShaderBytecode(source->VS, &record->vsBytecode, &record->graphicsDesc.VS);
	StoreShaderBytecode(source->PS, &record->psBytecode, &record->graphicsDesc.PS);
	StoreShaderBytecode(source->DS, &record->dsBytecode, &record->graphicsDesc.DS);
	StoreShaderBytecode(source->HS, &record->hsBytecode, &record->graphicsDesc.HS);
	StoreShaderBytecode(source->GS, &record->gsBytecode, &record->graphicsDesc.GS);
}

static void DeepCopyComputeDesc(
	const D3D12_COMPUTE_PIPELINE_STATE_DESC *source, DX12StoredPso *record)
{
	record->computeDesc = *source;
	record->computeRootSignature = source->pRootSignature;
	if (record->computeRootSignature)
		record->computeRootSignature->AddRef();
	StoreShaderBytecode(source->CS, &record->csBytecode, &record->computeDesc.CS);
}

uint64_t DX12ModHashShaderBytecode(const void *data, size_t size)
{
	const unsigned char *bytes = static_cast<const unsigned char*>(data);
	uint64_t hash = 14695981039346656037ull;
	for (size_t i = 0; i < size; ++i) {
		hash ^= bytes[i];
		hash *= 1099511628211ull;
	}
	return hash;
}

void DX12ModRuntimeLoad(const wchar_t *configPath)
{
	Bunny::MigotoIniLoadResult iniLoad;
	Bunny::ShaderOverrideMap shaderOverrides;
	Bunny::TextureOverrideMap textureOverrides;
	Bunny::ResourceMap resources;
	Bunny::CommandListMap commandLists;

	SetBasePaths(configPath);
	if (!Bunny::LoadMigotoIniWithIncludes(configPath, &iniLoad)) {
		DX12LogJsonFunc("DX12ModRuntime",
			"\"status\":\"config_load_failed\",\"path\":\"%S\",\"error\":\"%S\"",
			configPath ? configPath : L"", iniLoad.document.Error().c_str());
		return;
	}

	Bunny::ParseShaderOverrideSections(iniLoad.document, &shaderOverrides);
	Bunny::ParseTextureOverrideSections(iniLoad.document, &textureOverrides);
	Bunny::ParseResourceSections(iniLoad.document, &resources);
	Bunny::ParseCommandListSections(iniLoad.document, &commandLists);

	AcquireSRWLockExclusive(&gModLock);
	gShaderOverrides.swap(shaderOverrides);
	gTextureOverrides.swap(textureOverrides);
	gResources.swap(resources);
	gCommandLists.swap(commandLists);
	ReleaseLoadedResourcesLocked();
	gIaSkipCache.clear();
	InterlockedExchange(&gHasShaderOverrides, gShaderOverrides.empty() ? 0 : 1);
	InterlockedExchange(&gHasTextureOverrides, gTextureOverrides.empty() ? 0 : 1);
	gLoaded = true;
	++gReloadGeneration;
	for (auto &item : gPsoRecords) {
		if (item.second.replacement) {
			item.second.replacement->Release();
			item.second.replacement = nullptr;
		}
		item.second.replacementGeneration = 0;
		item.second.skipGeneration = 0;
	}
	ReleaseSRWLockExclusive(&gModLock);

	DX12LogJsonFunc("DX12ModRuntime",
		"\"status\":\"loaded\",\"path\":\"%S\",\"iniFiles\":%zu,\"warnings\":%zu,\"shaderOverrides\":%zu,\"textureOverrides\":%zu,\"resources\":%zu,\"commandLists\":%zu,\"shaderFixes\":\"%S\",\"generation\":%llu",
		configPath ? configPath : L"", iniLoad.loadedFiles.size(), iniLoad.warnings.size(),
		gShaderOverrides.size(), gTextureOverrides.size(), gResources.size(), gCommandLists.size(),
		gShaderFixesDir.c_str(),
		static_cast<unsigned long long>(gReloadGeneration));
	for (const std::wstring &loadedFile : iniLoad.loadedFiles) {
		DX12LogJsonFunc("DX12ModRuntimeIni",
			"\"status\":\"loaded\",\"path\":\"%S\"", loadedFile.c_str());
	}
	for (const std::wstring &warning : iniLoad.warnings) {
		DX12LogJsonFunc("DX12ModRuntimeIni",
			"\"status\":\"warning\",\"message\":\"%S\"", warning.c_str());
	}
}

void DX12ModRuntimeReload()
{
	std::wstring configPath;
	AcquireSRWLockShared(&gModLock);
	configPath = gConfigPath;
	ReleaseSRWLockShared(&gModLock);
	if (configPath.empty()) {
		DX12LogJsonFunc("DX12ModRuntimeReload", "\"status\":\"skipped\",\"reason\":\"empty_config_path\"");
		return;
	}
	DX12LogJsonFunc("DX12ModRuntimeReload", "\"status\":\"begin\",\"path\":\"%S\"", configPath.c_str());
	DX12ModRuntimeLoad(configPath.c_str());
	DX12SetOverlayStatus(L"F10 DX12 mod config reloaded");
}

static std::wstring ResolveResourcePathLocked(const Bunny::ResourceConfig &config)
{
	if (config.filename.empty())
		return L"";

	wchar_t path[MAX_PATH];
	wcsncpy_s(path, config.filename.c_str(), _TRUNCATE);
	if (PathIsRelativeW(path)) {
		wchar_t combined[MAX_PATH];
		wcsncpy_s(combined, gBaseDir.c_str(), _TRUNCATE);
		PathAppendW(combined, config.filename.c_str());
		return combined;
	}
	return path;
}

static bool ResourceConfigLooksLikeBuffer(const Bunny::ResourceConfig &config)
{
	std::wstring type = Bunny::ToLower(Bunny::Trim(config.type));
	if (type.empty())
		return true;
	return type == L"buffer" ||
		type == L"structuredbuffer" ||
		type == L"appendstructuredbuffer" ||
		type == L"consumestructuredbuffer" ||
		type == L"byteaddressbuffer" ||
		type == L"rwbuffer" ||
		type == L"rwstructuredbuffer" ||
		type == L"rwbyteaddressbuffer";
}

static bool LoadResourceBytesLocked(
	const Bunny::ResourceConfig &config, std::vector<unsigned char> *bytes, std::wstring *path)
{
	if (!bytes)
		return false;
	bytes->clear();

	std::wstring resolvedPath = ResolveResourcePathLocked(config);
	if (!resolvedPath.empty()) {
		if (path)
			*path = resolvedPath;
		return ReadFileBytes(resolvedPath.c_str(), bytes);
	}

	if (config.data.empty())
		return false;

	std::wstring data = Bunny::Trim(config.data);
	if (data.rfind(L"0x", 0) == 0 || data.rfind(L"0X", 0) == 0)
		data = data.substr(2);
	data.erase(std::remove_if(data.begin(), data.end(), iswspace), data.end());
	if (data.empty() || (data.size() % 2) != 0)
		return false;

	for (size_t i = 0; i < data.size(); i += 2) {
		wchar_t text[3] = {data[i], data[i + 1], 0};
		wchar_t *end = nullptr;
		unsigned long parsed = wcstoul(text, &end, 16);
		if (!end || *end)
			return false;
		bytes->push_back(static_cast<unsigned char>(parsed));
	}
	return !bytes->empty();
}

static ID3D12Device *AcquireModDevice(ID3D12GraphicsCommandList *commandList)
{
	if (!commandList)
		return nullptr;

	ID3D12Device *device = nullptr;
	if (SUCCEEDED(commandList->GetDevice(IID_PPV_ARGS(&device))) && device)
		return device;
	return nullptr;
}

static DX12LoadedResource *EnsureLoadedResourceLocked(
	ID3D12Device *device, const std::wstring &name)
{
	if (!device || name.empty())
		return nullptr;

	auto loaded = gLoadedResources.find(name);
	if (loaded != gLoadedResources.end())
		return loaded->second.resource ? &loaded->second : nullptr;

	auto configIt = gResources.find(name);
	if (configIt == gResources.end())
		return nullptr;

	const Bunny::ResourceConfig &config = configIt->second;
	if (!ResourceConfigLooksLikeBuffer(config))
		return nullptr;

	std::vector<unsigned char> bytes;
	std::wstring path;
	if (!LoadResourceBytesLocked(config, &bytes, &path)) {
		DX12LogJsonFunc("DX12ResourceLoad",
			"\"status\":\"failed\",\"section\":\"%S\",\"reason\":\"read_failed\",\"path\":\"%S\"",
			config.section.c_str(), path.c_str());
		return nullptr;
	}

	UINT64 byteWidth = config.hasByteWidth ? config.byteWidth : bytes.size();
	if (byteWidth < bytes.size())
		byteWidth = bytes.size();
	if (byteWidth == 0)
		return nullptr;

	D3D12_HEAP_PROPERTIES heap = {};
	heap.Type = D3D12_HEAP_TYPE_UPLOAD;
	heap.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
	heap.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
	heap.CreationNodeMask = 1;
	heap.VisibleNodeMask = 1;

	D3D12_RESOURCE_DESC desc = {};
	desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	desc.Width = byteWidth;
	desc.Height = 1;
	desc.DepthOrArraySize = 1;
	desc.MipLevels = 1;
	desc.Format = DXGI_FORMAT_UNKNOWN;
	desc.SampleDesc.Count = 1;
	desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

	ID3D12Resource *resource = nullptr;
	HRESULT hr = device->CreateCommittedResource(
		&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ,
		nullptr, IID_PPV_ARGS(&resource));
	if (FAILED(hr) || !resource) {
		DX12LogJsonFunc("DX12ResourceLoad",
			"\"status\":\"failed\",\"section\":\"%S\",\"reason\":\"create_failed\",\"hr\":\"0x%lx\",\"bytes\":%llu",
			config.section.c_str(), hr, static_cast<unsigned long long>(byteWidth));
		return nullptr;
	}

	void *mapped = nullptr;
	D3D12_RANGE readRange = {};
	hr = resource->Map(0, &readRange, &mapped);
	if (SUCCEEDED(hr) && mapped) {
		memcpy(mapped, bytes.data(), bytes.size());
		if (byteWidth > bytes.size())
			memset(static_cast<unsigned char*>(mapped) + bytes.size(), 0, byteWidth - bytes.size());
		resource->Unmap(0, nullptr);
	} else {
		resource->Release();
		DX12LogJsonFunc("DX12ResourceLoad",
			"\"status\":\"failed\",\"section\":\"%S\",\"reason\":\"map_failed\",\"hr\":\"0x%lx\"",
			config.section.c_str(), hr);
		return nullptr;
	}

	DX12LoadedResource loadedResource;
	loadedResource.resource = resource;
	loadedResource.byteWidth = byteWidth;
	loadedResource.stride = config.hasStride ? config.stride : 0;
	loadedResource.name = name;
	loadedResource.path = path;
	auto inserted = gLoadedResources.emplace(name, loadedResource);
	DX12LogJsonFunc("DX12ResourceLoad",
		"\"status\":\"loaded\",\"section\":\"%S\",\"name\":\"%S\",\"path\":\"%S\",\"bytes\":%llu,\"stride\":%u",
		config.section.c_str(), name.c_str(), path.c_str(),
		static_cast<unsigned long long>(byteWidth), loadedResource.stride);
	return &inserted.first->second;
}

bool DX12ModReplaceShaderBytecode(
	const char *stage, const D3D12_SHADER_BYTECODE &source,
	D3D12_SHADER_BYTECODE *replacement, std::vector<unsigned char> *storage)
{
	if (!stage || !source.pShaderBytecode || source.BytecodeLength == 0 ||
	    !replacement || !storage)
		return false;

	const uint64_t hash = DX12ModHashShaderBytecode(
		source.pShaderBytecode, source.BytecodeLength);

	std::wstring section;
	std::wstring shaderFixesDir;
	AcquireSRWLockShared(&gModLock);
	auto it = gShaderOverrides.find(hash);
	if (it == gShaderOverrides.end()) {
		ReleaseSRWLockShared(&gModLock);
		return false;
	}
	section = it->second.section;
	shaderFixesDir = gShaderFixesDir;
	ReleaseSRWLockShared(&gModLock);

	wchar_t path[MAX_PATH];
	swprintf_s(path, L"%s\\%016llx-%S.bin",
		shaderFixesDir.c_str(), static_cast<unsigned long long>(hash), stage);

	std::vector<unsigned char> bytes;
	if (!ReadFileBytes(path, &bytes)) {
		DX12LogJsonFunc("DX12ShaderOverrideMissingReplacement",
			"\"section\":\"%S\",\"stage\":\"%s\",\"hash\":\"%016llx\",\"path\":\"%S\"",
			section.c_str(), stage, static_cast<unsigned long long>(hash), path);
		return false;
	}

	storage->swap(bytes);
	replacement->pShaderBytecode = storage->data();
	replacement->BytecodeLength = storage->size();
	DX12LogJsonFunc("DX12ShaderOverrideApplied",
		"\"section\":\"%S\",\"stage\":\"%s\",\"hash\":\"%016llx\",\"path\":\"%S\",\"bytes\":%zu",
		section.c_str(), stage, static_cast<unsigned long long>(hash), path, storage->size());
	return true;
}

bool DX12ModHasShaderOverride(uint64_t hash)
{
	AcquireSRWLockShared(&gModLock);
	bool found = gShaderOverrides.find(hash) != gShaderOverrides.end();
	ReleaseSRWLockShared(&gModLock);
	return found;
}

bool DX12ModHasActiveShaderOverrides()
{
	return gHasShaderOverrides != 0;
}

bool DX12ModHasActiveTextureOverrides()
{
	return gHasTextureOverrides != 0;
}

bool DX12ModHasAnyActiveOverrides()
{
	return gHasShaderOverrides != 0 || gHasTextureOverrides != 0;
}

static bool TextureOverrideMatchesDrawContext(
	const Bunny::TextureOverrideConfig &config,
	uint32_t vertexCount, uint32_t indexCount, uint32_t instanceCount)
{
	if (config.hasMatchVertexCount && config.matchVertexCount != vertexCount)
		return false;
	if (config.hasMatchIndexCount && config.matchIndexCount != indexCount)
		return false;
	if (config.hasMatchInstanceCount && config.matchInstanceCount != instanceCount)
		return false;
	return true;
}

static void ReleaseLoadedResourcesLocked()
{
	for (auto &item : gLoadedResources) {
		if (item.second.resource) {
			item.second.resource->Release();
			item.second.resource = nullptr;
		}
	}
	gLoadedResources.clear();
}

static const Bunny::TextureOverrideConfig *FindTextureOverrideLocked(
	uint32_t hash, uint32_t vertexCount, uint32_t indexCount, uint32_t instanceCount,
	bool requireSkip)
{
	auto it = gTextureOverrides.find(hash);
	if (it == gTextureOverrides.end())
		return nullptr;

	for (const Bunny::TextureOverrideConfig &config : it->second) {
		if (requireSkip && !config.handlingSkip)
			continue;
		if (!TextureOverrideMatchesDrawContext(config, vertexCount, indexCount, instanceCount))
			continue;
		return &config;
	}
	return nullptr;
}

static bool TextureOverrideHasSkipLocked(
	uint32_t hash, uint32_t vertexCount, uint32_t indexCount, uint32_t instanceCount,
	std::wstring *section)
{
	const Bunny::TextureOverrideConfig *config = FindTextureOverrideLocked(
		hash, vertexCount, indexCount, instanceCount, true);
	if (!config)
		return false;
	if (section)
		*section = config->section;
	return true;
}

static const Bunny::TextureOverrideConfig *FindMatchingIaOverrideLocked(
	uint32_t ibHash, const uint32_t *vbHashes, size_t vbHashCount,
	uint32_t vertexCount, uint32_t indexCount, uint32_t instanceCount)
{
	if (ibHash) {
		const Bunny::TextureOverrideConfig *config = FindTextureOverrideLocked(
			ibHash, vertexCount, indexCount, instanceCount, false);
		if (config)
			return config;
	}

	if (!vbHashes)
		return nullptr;
	for (size_t i = 0; i < vbHashCount; ++i) {
		if (!vbHashes[i])
			continue;
		const Bunny::TextureOverrideConfig *config = FindTextureOverrideLocked(
			vbHashes[i], vertexCount, indexCount, instanceCount, false);
		if (config)
			return config;
	}
	return nullptr;
}

static const Bunny::TextureOverrideConfig *FindMatchingIaOverrideLocked(
	const DX12IaHashState &iaState,
	uint32_t vertexCount, uint32_t indexCount, uint32_t instanceCount)
{
	if (iaState.hasIndexBuffer && iaState.indexHash) {
		const Bunny::TextureOverrideConfig *config = FindTextureOverrideLocked(
			iaState.indexHash, vertexCount, indexCount, instanceCount, false);
		if (config)
			return config;
	}

	for (const DX12IaBufferHash &buffer : iaState.vertexBuffers) {
		if (!buffer.hash)
			continue;
		const Bunny::TextureOverrideConfig *config = FindTextureOverrideLocked(
			buffer.hash, vertexCount, indexCount, instanceCount, false);
		if (!config)
			continue;
		auto slotResource = config->vertexBufferResources.find(buffer.slot);
		if (slotResource != config->vertexBufferResources.end())
			return config;
		if (config->indexBufferResource.empty() && !config->vertexBufferResources.empty())
			continue;
		return config;
	}
	return nullptr;
}

static const DX12IaBufferHash *FindIaVertexSlot(const DX12IaHashState &iaState, uint32_t slot)
{
	for (const DX12IaBufferHash &buffer : iaState.vertexBuffers) {
		if (buffer.slot == slot)
			return &buffer;
	}
	return nullptr;
}

static bool AppendResourceViewsLocked(
	ID3D12Device *device, const DX12IaHashState &iaState,
	const std::wstring &indexResource, const std::map<uint32_t, std::wstring> &vertexResources,
	DX12ModIaReplacement *replacement)
{
	if (!device || !replacement)
		return false;

	bool changed = false;
	if (!indexResource.empty()) {
		DX12LoadedResource *resource = EnsureLoadedResourceLocked(device, indexResource);
		if (resource && resource->resource) {
			replacement->indexBuffer.BufferLocation = resource->resource->GetGPUVirtualAddress();
			replacement->indexBuffer.SizeInBytes =
				static_cast<UINT>((std::min)(resource->byteWidth, static_cast<UINT64>(UINT_MAX)));
			replacement->indexBuffer.Format = iaState.hasIndexBuffer &&
				iaState.indexView.Format != DXGI_FORMAT_UNKNOWN ?
				iaState.indexView.Format : DXGI_FORMAT_R32_UINT;
			replacement->hasIndexBuffer = true;
			changed = true;
		}
	}

	if (!vertexResources.empty()) {
		uint32_t minSlot = vertexResources.begin()->first;
		uint32_t maxSlot = vertexResources.rbegin()->first;
		if (maxSlot >= minSlot && maxSlot - minSlot < 32) {
			if (replacement->vertexBuffers.empty() ||
			    minSlot < replacement->vertexBufferStartSlot ||
			    maxSlot >= replacement->vertexBufferStartSlot + replacement->vertexBuffers.size()) {
				uint32_t oldStart = replacement->vertexBuffers.empty() ?
					minSlot : replacement->vertexBufferStartSlot;
				uint32_t oldEnd = replacement->vertexBuffers.empty() ?
					maxSlot : replacement->vertexBufferStartSlot +
						static_cast<uint32_t>(replacement->vertexBuffers.size()) - 1;
				uint32_t newStart = (std::min)(oldStart, minSlot);
				uint32_t newEnd = (std::max)(oldEnd, maxSlot);
				if (newEnd - newStart >= 32)
					return changed;

				std::vector<D3D12_VERTEX_BUFFER_VIEW> resized(newEnd - newStart + 1);
				for (size_t i = 0; i < replacement->vertexBuffers.size(); ++i) {
					uint32_t slot = replacement->vertexBufferStartSlot + static_cast<uint32_t>(i);
					resized[slot - newStart] = replacement->vertexBuffers[i];
				}
				replacement->vertexBufferStartSlot = newStart;
				replacement->vertexBuffers.swap(resized);
			}

			for (const auto &item : vertexResources) {
				DX12LoadedResource *resource = EnsureLoadedResourceLocked(device, item.second);
				if (!resource || !resource->resource)
					continue;
				D3D12_VERTEX_BUFFER_VIEW &view =
					replacement->vertexBuffers[item.first - replacement->vertexBufferStartSlot];
				view.BufferLocation = resource->resource->GetGPUVirtualAddress();
				view.SizeInBytes =
					static_cast<UINT>((std::min)(resource->byteWidth, static_cast<UINT64>(UINT_MAX)));
				const DX12IaBufferHash *sourceSlot = FindIaVertexSlot(iaState, item.first);
				view.StrideInBytes = resource->stride ? resource->stride :
					(sourceSlot ? sourceSlot->vertexView.StrideInBytes : 0);
				changed = true;
			}
		}
	}
	return changed;
}

static const Bunny::TextureOverrideConfig *FindTargetTextureOverrideLocked(
	const DX12IaHashState &iaState, const Bunny::CommandListTarget &target,
	uint32_t vertexCount, uint32_t indexCount, uint32_t instanceCount)
{
	if (target.kind == Bunny::CommandListTargetKind::IndexBuffer) {
		if (!iaState.hasIndexBuffer || !iaState.indexHash)
			return nullptr;
		return FindTextureOverrideLocked(
			iaState.indexHash, vertexCount, indexCount, instanceCount, false);
	}

	if (target.kind == Bunny::CommandListTargetKind::VertexBuffer) {
		const DX12IaBufferHash *slot = FindIaVertexSlot(iaState, target.slot);
		if (!slot || !slot->hash)
			return nullptr;
		return FindTextureOverrideLocked(
			slot->hash, vertexCount, indexCount, instanceCount, false);
	}

	return nullptr;
}

struct DX12CommandListContext
{
	ID3D12Device *device = nullptr;
	const DX12IaHashState *iaState = nullptr;
	uint32_t vertexCount = 0;
	uint32_t indexCount = 0;
	uint32_t instanceCount = 0;
	DX12ModIaReplacement *replacement = nullptr;
	bool changed = false;
};

static void ExecuteCommandListLinksLocked(
	const Bunny::CommandListLinks &links, DX12CommandListContext *context,
	int depth, bool includePost);

static void ExecuteTextureOverrideLocked(
	const Bunny::TextureOverrideConfig &config, DX12CommandListContext *context,
	int depth, bool includePost)
{
	if (!context || !context->replacement || !context->iaState)
		return;

	for (const std::wstring &list : config.commandLists.pre)
	{
		Bunny::CommandListLinks links;
		links.main.push_back(list);
		ExecuteCommandListLinksLocked(links, context, depth + 1, false);
	}

	context->replacement->skip = context->replacement->skip || config.handlingSkip;
	context->changed |= AppendResourceViewsLocked(
		context->device, *context->iaState,
		config.indexBufferResource, config.vertexBufferResources, context->replacement);

	for (const std::wstring &list : config.commandLists.main)
	{
		Bunny::CommandListLinks links;
		links.main.push_back(list);
		ExecuteCommandListLinksLocked(links, context, depth + 1, false);
	}

	if (includePost) {
		for (const std::wstring &list : config.commandLists.post)
		{
			Bunny::CommandListLinks links;
			links.main.push_back(list);
			ExecuteCommandListLinksLocked(links, context, depth + 1, true);
		}
	}
}

static void ExecuteCommandListLocked(
	const Bunny::CommandListConfig &commandList, DX12CommandListContext *context,
	int depth, bool includePost)
{
	if (!context || !context->replacement || !context->iaState || depth > 16)
		return;

	for (const Bunny::CommandListAction &action : commandList.actions) {
		switch (action.kind) {
		case Bunny::CommandListActionKind::Run: {
			auto it = gCommandLists.find(action.commandList);
			if (it != gCommandLists.end())
				ExecuteCommandListLocked(it->second, context, depth + 1, includePost);
			break;
		}
		case Bunny::CommandListActionKind::CheckTextureOverride: {
			const Bunny::TextureOverrideConfig *config = FindTargetTextureOverrideLocked(
				*context->iaState, action.target,
				context->vertexCount, context->indexCount, context->instanceCount);
			if (config)
				ExecuteTextureOverrideLocked(*config, context, depth + 1, includePost);
			break;
		}
		case Bunny::CommandListActionKind::HandlingSkip:
			context->replacement->skip = true;
			break;
		case Bunny::CommandListActionKind::SetIndexBuffer:
			context->changed |= AppendResourceViewsLocked(
				context->device, *context->iaState, action.resource,
				std::map<uint32_t, std::wstring>(), context->replacement);
			break;
		case Bunny::CommandListActionKind::SetVertexBuffer:
			context->changed |= AppendResourceViewsLocked(
				context->device, *context->iaState, L"",
				std::map<uint32_t, std::wstring>{{ action.target.slot, action.resource }},
				context->replacement);
			break;
		}
	}
}

static void ExecuteCommandListLinksLocked(
	const Bunny::CommandListLinks &links, DX12CommandListContext *context,
	int depth, bool includePost)
{
	if (!context || depth > 16)
		return;

	for (const std::wstring &list : links.pre) {
		auto it = gCommandLists.find(list);
		if (it != gCommandLists.end())
			ExecuteCommandListLocked(it->second, context, depth + 1, false);
	}
	for (const std::wstring &list : links.main) {
		auto it = gCommandLists.find(list);
		if (it != gCommandLists.end())
			ExecuteCommandListLocked(it->second, context, depth + 1, includePost);
	}
	if (includePost) {
		for (const std::wstring &list : links.post) {
			auto it = gCommandLists.find(list);
			if (it != gCommandLists.end())
				ExecuteCommandListLocked(it->second, context, depth + 1, true);
		}
	}
}

static uint64_t MakeIaSkipCacheKey(uint32_t ibHash, const uint32_t *vbHashes, size_t vbHashCount)
{
	uint64_t hash = 14695981039346656037ull;
	auto append = [&hash](const void *data, size_t size) {
		const unsigned char *bytes = static_cast<const unsigned char*>(data);
		for (size_t i = 0; i < size; ++i) {
			hash ^= bytes[i];
			hash *= 1099511628211ull;
		}
	};
	append(&ibHash, sizeof(ibHash));
	for (size_t i = 0; vbHashes && i < vbHashCount; ++i) {
		if (!vbHashes[i])
			continue;
		append(&vbHashes[i], sizeof(vbHashes[i]));
	}
	return hash;
}

bool DX12ModShouldSkipIa(
	uint32_t ibHash, const uint32_t *vbHashes, size_t vbHashCount,
	uint32_t vertexCount, uint32_t indexCount, uint32_t instanceCount)
{
	if (gHasTextureOverrides == 0)
		return false;

	uint64_t cacheKey = MakeIaSkipCacheKey(ibHash, vbHashes, vbHashCount);
	cacheKey ^= static_cast<uint64_t>(vertexCount) << 1;
	cacheKey ^= static_cast<uint64_t>(indexCount) << 17;
	cacheKey ^= static_cast<uint64_t>(instanceCount) << 33;
	AcquireSRWLockShared(&gModLock);
	auto cached = gIaSkipCache.find(cacheKey);
	if (cached != gIaSkipCache.end()) {
		bool skip = cached->second;
		ReleaseSRWLockShared(&gModLock);
		return skip;
	}
	ReleaseSRWLockShared(&gModLock);

	bool skip = false;
	uint32_t matchedHash = 0;
	std::wstring section;
	AcquireSRWLockShared(&gModLock);
	if (ibHash && TextureOverrideHasSkipLocked(
		ibHash, vertexCount, indexCount, instanceCount, &section)) {
		skip = true;
		matchedHash = ibHash;
	}
	if (!skip && vbHashes) {
		for (size_t i = 0; i < vbHashCount; ++i) {
			if (!vbHashes[i])
				continue;
			if (TextureOverrideHasSkipLocked(
				vbHashes[i], vertexCount, indexCount, instanceCount, &section)) {
				skip = true;
				matchedHash = vbHashes[i];
				break;
			}
		}
	}
	ReleaseSRWLockShared(&gModLock);

	AcquireSRWLockExclusive(&gModLock);
	if (gIaSkipCache.size() > 4096)
		gIaSkipCache.clear();
	gIaSkipCache[cacheKey] = skip;
	ReleaseSRWLockExclusive(&gModLock);
	return skip;
}

bool DX12ModPrepareIaReplacement(
	ID3D12GraphicsCommandList *commandList, const DX12IaHashState &iaState,
	uint32_t vertexCount, uint32_t indexCount, uint32_t instanceCount,
	DX12ModIaReplacement *replacement)
{
	if (!replacement)
		return false;
	*replacement = DX12ModIaReplacement();
	if (!commandList || gHasTextureOverrides == 0)
		return false;

	ID3D12Device *device = AcquireModDevice(commandList);
	if (!device)
		return false;

	Bunny::TextureOverrideConfig configCopy;
	bool hasConfig = false;
	AcquireSRWLockShared(&gModLock);
	const Bunny::TextureOverrideConfig *config = FindMatchingIaOverrideLocked(
		iaState, vertexCount, indexCount, instanceCount);
	if (config) {
		configCopy = *config;
		hasConfig = true;
	}
	ReleaseSRWLockShared(&gModLock);

	if (!hasConfig) {
		device->Release();
		return false;
	}

	AcquireSRWLockExclusive(&gModLock);
	DX12CommandListContext context;
	context.device = device;
	context.iaState = &iaState;
	context.vertexCount = vertexCount;
	context.indexCount = indexCount;
	context.instanceCount = instanceCount;
	context.replacement = replacement;
	ExecuteTextureOverrideLocked(configCopy, &context, 0, false);
	ReleaseSRWLockExclusive(&gModLock);
	device->Release();

	return context.changed || replacement->skip;
}

void DX12ModRunPostIaReplacement(
	ID3D12GraphicsCommandList *commandList, const DX12IaHashState &iaState,
	uint32_t vertexCount, uint32_t indexCount, uint32_t instanceCount,
	DX12ModIaReplacement *replacement)
{
	if (!replacement || !commandList || gHasTextureOverrides == 0)
		return;

	ID3D12Device *device = AcquireModDevice(commandList);
	if (!device)
		return;

	Bunny::TextureOverrideConfig configCopy;
	bool hasConfig = false;
	AcquireSRWLockShared(&gModLock);
	const Bunny::TextureOverrideConfig *config = FindMatchingIaOverrideLocked(
		iaState, vertexCount, indexCount, instanceCount);
	if (config) {
		configCopy = *config;
		hasConfig = true;
	}
	ReleaseSRWLockShared(&gModLock);

	if (hasConfig) {
		AcquireSRWLockExclusive(&gModLock);
		DX12CommandListContext context;
		context.device = device;
		context.iaState = &iaState;
		context.vertexCount = vertexCount;
		context.indexCount = indexCount;
		context.instanceCount = instanceCount;
		context.replacement = replacement;
		for (const std::wstring &list : configCopy.commandLists.post) {
			Bunny::CommandListLinks links;
			links.main.push_back(list);
			ExecuteCommandListLinksLocked(links, &context, 0, true);
		}
		ReleaseSRWLockExclusive(&gModLock);
	}
	device->Release();
}

static bool ShaderOverrideHasSkipLocked(uint64_t hash)
{
	auto it = gShaderOverrides.find(hash);
	return it != gShaderOverrides.end() && it->second.handlingSkip;
}

static bool ShaderBytecodeHasSkipLocked(const D3D12_SHADER_BYTECODE &bytecode)
{
	if (!bytecode.pShaderBytecode || !bytecode.BytecodeLength)
		return false;
	return ShaderOverrideHasSkipLocked(
		DX12ModHashShaderBytecode(bytecode.pShaderBytecode, bytecode.BytecodeLength));
}

static bool StoredPsoHasSkipLocked(const DX12StoredPso &record, bool dispatch)
{
	if (dispatch) {
		return record.kind == DX12PsoKind::Compute &&
			ShaderBytecodeHasSkipLocked(record.computeDesc.CS);
	}

	return record.kind == DX12PsoKind::Graphics &&
		(ShaderBytecodeHasSkipLocked(record.graphicsDesc.VS) ||
		 ShaderBytecodeHasSkipLocked(record.graphicsDesc.PS));
}

static void UpdateStoredPsoSkipLocked(DX12StoredPso *record)
{
	if (!record || record->skipGeneration == gReloadGeneration)
		return;

	record->skipDraw = false;
	record->skipDispatch = false;
	if (record->kind == DX12PsoKind::Compute) {
		record->skipDispatch = ShaderBytecodeHasSkipLocked(record->computeDesc.CS);
	} else {
		record->skipDraw =
			ShaderBytecodeHasSkipLocked(record->graphicsDesc.VS) ||
			ShaderBytecodeHasSkipLocked(record->graphicsDesc.PS);
	}
	record->skipGeneration = gReloadGeneration;
}

bool DX12ModShouldSkipPipelineState(ID3D12PipelineState *pipelineState, bool dispatch)
{
	if (!pipelineState || gHasShaderOverrides == 0)
		return false;

	bool skip = false;
	AcquireSRWLockShared(&gModLock);
	auto record = gPsoRecords.find(pipelineState);
	if (record != gPsoRecords.end()) {
		if (record->second.skipGeneration == gReloadGeneration) {
			skip = dispatch ? record->second.skipDispatch : record->second.skipDraw;
			ReleaseSRWLockShared(&gModLock);
			return skip;
		}
	}
	ReleaseSRWLockShared(&gModLock);

	AcquireSRWLockExclusive(&gModLock);
	record = gPsoRecords.find(pipelineState);
	if (record != gPsoRecords.end()) {
		UpdateStoredPsoSkipLocked(&record->second);
		skip = dispatch ? record->second.skipDispatch : record->second.skipDraw;
		ReleaseSRWLockExclusive(&gModLock);
		return skip;
	}
	ReleaseSRWLockExclusive(&gModLock);

	DX12PsoShaderInfo info = {};
	if (!DX12GetPipelineStateShaderInfo(pipelineState, &info))
		return false;

	AcquireSRWLockShared(&gModLock);
	if (dispatch) {
		skip = info.hasCS && ShaderOverrideHasSkipLocked(info.cs);
	} else {
		skip = (info.hasVS && ShaderOverrideHasSkipLocked(info.vs)) ||
			(info.hasPS && ShaderOverrideHasSkipLocked(info.ps));
	}
	ReleaseSRWLockShared(&gModLock);
	return skip;
}

UINT64 DX12ModGetReloadGeneration()
{
	AcquireSRWLockShared(&gModLock);
	UINT64 generation = gReloadGeneration;
	ReleaseSRWLockShared(&gModLock);
	return generation;
}

void DX12ModRecordGraphicsPipelineState(
	ID3D12Device *device, ID3D12PipelineState *pipelineState,
	const D3D12_GRAPHICS_PIPELINE_STATE_DESC *desc)
{
	if (!device || !pipelineState || !desc)
		return;

	DX12StoredPso record;
	record.kind = DX12PsoKind::Graphics;
	record.device = device;
	record.device->AddRef();
	DeepCopyGraphicsDesc(desc, &record);

	AcquireSRWLockExclusive(&gModLock);
	auto existing = gPsoRecords.find(pipelineState);
	if (existing != gPsoRecords.end())
		ReleaseStoredPso(&existing->second);
	gPsoRecords[pipelineState] = record;
	ReleaseSRWLockExclusive(&gModLock);
}

void DX12ModRecordComputePipelineState(
	ID3D12Device *device, ID3D12PipelineState *pipelineState,
	const D3D12_COMPUTE_PIPELINE_STATE_DESC *desc)
{
	if (!device || !pipelineState || !desc)
		return;

	DX12StoredPso record;
	record.kind = DX12PsoKind::Compute;
	record.device = device;
	record.device->AddRef();
	DeepCopyComputeDesc(desc, &record);

	AcquireSRWLockExclusive(&gModLock);
	auto existing = gPsoRecords.find(pipelineState);
	if (existing != gPsoRecords.end())
		ReleaseStoredPso(&existing->second);
	gPsoRecords[pipelineState] = record;
	ReleaseSRWLockExclusive(&gModLock);
}

static bool HasShaderOverrideLocked(uint64_t hash)
{
	return gShaderOverrides.find(hash) != gShaderOverrides.end();
}

static bool GraphicsPsoNeedsReplacementLocked(const D3D12_GRAPHICS_PIPELINE_STATE_DESC &desc)
{
	if (desc.VS.pShaderBytecode && desc.VS.BytecodeLength &&
	    HasShaderOverrideLocked(DX12ModHashShaderBytecode(desc.VS.pShaderBytecode, desc.VS.BytecodeLength)))
		return true;
	if (desc.PS.pShaderBytecode && desc.PS.BytecodeLength &&
	    HasShaderOverrideLocked(DX12ModHashShaderBytecode(desc.PS.pShaderBytecode, desc.PS.BytecodeLength)))
		return true;
	return false;
}

static bool ComputePsoNeedsReplacementLocked(const D3D12_COMPUTE_PIPELINE_STATE_DESC &desc)
{
	return desc.CS.pShaderBytecode && desc.CS.BytecodeLength &&
		HasShaderOverrideLocked(DX12ModHashShaderBytecode(desc.CS.pShaderBytecode, desc.CS.BytecodeLength));
}

static ID3D12PipelineState *CreateGraphicsReplacement(DX12StoredPso *record)
{
	D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = record->graphicsDesc;
	D3D12_SHADER_BYTECODE vs = {};
	D3D12_SHADER_BYTECODE ps = {};
	std::vector<unsigned char> vsBytes;
	std::vector<unsigned char> psBytes;
	bool changed = false;

	if (DX12ModReplaceShaderBytecode("vs", desc.VS, &vs, &vsBytes)) {
		desc.VS = vs;
		changed = true;
	}
	if (DX12ModReplaceShaderBytecode("ps", desc.PS, &ps, &psBytes)) {
		desc.PS = ps;
		changed = true;
	}
	if (!changed)
		return nullptr;

	ID3D12PipelineState *replacement = nullptr;
	HRESULT hr = DX12CreateGraphicsPipelineStateOriginal(
		record->device, &desc, IID_PPV_ARGS(&replacement));
	DX12LogJsonFunc("DX12ReplacementPsoCreate",
		"\"kind\":\"graphics\",\"hr\":\"0x%lx\",\"pso\":\"%p\"",
		hr, replacement);
	return SUCCEEDED(hr) ? replacement : nullptr;
}

static ID3D12PipelineState *CreateComputeReplacement(DX12StoredPso *record)
{
	D3D12_COMPUTE_PIPELINE_STATE_DESC desc = record->computeDesc;
	D3D12_SHADER_BYTECODE cs = {};
	std::vector<unsigned char> csBytes;
	if (!DX12ModReplaceShaderBytecode("cs", desc.CS, &cs, &csBytes))
		return nullptr;
	desc.CS = cs;

	ID3D12PipelineState *replacement = nullptr;
	HRESULT hr = DX12CreateComputePipelineStateOriginal(
		record->device, &desc, IID_PPV_ARGS(&replacement));
	DX12LogJsonFunc("DX12ReplacementPsoCreate",
		"\"kind\":\"compute\",\"hr\":\"0x%lx\",\"pso\":\"%p\"",
		hr, replacement);
	return SUCCEEDED(hr) ? replacement : nullptr;
}

ID3D12PipelineState *DX12ModGetReplacementPipelineState(ID3D12PipelineState *pipelineState)
{
	if (!pipelineState || gHasShaderOverrides == 0)
		return nullptr;

	DX12StoredPso createRecord;
	bool shouldCreate = false;
	UINT64 generation = 0;

	AcquireSRWLockExclusive(&gModLock);
	auto it = gPsoRecords.find(pipelineState);
	if (it == gPsoRecords.end()) {
		ReleaseSRWLockExclusive(&gModLock);
		return nullptr;
	}

	DX12StoredPso &record = it->second;
	generation = gReloadGeneration;
	if (record.replacement && record.replacementGeneration == generation) {
		ID3D12PipelineState *replacement = record.replacement;
		ReleaseSRWLockExclusive(&gModLock);
		return replacement;
	}

	if (record.replacement) {
		record.replacement->Release();
		record.replacement = nullptr;
		record.replacementGeneration = 0;
	}

	bool needsReplacement = record.kind == DX12PsoKind::Graphics ?
		GraphicsPsoNeedsReplacementLocked(record.graphicsDesc) :
		ComputePsoNeedsReplacementLocked(record.computeDesc);
	if (!needsReplacement) {
		ReleaseSRWLockExclusive(&gModLock);
		return nullptr;
	}

	createRecord.kind = record.kind;
	createRecord.device = record.device;
	createRecord.graphicsRootSignature = record.graphicsRootSignature;
	createRecord.computeRootSignature = record.computeRootSignature;
	createRecord.graphicsDesc = record.graphicsDesc;
	createRecord.computeDesc = record.computeDesc;
	createRecord.vsBytecode = record.vsBytecode;
	createRecord.psBytecode = record.psBytecode;
	createRecord.dsBytecode = record.dsBytecode;
	createRecord.hsBytecode = record.hsBytecode;
	createRecord.gsBytecode = record.gsBytecode;
	createRecord.csBytecode = record.csBytecode;
	if (!createRecord.vsBytecode.empty())
		createRecord.graphicsDesc.VS.pShaderBytecode = createRecord.vsBytecode.data();
	if (!createRecord.psBytecode.empty())
		createRecord.graphicsDesc.PS.pShaderBytecode = createRecord.psBytecode.data();
	if (!createRecord.dsBytecode.empty())
		createRecord.graphicsDesc.DS.pShaderBytecode = createRecord.dsBytecode.data();
	if (!createRecord.hsBytecode.empty())
		createRecord.graphicsDesc.HS.pShaderBytecode = createRecord.hsBytecode.data();
	if (!createRecord.gsBytecode.empty())
		createRecord.graphicsDesc.GS.pShaderBytecode = createRecord.gsBytecode.data();
	if (!createRecord.csBytecode.empty())
		createRecord.computeDesc.CS.pShaderBytecode = createRecord.csBytecode.data();
	if (createRecord.device)
		createRecord.device->AddRef();
	if (createRecord.graphicsRootSignature)
		createRecord.graphicsRootSignature->AddRef();
	if (createRecord.computeRootSignature)
		createRecord.computeRootSignature->AddRef();
	shouldCreate = true;
	ReleaseSRWLockExclusive(&gModLock);

	if (!shouldCreate)
		return nullptr;

	ID3D12PipelineState *newReplacement = createRecord.kind == DX12PsoKind::Graphics ?
		CreateGraphicsReplacement(&createRecord) :
		CreateComputeReplacement(&createRecord);
	if (createRecord.device)
		createRecord.device->Release();
	if (createRecord.graphicsRootSignature)
		createRecord.graphicsRootSignature->Release();
	if (createRecord.computeRootSignature)
		createRecord.computeRootSignature->Release();
	if (!newReplacement)
		return nullptr;

	AcquireSRWLockExclusive(&gModLock);
	it = gPsoRecords.find(pipelineState);
	if (it == gPsoRecords.end() || gReloadGeneration != generation) {
		ReleaseSRWLockExclusive(&gModLock);
		newReplacement->Release();
		return nullptr;
	}
	if (it->second.replacement)
		it->second.replacement->Release();
	it->second.replacement = newReplacement;
	it->second.replacementGeneration = generation;
	ReleaseSRWLockExclusive(&gModLock);
	return newReplacement;
}
