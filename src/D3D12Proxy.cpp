#include <windows.h>
#include <d3d12.h>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <string>

static std::mutex g_logMutex;
static HMODULE g_originalD3D12 = nullptr;

using D3D12CreateDevice_t = HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
using CheckFeatureSupport_t = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, D3D12_FEATURE, void*, UINT);

// BLINDAGEM: Usamos void* para pVideoMemoryInfo e para o dispositivo para o compilador não reclamar de tipos ausentes no SDK
using QueryVideoMemoryInfo_t = HRESULT(STDMETHODCALLTYPE*)(void*, UINT, int, void*);

static D3D12CreateDevice_t g_originalD3D12CreateDevice = nullptr;
static CheckFeatureSupport_t g_originalCheckFeatureSupport = nullptr;
static QueryVideoMemoryInfo_t g_originalQueryVideoMemoryInfo = nullptr;

static LONG g_featureLogBudget = 100;
static constexpr size_t kCheckFeatureSupportVtableIndex = 13;
static constexpr size_t kQueryVideoMemoryInfoVtableIndex = 56; 

static void Log(const char* msg) {
    std::lock_guard<std::mutex> lock(g_logMutex);
    std::ofstream log("ForzaFix_RX580.log", std::ios::app);
    if (!log) return;

    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    struct tm tmBuf {};
    localtime_s(&tmBuf, &time);
    log << std::put_time(&tmBuf, "%H:%M:%S") << " - " << msg << std::endl;
}

static bool LoadOriginalD3D12() {
    if (g_originalD3D12CreateDevice) return true;
    char systemPath[MAX_PATH];
    GetSystemDirectoryA(systemPath, MAX_PATH);
    std::string dllPath = std::string(systemPath) + "\\d3d12.dll";
    g_originalD3D12 = LoadLibraryA(dllPath.c_str());
    if (!g_originalD3D12) return false;
    g_originalD3D12CreateDevice = reinterpret_cast<D3D12CreateDevice_t>(GetProcAddress(g_originalD3D12, "D3D12CreateDevice"));
    return g_originalD3D12CreateDevice != nullptr;
}

extern "C" FARPROC WINAPI GetOriginalProcByName(const char* name) {
    if (!LoadOriginalD3D12()) return nullptr;
    return GetProcAddress(g_originalD3D12, name);
}

extern "C" FARPROC WINAPI GetOriginalProcByOrdinal(WORD ordinal) {
    if (!LoadOriginalD3D12()) return nullptr;
    return GetProcAddress(g_originalD3D12, reinterpret_cast<LPCSTR>(ordinal));
}

