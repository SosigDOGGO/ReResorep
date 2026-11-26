#include "win_util.h"

#include <shellapi.h>
#include <tlhelp32.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iterator>

#include "settings.h"

namespace resorep::ui {
namespace fs = std::filesystem;
using Microsoft::WRL::ComPtr;

std::vector<fs::path> PickFiles(HWND owner, const wchar_t* title, const std::vector<FileFilter>& filters,
                                bool multiple) {
    std::vector<fs::path> out;
    ComPtr<IFileOpenDialog> dlg;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return out;

    std::vector<COMDLG_FILTERSPEC> specs;
    for (const auto& f : filters) specs.push_back({f.name, f.spec});
    if (!specs.empty()) dlg->SetFileTypes(UINT(specs.size()), specs.data());
    dlg->SetTitle(title);
    FILEOPENDIALOGOPTIONS opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | (multiple ? FOS_ALLOWMULTISELECT : 0));
    if (FAILED(dlg->Show(owner))) return out;

    ComPtr<IShellItemArray> items;
    if (FAILED(dlg->GetResults(&items))) return out;
    DWORD count = 0;
    items->GetCount(&count);
    for (DWORD i = 0; i < count; ++i) {
        ComPtr<IShellItem> item;
        PWSTR path = nullptr;
        if (SUCCEEDED(items->GetItemAt(i, &item)) && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
            out.emplace_back(path);
            CoTaskMemFree(path);
        }
    }
    return out;
}

std::optional<fs::path> PickFolder(HWND owner, const wchar_t* title) {
    ComPtr<IFileOpenDialog> dlg;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg))))
        return std::nullopt;
    dlg->SetTitle(title);
    FILEOPENDIALOGOPTIONS opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    ComPtr<IShellItem> item;
    PWSTR path = nullptr;
    if (FAILED(dlg->Show(owner)) || FAILED(dlg->GetResult(&item)) ||
        FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)))
        return std::nullopt;
    fs::path result(path);
    CoTaskMemFree(path);
    return result;
}

std::optional<fs::path> PickSaveFile(HWND owner, const wchar_t* title, const wchar_t* defaultName,
                                     const std::vector<FileFilter>& filters, const wchar_t* defaultExtension) {
    ComPtr<IFileSaveDialog> dlg;
    if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg))))
        return std::nullopt;
    std::vector<COMDLG_FILTERSPEC> specs;
    for (const auto& f : filters) specs.push_back({f.name, f.spec});
    if (!specs.empty()) dlg->SetFileTypes(UINT(specs.size()), specs.data());
    dlg->SetTitle(title);
    dlg->SetFileName(defaultName);
    dlg->SetDefaultExtension(defaultExtension);
    ComPtr<IShellItem> item;
    PWSTR path = nullptr;
    if (FAILED(dlg->Show(owner)) || FAILED(dlg->GetResult(&item)) ||
        FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)))
        return std::nullopt;
    fs::path result(path);
    CoTaskMemFree(path);
    return result;
}

bool MoveToRecycleBin(const fs::path& path) {
    std::wstring from = path.wstring();
    from.push_back(L'\0');  // SHFileOperation wants a double-null-terminated list
    SHFILEOPSTRUCTW op{};
    op.wFunc = FO_DELETE;
    op.pFrom = from.c_str();
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
    return SHFileOperationW(&op) == 0 && !op.fAnyOperationsAborted;
}

void ShowInExplorer(const fs::path& path) {
    if (PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(path.c_str())) {
        SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
        ILFree(pidl);
    }
}

bool OpenWithShell(const fs::path& path) {
    // ShellExecute returns a value above 32 on success (an HINSTANCE kept for 16-bit compatibility).
    return reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) > 32;
}

std::string LaunchProgram(const fs::path& exe) {
    std::error_code ec;
    if (!fs::exists(exe, ec)) return "Game executable not found";
    const std::wstring dir = exe.parent_path().wstring();
    SHELLEXECUTEINFOW info{sizeof(info)};
    info.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    info.lpVerb = L"open";
    info.lpFile = exe.c_str();
    info.lpDirectory = dir.c_str();  // games load data (and our d3d11.dll) relative to their folder
    info.nShow = SW_SHOWNORMAL;
    if (ShellExecuteExW(&info)) return {};
    const DWORD err = GetLastError();
    return err == ERROR_CANCELLED ? "Start was cancelled" : "Could not start the game (error " + std::to_string(err) + ")";
}

std::string FileVersionString(const fs::path& path) {
    DWORD ignored = 0;
    const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &ignored);
    if (!size) return {};
    std::vector<uint8_t> data(size);
    VS_FIXEDFILEINFO* info = nullptr;
    UINT len = 0;
    if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data()) ||
        !VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info), &len) || !info)
        return {};
    return std::to_string(HIWORD(info->dwFileVersionMS)) + "." + std::to_string(LOWORD(info->dwFileVersionMS)) + "." +
           std::to_string(HIWORD(info->dwFileVersionLS));
}

