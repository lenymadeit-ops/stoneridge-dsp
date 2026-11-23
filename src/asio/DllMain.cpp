#include "asio/StoneridgeASIO.h"

#include <atomic>
#include <string>

HMODULE g_hModule = nullptr;
static std::atomic<LONG> g_serverLocks{0};

namespace {

class StoneridgeClassFactory final : public IClassFactory {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IClassFactory) {
            *ppv = static_cast<IClassFactory*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return static_cast<ULONG>(InterlockedIncrement(&m_ref));
    }

    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG c = static_cast<ULONG>(InterlockedDecrement(&m_ref));
        if (c == 0) delete this;
        return c;
    }

    HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* pUnkOuter, REFIID riid, void** ppv) override {
        if (pUnkOuter) return CLASS_E_NOAGGREGATION;
        auto* drv = new (std::nothrow) stoneridge::StoneridgeASIO();
        if (!drv) return E_OUTOFMEMORY;
        const HRESULT hr = drv->QueryInterface(riid, ppv);
        drv->Release();
        return hr;
    }

    HRESULT STDMETHODCALLTYPE LockServer(BOOL fLock) override {
        if (fLock) g_serverLocks.fetch_add(1, std::memory_order_relaxed);
        else       g_serverLocks.fetch_sub(1, std::memory_order_relaxed);
        return S_OK;
    }

private:
    LONG m_ref = 1;
};

HRESULT WriteRegString(HKEY root, const wchar_t* subKey, const wchar_t* valueName, const wchar_t* data) {
    HKEY hKey = nullptr;
    const LONG res = RegCreateKeyExW(root, subKey, 0, nullptr, REG_OPTION_NON_VOLATILE,
                                     KEY_WRITE, nullptr, &hKey, nullptr);
    if (res != ERROR_SUCCESS) return HRESULT_FROM_WIN32(res);
    const DWORD bytes = static_cast<DWORD>((wcslen(data) + 1) * sizeof(wchar_t));
    const LONG setRes = RegSetValueExW(hKey, valueName, 0, REG_SZ,
                                       reinterpret_cast<const BYTE*>(data), bytes);
    RegCloseKey(hKey);
    return HRESULT_FROM_WIN32(setRes);
}

} // namespace

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID) {
    if (ul_reason_for_call == DLL_PROCESS_ATTACH) {
        g_hModule = hModule;
        DisableThreadLibraryCalls(hModule);
    }
    return TRUE;
}

_Check_return_ STDAPI DllGetClassObject(_In_ REFCLSID rclsid, _In_ REFIID riid, _Outptr_ LPVOID* ppv) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    if (rclsid != stoneridge::CLSID_StoneridgeASIO) {
        return CLASS_E_CLASSNOTAVAILABLE;
    }
    auto* factory = new (std::nothrow) StoneridgeClassFactory();
    if (!factory) return E_OUTOFMEMORY;
    const HRESULT hr = factory->QueryInterface(riid, ppv);
    factory->Release();
    return hr;
}

__control_entrypoint(DllExport) STDAPI DllCanUnloadNow() {
    return (g_serverLocks.load(std::memory_order_relaxed) == 0) ? S_OK : S_FALSE;
}

STDAPI DllRegisterServer() {
    wchar_t modulePath[MAX_PATH]{};
    if (GetModuleFileNameW(g_hModule, modulePath, MAX_PATH) == 0) {
        return HRESULT_FROM_WIN32(GetLastError());
    }

    const std::wstring clsidKey = std::wstring(L"CLSID\\") + stoneridge::kClsidString;
    const std::wstring inprocKey = clsidKey + L"\\InprocServer32";

    HRESULT hr = WriteRegString(HKEY_CLASSES_ROOT, clsidKey.c_str(), nullptr, stoneridge::kDriverNameW);
    if (FAILED(hr)) return hr;

    hr = WriteRegString(HKEY_CLASSES_ROOT, inprocKey.c_str(), nullptr, modulePath);
    if (FAILED(hr)) return hr;

    hr = WriteRegString(HKEY_CLASSES_ROOT, inprocKey.c_str(), L"ThreadingModel", L"Both");
    if (FAILED(hr)) return hr;

    const std::wstring asioKey = std::wstring(L"SOFTWARE\\ASIO\\") + stoneridge::kDriverNameW;
    hr = WriteRegString(HKEY_LOCAL_MACHINE, asioKey.c_str(), L"CLSID", stoneridge::kClsidString);
    if (FAILED(hr)) return hr;

    hr = WriteRegString(HKEY_LOCAL_MACHINE, asioKey.c_str(), L"Description", stoneridge::kDriverNameW);
    return hr;
}

STDAPI DllUnregisterServer() {
    const std::wstring clsidKey = std::wstring(L"CLSID\\") + stoneridge::kClsidString;
    RegDeleteTreeW(HKEY_CLASSES_ROOT, clsidKey.c_str());

    const std::wstring asioKey = std::wstring(L"SOFTWARE\\ASIO\\") + stoneridge::kDriverNameW;
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, asioKey.c_str());
    return S_OK;
}