// SPOOF: Interceptador do orçamento de memória de vídeo (Estrutura mapeada manualmente via ponteiro de uint64)
static HRESULT STDMETHODCALLTYPE HookedQueryVideoMemoryInfo(
    void* self,
    UINT NodeIndex,
    int MemorySegmentGroup,
    void* pVideoMemoryInfo
) {
    HRESULT hr = S_OK;
    if (g_originalQueryVideoMemoryInfo) {
        hr = g_originalQueryVideoMemoryInfo(self, NodeIndex, MemorySegmentGroup, pVideoMemoryInfo);
    }

    if (!pVideoMemoryInfo) return hr;

    // Mapeamento direto na memória da estrutura D3D12_QUERY_VIDEO_MEMORY_INFO
    uint64_t* memoryInfoFields = reinterpret_cast<uint64_t*>(pVideoMemoryInfo);
    memoryInfoFields[0] = 4294967296; // Budget = 4 GB
    memoryInfoFields[1] = 1048576;    // CurrentUsage = 1 MB
    memoryInfoFields[2] = 0;          // CurrentReservation = 0
    memoryInfoFields[3] = 4294967296; // AvailableForReservation = 4 GB

    Log("SPOOF: QueryVideoMemoryInfo interceptado! Forçando 4GB VRAM Budget.");
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE HookedCheckFeatureSupport(
    ID3D12Device* self,
    D3D12_FEATURE feature,
    void* pFeatureSupportData,
    UINT featureSupportDataSize
) {
    HRESULT hr = g_originalCheckFeatureSupport(self, feature, pFeatureSupportData, featureSupportDataSize);

    if (!pFeatureSupportData) return hr;

    if (feature == D3D12_FEATURE_FEATURE_LEVELS && featureSupportDataSize >= sizeof(D3D12_FEATURE_DATA_FEATURE_LEVELS)) {
        auto* levels = reinterpret_cast<D3D12_FEATURE_DATA_FEATURE_LEVELS*>(pFeatureSupportData);
        levels->MaxSupportedFeatureLevel = D3D_FEATURE_LEVEL_12_1;
        Log("SPOOF: MaxSupportedFeatureLevel -> 12_1");
        return S_OK;
    }

    if (feature == D3D12_FEATURE_SHADER_MODEL && featureSupportDataSize >= sizeof(D3D12_FEATURE_DATA_SHADER_MODEL)) {
        auto* sm = reinterpret_cast<D3D12_FEATURE_DATA_SHADER_MODEL*>(pFeatureSupportData);
        sm->HighestShaderModel = D3D_SHADER_MODEL_6_6;
        Log("SPOOF: Shader Model forced to 6.6");
        return S_OK;
    }

    if (feature == D3D12_FEATURE_D3D12_OPTIONS12 && featureSupportDataSize >= sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS12)) {
        auto* opts12 = reinterpret_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS12*>(pFeatureSupportData);
        opts12->EnhancedBarriersSupported = TRUE;
        Log("SPOOF: EnhancedBarriersSupported FORCED TRUE");
        return S_OK;
    }

    if (feature == D3D12_FEATURE_D3D12_OPTIONS7 && featureSupportDataSize >= sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS7)) {
        auto* opts7 = reinterpret_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS7*>(pFeatureSupportData);
        opts7->MeshShaderTier = D3D12_MESH_SHADER_TIER_1;
        opts7->SamplerFeedbackTier = D3D12_SAMPLER_FEEDBACK_TIER_1_0;
        Log("SPOOF: MeshShaderTier forced to TIER_1");
        return S_OK;
    }

    return hr;
}

static void PatchDeviceInterfaces(IUnknown* deviceUnknown) {
    ID3D12Device* device = nullptr;
    if (FAILED(deviceUnknown->QueryInterface(IID_PPV_ARGS(&device)))) return;

    void*** object = reinterpret_cast<void***>(device);
    void** vtable = *object;

    DWORD oldProtect;

    if (!g_originalCheckFeatureSupport) {
        g_originalCheckFeatureSupport = reinterpret_cast<CheckFeatureSupport_t>(vtable[kCheckFeatureSupportVtableIndex]);
    }
    VirtualProtect(&vtable[kCheckFeatureSupportVtableIndex], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect);
    vtable[kCheckFeatureSupportVtableIndex] = reinterpret_cast<void*>(&HookedCheckFeatureSupport);
    VirtualProtect(&vtable[kCheckFeatureSupportVtableIndex], sizeof(void*), oldProtect, &oldProtect);

    if (!g_originalQueryVideoMemoryInfo) {
        g_originalQueryVideoMemoryInfo = reinterpret_cast<QueryVideoMemoryInfo_t>(vtable[kQueryVideoMemoryInfoVtableIndex]);
    }
    VirtualProtect(&vtable[kQueryVideoMemoryInfoVtableIndex], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect);
    vtable[kQueryVideoMemoryInfoVtableIndex] = reinterpret_cast<void*>(&HookedQueryVideoMemoryInfo);
    VirtualProtect(&vtable[kQueryVideoMemoryInfoVtableIndex], sizeof(void*), oldProtect, &oldProtect);

    Log("Patch aplicado: ID3D12Device Spoof de Recursos e VRAM Ativos");
    device->Release();
}

extern "C" HRESULT WINAPI D3D12CreateDevice(IUnknown* pAdapter, D3D_FEATURE_LEVEL minimumFeatureLevel, REFIID riid, void** ppDevice) {
    if (!LoadOriginalD3D12()) return E_FAIL;

    HRESULT hr = g_originalD3D12CreateDevice(pAdapter, D3D_FEATURE_LEVEL_11_0, riid, ppDevice);
    if (SUCCEEDED(hr) && ppDevice && *ppDevice) {
        PatchDeviceInterfaces(reinterpret_cast<IUnknown*>(*ppDevice));
    }
    return hr;
}

BOOL APIENTRY DllMain(HMODULE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        Log("=== d3d12.dll proxy com Anti-Crash VRAM carregado ===");
    }
    return TRUE;
}