std::vector<DWORD> FindProcesses(const fs::path& exe) {
    std::vector<DWORD> pids;
    std::error_code ec;
    const fs::path target = fs::weakly_canonical(exe, ec);
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return pids;
    PROCESSENTRY32W pe{sizeof(pe)};
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
        // Cheap name check first; only matching names get their full path looked up.
        if (_wcsicmp(pe.szExeFile, exe.filename().c_str()) != 0) continue;
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
        if (!h) continue;
        wchar_t path[MAX_PATH * 4];
        DWORD len = DWORD(std::size(path));
        if (QueryFullProcessImageNameW(h, 0, path, &len) &&
            _wcsicmp(fs::weakly_canonical(fs::path(std::wstring(path, len)), ec).c_str(), target.c_str()) == 0)
            pids.push_back(pe.th32ProcessID);
        CloseHandle(h);
    }
    CloseHandle(snap);
    return pids;
}

CloseResult CloseProgram(const fs::path& exe, DWORD graceMs) {
    CloseResult r;
    std::vector<HANDLE> handles;
    for (DWORD pid : FindProcesses(exe)) {
        if (HANDLE h = OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid))
            handles.push_back(h);
        else
            r.error = "Access denied to the game process (try running ReResorep as administrator)";
    }
    if (handles.empty()) return r;

    // The polite way: what clicking the window's close button does.
    std::vector<DWORD> pids;
    for (HANDLE h : handles) pids.push_back(GetProcessId(h));
    EnumWindows(
        [](HWND hwnd, LPARAM param) -> BOOL {
            const auto& ids = *reinterpret_cast<const std::vector<DWORD>*>(param);
            DWORD pid = 0;
            GetWindowThreadProcessId(hwnd, &pid);
            if (std::find(ids.begin(), ids.end(), pid) != ids.end() && !GetWindow(hwnd, GW_OWNER))
                PostMessageW(hwnd, WM_CLOSE, 0, 0);
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&pids));

    const ULONGLONG deadline = GetTickCount64() + graceMs;
    for (HANDLE h : handles) {
        const ULONGLONG now = GetTickCount64();
        const DWORD wait = now < deadline ? DWORD(deadline - now) : 0;
        if (WaitForSingleObject(h, wait) == WAIT_OBJECT_0) {
            ++r.closed;
            continue;
        }
        // Still running after the grace period: end it.
        if (TerminateProcess(h, 1) && WaitForSingleObject(h, 5000) == WAIT_OBJECT_0)
            ++r.killed;
        else
            r.error = "Could not end the game process (try running ReResorep as administrator)";
    }
    for (HANDLE h : handles) CloseHandle(h);
    return r;
}

std::wstring FileCompanyName(const fs::path& path) {
    DWORD ignored = 0;
    const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &ignored);
    if (!size) return {};
    std::vector<uint8_t> data(size);
    if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data())) return {};

    struct LangCodePage { WORD lang, codePage; } *translations = nullptr;
    UINT len = 0;
    if (!VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<void**>(&translations), &len) ||
        len < sizeof(LangCodePage))
        return {};
    wchar_t key[64];
    swprintf_s(key, L"\\StringFileInfo\\%04x%04x\\CompanyName", translations[0].lang, translations[0].codePage);
    wchar_t* value = nullptr;
    if (!VerQueryValueW(data.data(), key, reinterpret_cast<void**>(&value), &len) || !value) return {};
    return value;
}

namespace {
// The whole file in one read; reading it a character at a time is far slower.
std::optional<std::string> ReadWholeFile(const fs::path& path) {
    std::error_code ec;
    const auto size = fs::file_size(path, ec);
    if (ec) return std::nullopt;
    std::ifstream in(path, std::ios::binary);
    std::string data(size_t(size), '\0');
    if (!in.read(data.data(), std::streamsize(data.size()))) return std::nullopt;
    return data;
}
}  // namespace

bool FileContains(const fs::path& path, const std::string& needle) {
    const auto data = ReadWholeFile(path);
    return data && data->find(needle) != std::string::npos;
}

bool FilesEqual(const fs::path& a, const fs::path& b) {
    std::error_code ea, eb;
    const auto sizeA = fs::file_size(a, ea);
    const auto sizeB = fs::file_size(b, eb);
    if (ea || eb || sizeA != sizeB) return false;
    const auto da = ReadWholeFile(a), db = ReadWholeFile(b);
    return da && db && *da == *db;
}

fs::path ExeDirectory() {
    wchar_t buf[MAX_PATH * 4];
    const DWORD n = GetModuleFileNameW(nullptr, buf, DWORD(std::size(buf)));
    return fs::path(std::wstring(buf, n)).parent_path();
}

std::string FormatBytes(uint64_t bytes) {
    char buf[32];
    if (bytes >= (1ull << 20)) snprintf(buf, sizeof(buf), "%.1f MB", double(bytes) / (1 << 20));
    else if (bytes >= 1024) snprintf(buf, sizeof(buf), "%.0f KB", double(bytes) / 1024);
    else snprintf(buf, sizeof(buf), "%llu B", static_cast<unsigned long long>(bytes));
    return buf;
}

std::string ToUtf8(const fs::path& p) { return WideToUtf8(p.wstring()); }

}  // namespace resorep::ui
