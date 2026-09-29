#include <objbase.h>
#include <shlwapi.h>
#include <thumbcache.h> // For IThumbnailProvider.
#include <shlobj.h>     // For SHChangeNotify
#include <new>
#include <strsafe.h>

#include "log.h"

extern HRESULT CHEICThumbProvider_CreateInstance(REFIID riid, void** ppv);

#define SZ_CLSID_HEICTHUMBHANDLER     L"{2c93d534-2a1f-40d2-a375-babc92996987}"
#define SZ_HEICTHUMBHANDLER           L"HEIC Thumbnail Handler"

const CLSID CLSID_HEICThumbHandler = { 0x2c93d534, 0x2a1f, 0x40d2, {0xa3, 0x75, 0xba, 0xbc, 0x92, 0x99, 0x69, 0x87} };

typedef HRESULT(*PFNCREATEINSTANCE)(REFIID riid, void** ppvObject);
struct CLASS_OBJECT_INIT
{
    const CLSID* pClsid;
    PFNCREATEINSTANCE pfnCreate;
};

// add classes supported by this module here
const CLASS_OBJECT_INIT c_rgClassObjectInit[] =
{
    { &CLSID_HEICThumbHandler, CHEICThumbProvider_CreateInstance }
};


long g_cRefModule = 0;

// Handle the the DLL's module
HINSTANCE g_hInst = NULL;

// Standard DLL functions
STDAPI_(BOOL) DllMain(HINSTANCE hInstance, DWORD dwReason, void*)
{
    if (dwReason == DLL_PROCESS_ATTACH)
    {
        g_hInst = hInstance;
        DisableThreadLibraryCalls(hInstance);

        DWORD dwLevel = LOG_NONE;

        HKEY hk = 0;
        HRESULT hr = HRESULT_FROM_WIN32(RegOpenKeyEx(HKEY_CURRENT_USER, 
            L"Software\\Classes\\CLSID\\" SZ_CLSID_HEICTHUMBHANDLER, 
            0, KEY_QUERY_VALUE, &hk));
        //Log_WriteFmt(LOG_NONE, L"RegOpenKeyEx: 0x%08x", hr);
        if (SUCCEEDED(hr))
        {

            DWORD dwType = 0;
            DWORD dwSize = sizeof(DWORD);
            hr = HRESULT_FROM_WIN32(RegQueryValueEx(hk, L"LogLevel", 0, &dwType, (LPBYTE) &dwLevel, &dwSize));
            //Log_WriteFmt(LOG_NONE, L"RegQueryValueEx: 0x%08x", hr);
            if (SUCCEEDED(hr))
            {
                if (dwType == REG_DWORD && 
                    dwLevel >= LOG_NONE && 
                    dwLevel < LOG_MAX)
                {
                    Log_SetLevel((LOG_LEVEL)dwLevel);
                    if (dwLevel != LOG_NONE)
                        Log_Open(L"HEICThumbProvider");
                }
            }

            RegCloseKey(hk);
        }

        //Log_WriteFmt(LOG_NONE, L"LogLevel: %u", dwLevel);
    }
    else if (dwReason == DLL_PROCESS_DETACH)
    {
        Log_Close();
    }
    return TRUE;
}

STDAPI DllCanUnloadNow()
{
    // Only allow the DLL to be unloaded after all outstanding references have been released
    return (g_cRefModule == 0) ? S_OK : S_FALSE;
}

void DllAddRef()
{
    InterlockedIncrement(&g_cRefModule);
}

void DllRelease()
{
    InterlockedDecrement(&g_cRefModule);
}

class CClassFactory : public IClassFactory
{
public:
    static HRESULT CreateInstance(REFCLSID clsid, const CLASS_OBJECT_INIT* pClassObjectInits, size_t cClassObjectInits, REFIID riid, void** ppv)
    {
        *ppv = NULL;
        HRESULT hr = CLASS_E_CLASSNOTAVAILABLE;
        for (size_t i = 0; i < cClassObjectInits; i++)
        {
            if (clsid == *pClassObjectInits[i].pClsid)
            {
                IClassFactory* pClassFactory = new (std::nothrow) CClassFactory(pClassObjectInits[i].pfnCreate);
                hr = pClassFactory ? S_OK : E_OUTOFMEMORY;
                if (SUCCEEDED(hr))
                {
                    hr = pClassFactory->QueryInterface(riid, ppv);
                    pClassFactory->Release();
                }
                break; // match found
            }
        }
        return hr;
    }

    CClassFactory(PFNCREATEINSTANCE pfnCreate) : _cRef(1), _pfnCreate(pfnCreate)
    {
        DllAddRef();
    }

