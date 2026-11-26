#pragma once
#include <windows.h>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace resorep::ui {

struct FileFilter {
    const wchar_t* name;
    const wchar_t* spec;
};

std::vector<std::filesystem::path> PickFiles(HWND owner, const wchar_t* title, const std::vector<FileFilter>& filters,
                                             bool multiple);
std::optional<std::filesystem::path> PickFolder(HWND owner, const wchar_t* title);
std::optional<std::filesystem::path> PickSaveFile(HWND owner, const wchar_t* title, const wchar_t* defaultName,
                                                  const std::vector<FileFilter>& filters,
                                                  const wchar_t* defaultExtension = L"txt");

bool MoveToRecycleBin(const std::filesystem::path& path);
void ShowInExplorer(const std::filesystem::path& path);
// Opens a file, folder or URL with its default program. False when Windows could not (for
// example, no program is set up for that file type).
bool OpenWithShell(const std::filesystem::path& path);
// Starts a program with its own folder as working directory. Returns an error message or "".
std::string LaunchProgram(const std::filesystem::path& exe);

// Process ids running exactly this exe (full path, case-insensitive).
std::vector<DWORD> FindProcesses(const std::filesystem::path& exe);

struct CloseResult {
    size_t closed = 0;  // exited after being asked to close
    size_t killed = 0;  // had to be ended forcefully
    std::string error;
};
// Asks every process running `exe` to close (WM_CLOSE to its windows), waits up to `grace`,
// then ends whatever is still running. Blocks; call off the UI thread.
CloseResult CloseProgram(const std::filesystem::path& exe, DWORD graceMs);

// CompanyName from the file's version resource ("" if none).
std::wstring FileCompanyName(const std::filesystem::path& path);
// "major.minor.patch" from the file's version resource ("" if none).
std::string FileVersionString(const std::filesystem::path& path);
bool FileContains(const std::filesystem::path& path, const std::string& needle);
bool FilesEqual(const std::filesystem::path& a, const std::filesystem::path& b);

std::filesystem::path ExeDirectory();
std::string FormatBytes(uint64_t bytes);
std::string ToUtf8(const std::filesystem::path& p);

}  // namespace resorep::ui