    // IUnknown
    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv)
    {
        static const QITAB qit[] =
        {
            QITABENT(CClassFactory, IClassFactory),
            { 0 }
        };
        return QISearch(this, qit, riid, ppv);
    }

    IFACEMETHODIMP_(ULONG) AddRef()
    {
        return InterlockedIncrement(&_cRef);
    }

    IFACEMETHODIMP_(ULONG) Release()
    {
        long cRef = InterlockedDecrement(&_cRef);
        if (cRef == 0)
        {
            delete this;
        }
        return cRef;
    }

    // IClassFactory
    IFACEMETHODIMP CreateInstance(IUnknown* punkOuter, REFIID riid, void** ppv)
    {
        return punkOuter ? CLASS_E_NOAGGREGATION : _pfnCreate(riid, ppv);
    }

    IFACEMETHODIMP LockServer(BOOL fLock)
    {
        if (fLock)
        {
            DllAddRef();
        }
        else
        {
            DllRelease();
        }
        return S_OK;
    }

private:
    ~CClassFactory()
    {
        DllRelease();
    }

    long _cRef;
    PFNCREATEINSTANCE _pfnCreate;
};

STDAPI DllGetClassObject(REFCLSID clsid, REFIID riid, void** ppv)
{
    return CClassFactory::CreateInstance(clsid, c_rgClassObjectInit, ARRAYSIZE(c_rgClassObjectInit), riid, ppv);
}

// A struct to hold the information required for a registry entry

struct REGISTRY_ENTRY
{
    HKEY   hkeyRoot;
    PCWSTR pszKeyName;
    PCWSTR pszValueName;
    PCWSTR pszData;
};

// Creates a registry key (if needed) and sets the default value of the key

HRESULT CreateRegKeyAndSetValue(const REGISTRY_ENTRY* pRegistryEntry)
{
    HKEY hKey;
    HRESULT hr = HRESULT_FROM_WIN32(RegCreateKeyExW(pRegistryEntry->hkeyRoot, pRegistryEntry->pszKeyName,
        0, NULL, REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, NULL, &hKey, NULL));
    if (SUCCEEDED(hr))
    {
        hr = HRESULT_FROM_WIN32(RegSetValueExW(hKey, pRegistryEntry->pszValueName, 0, REG_SZ,
            (LPBYTE)pRegistryEntry->pszData,
            ((DWORD)wcslen(pRegistryEntry->pszData) + 1) * sizeof(WCHAR)));
        RegCloseKey(hKey);
    }
    return hr;
}

const PCWSTR thumbnailTypes[] = { L".heic", L".heif", L".dng" };
const PCWSTR previousValues[] = { L"PreviousHeic", L"PreviousHeif", L"PreviousDng" };

void ThumbnailKey(PCWSTR type, WCHAR* path, size_t capacity)
{
    StringCchPrintfW(path, capacity, L"Software\\Classes\\%s\\ShellEx\\{e357fccd-a995-4576-b01f-234630154e96}", type);
}

HRESULT RegisterThumbnailType(size_t index)
{
    WCHAR path[160] = {};
    ThumbnailKey(thumbnailTypes[index], path, ARRAYSIZE(path));
    WCHAR effectivePath[160] = {};
    StringCchPrintfW(effectivePath, ARRAYSIZE(effectivePath),
        L"%s\\ShellEx\\{e357fccd-a995-4576-b01f-234630154e96}", thumbnailTypes[index]);
    WCHAR previous[64] = {};
    DWORD bytes = sizeof(previous);
    HKEY existing = nullptr;
    if (RegOpenKeyExW(HKEY_CLASSES_ROOT, effectivePath, 0, KEY_QUERY_VALUE, &existing) == ERROR_SUCCESS)
    {
        DWORD type = 0;
        if (RegQueryValueExW(existing, nullptr, nullptr, &type, reinterpret_cast<BYTE*>(previous), &bytes) != ERROR_SUCCESS ||
            type != REG_SZ || bytes > sizeof(previous)) previous[0] = 0;
        RegCloseKey(existing);
    }
    if (wcscmp(previous, SZ_CLSID_HEICTHUMBHANDLER) != 0)
    {
        const REGISTRY_ENTRY backup = { HKEY_CURRENT_USER,
            L"Software\\Classes\\CLSID\\" SZ_CLSID_HEICTHUMBHANDLER, previousValues[index], previous };
        HRESULT hr = CreateRegKeyAndSetValue(&backup);
        if (FAILED(hr)) return hr;
    }
    const REGISTRY_ENTRY association = { HKEY_CURRENT_USER, path, nullptr, SZ_CLSID_HEICTHUMBHANDLER };
    return CreateRegKeyAndSetValue(&association);
}

HRESULT UnregisterThumbnailType(size_t index)
{
    WCHAR path[160] = {};
    ThumbnailKey(thumbnailTypes[index], path, ARRAYSIZE(path));
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, path, 0, KEY_QUERY_VALUE | KEY_SET_VALUE, &key) != ERROR_SUCCESS)
        return S_OK;
    WCHAR current[64] = {};
    DWORD bytes = sizeof(current), type = 0;
    LONG status = RegQueryValueExW(key, nullptr, nullptr, &type, reinterpret_cast<BYTE*>(current), &bytes);
    if (status != ERROR_SUCCESS || type != REG_SZ || wcscmp(current, SZ_CLSID_HEICTHUMBHANDLER) != 0)
    {
        RegCloseKey(key);
        return S_OK;
    }
    WCHAR previous[64] = {};
    bytes = sizeof(previous);
    HKEY handler = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Classes\\CLSID\\" SZ_CLSID_HEICTHUMBHANDLER,
        0, KEY_QUERY_VALUE, &handler) == ERROR_SUCCESS)
    {
        if (RegQueryValueExW(handler, previousValues[index], nullptr, &type,
            reinterpret_cast<BYTE*>(previous), &bytes) != ERROR_SUCCESS || type != REG_SZ || bytes > sizeof(previous))
            previous[0] = 0;
        RegCloseKey(handler);
    }
    status = previous[0] ? RegSetValueExW(key, nullptr, 0, REG_SZ,
        reinterpret_cast<const BYTE*>(previous), static_cast<DWORD>((wcslen(previous) + 1) * sizeof(WCHAR))) :
        RegDeleteValueW(key, nullptr);
    RegCloseKey(key);
    return status == ERROR_FILE_NOT_FOUND ? S_OK : HRESULT_FROM_WIN32(status);
}

//
// Registers this COM server
//
STDAPI DllRegisterServer()
{
    HRESULT hr;

    WCHAR szModuleName[MAX_PATH];

    if (!GetModuleFileNameW(g_hInst, szModuleName, ARRAYSIZE(szModuleName)))
    {
        hr = HRESULT_FROM_WIN32(GetLastError());
    }
    else
    {
        // List of registry entries we want to create
        const REGISTRY_ENTRY rgRegistryEntries[] =
        {
            // RootKey            KeyName                                                                ValueName                     Data
            {HKEY_CURRENT_USER,   L"Software\\Classes\\CLSID\\" SZ_CLSID_HEICTHUMBHANDLER,                                 NULL,                           SZ_HEICTHUMBHANDLER},
            {HKEY_CURRENT_USER,   L"Software\\Classes\\CLSID\\" SZ_CLSID_HEICTHUMBHANDLER L"\\InProcServer32",             NULL,                           szModuleName},
            {HKEY_CURRENT_USER,   L"Software\\Classes\\CLSID\\" SZ_CLSID_HEICTHUMBHANDLER L"\\InProcServer32",             L"ThreadingModel",              L"Apartment"},
        };

        hr = S_OK;
        for (int i = 0; i < ARRAYSIZE(rgRegistryEntries) && SUCCEEDED(hr); i++)
        {
            hr = CreateRegKeyAndSetValue(&rgRegistryEntries[i]);
        }
        for (size_t i = 0; i < ARRAYSIZE(thumbnailTypes) && SUCCEEDED(hr); ++i)
            hr = RegisterThumbnailType(i);
    }
    if (SUCCEEDED(hr))
    {
        // This tells the shell to invalidate the thumbnail cache.  This is important because any .heic files
        // viewed before registering this handler would otherwise show cached blank thumbnails.
        SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, NULL, NULL);
    }
    return hr;
}

//
// Unregisters this COM server
//
STDAPI DllUnregisterServer()
{
    HRESULT hr = S_OK;

    for (size_t i = 0; i < ARRAYSIZE(thumbnailTypes) && SUCCEEDED(hr); ++i)
        hr = UnregisterThumbnailType(i);
    if (SUCCEEDED(hr))
    {
        hr = HRESULT_FROM_WIN32(RegDeleteTreeW(HKEY_CURRENT_USER,
            L"Software\\Classes\\CLSID\\" SZ_CLSID_HEICTHUMBHANDLER));
        if (hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)) hr = S_OK;
    }
    if (SUCCEEDED(hr)) SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return hr;
}
