#include "switchbox/core/playback_launcher.hpp"
#include "switchbox/core/switch_mpv_player.hpp"

#if defined(_WIN32) && !defined(__SWITCH__)
#include <windows.h>
#include <cstdlib>
#include <vector>
#endif

namespace switchbox::core {

namespace {

#if defined(_WIN32) && !defined(__SWITCH__)
std::wstring utf8_to_wstring(const std::string& value) {
    if (value.empty()) {
        return {};
    }

    const int wide_size = MultiByteToWideChar(
        CP_UTF8,
        0,
        value.data(),
        static_cast<int>(value.size()),
        nullptr,
        0);
    if (wide_size <= 0) {
        return {};
    }

    std::wstring wide(static_cast<size_t>(wide_size), L'\0');
    const int converted = MultiByteToWideChar(
        CP_UTF8,
        0,
        value.data(),
        static_cast<int>(value.size()),
        wide.data(),
        wide_size);
    if (converted != wide_size) {
        return {};
    }

    return wide;
}

std::string wstring_to_utf8(const std::wstring& value) {
    if (value.empty()) {
        return {};
    }

    const int utf8_size = WideCharToMultiByte(
        CP_UTF8,
        0,
        value.data(),
        static_cast<int>(value.size()),
        nullptr,
        0,
        nullptr,
        nullptr);
    if (utf8_size <= 0) {
        return {};
    }

    std::string utf8(static_cast<size_t>(utf8_size), '\0');
    const int converted = WideCharToMultiByte(
        CP_UTF8,
        0,
        value.data(),
        static_cast<int>(value.size()),
        utf8.data(),
        utf8_size,
        nullptr,
        nullptr);
    if (converted != utf8_size) {
        return {};
    }

    return utf8;
}

std::wstring quote_argument(const std::wstring& value) {
    std::wstring escaped;
    escaped.push_back(L'"');
    for (const wchar_t ch : value) {
        if (ch == L'"') {
            escaped += L"\\\"";
        } else {
            escaped.push_back(ch);
        }
    }
    escaped.push_back(L'"');
    return escaped;
}

std::string win32_error_message(DWORD code) {
    LPWSTR message_buffer = nullptr;
    const DWORD size = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        code,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPWSTR>(&message_buffer),
        0,
        nullptr);

    if (size == 0 || message_buffer == nullptr) {
        return "Win32 error " + std::to_string(code) + ".";
    }

    std::wstring message(message_buffer, size);
    LocalFree(message_buffer);

    while (!message.empty() &&
           (message.back() == L'\r' || message.back() == L'\n' || message.back() == L' ')) {
        message.pop_back();
    }

    const std::string utf8_message = wstring_to_utf8(message);
    if (utf8_message.empty()) {
        return "Win32 error " + std::to_string(code) + ".";
    }

    return utf8_message + " (code " + std::to_string(code) + ").";
}

std::wstring desktop_exe_directory() {
    std::wstring path(MAX_PATH, L'\0');
    DWORD copied = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (copied == 0) {
        return {};
    }

    while (copied == path.size()) {
        path.resize(path.size() * 2, L'\0');
        copied = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (copied == 0) {
            return {};
        }
    }

    path.resize(copied);
    const size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) {
        return {};
    }

    return path.substr(0, slash);
}

std::wstring env_to_wstring(const char* name) {
    const char* raw = std::getenv(name);
    if (raw == nullptr || *raw == '\0') {
        return {};
    }
    return utf8_to_wstring(raw);
}

std::vector<std::wstring> collect_mpv_candidates() {
    std::vector<std::wstring> candidates;

    const std::wstring env_path = env_to_wstring("SWITCHBOX_MPV_PATH");
    if (!env_path.empty()) {
        candidates.push_back(env_path);
    }

    const std::wstring exe_dir = desktop_exe_directory();
    if (!exe_dir.empty()) {
        candidates.push_back(exe_dir + L"\\mpv.exe");
    }

    candidates.push_back(L"mpv.exe");
    return candidates;
}

bool try_spawn_mpv(
    const std::wstring& mpv_path,
    const std::string& locator,
    std::string& attempted_locator,
    std::string& attempted_backend,
    std::string& error_message) {
    attempted_locator = locator;
    attempted_backend = wstring_to_utf8(mpv_path);

    const std::wstring wide_locator = utf8_to_wstring(locator);
    if (wide_locator.empty()) {
        error_message = "Unable to convert playback locator to UTF-16.";
        return false;
    }

    std::wstring command_line;
    command_line += quote_argument(mpv_path);
    command_line += L" --force-window=yes --keep-open=yes --hwdec=auto-safe ";
    command_line += quote_argument(wide_locator);

    STARTUPINFOW startup_info{};
    startup_info.cb = sizeof(startup_info);
    PROCESS_INFORMATION process_info{};

    std::vector<wchar_t> mutable_command(command_line.begin(), command_line.end());
    mutable_command.push_back(L'\0');

    const BOOL created = CreateProcessW(
        nullptr,
        mutable_command.data(),
        nullptr,
        nullptr,
        FALSE,
        CREATE_NEW_PROCESS_GROUP,
        nullptr,
        nullptr,
        &startup_info,
        &process_info);
    if (!created) {
        error_message = win32_error_message(GetLastError());
        return false;
    }

    CloseHandle(process_info.hThread);
    CloseHandle(process_info.hProcess);
    error_message.clear();
    return true;
}
#endif

}  // namespace

PlaybackLaunchResult launch_playback(const PlaybackTarget& target) {
    PlaybackLaunchResult result;

#if defined(_WIN32) && !defined(__SWITCH__)
    result.backend_available = true;

    if (target.primary_locator.empty() && target.fallback_locator.empty()) {
        result.error_message = "The selected target does not expose an openable locator yet.";
        return result;
    }

    const std::vector<std::wstring> mpv_candidates = collect_mpv_candidates();
    std::vector<std::string> errors;

    const auto attempt_with_locator = [&](const std::string& locator, bool is_fallback) -> bool {
        if (locator.empty()) {
            return false;
        }

        for (const auto& candidate : mpv_candidates) {
            std::string attempt_error;
            std::string attempt_backend;
            if (try_spawn_mpv(
                    candidate,
                    locator,
                    result.attempted_locator,
                    attempt_backend,
                    attempt_error)) {
                result.started = true;
                result.used_fallback_locator = is_fallback;
                return true;
            }

            if (!attempt_backend.empty()) {
                errors.push_back("[" + attempt_backend + "] " + attempt_error);
            } else {
                errors.push_back(attempt_error);
            }
        }
        return false;
    };

    if (attempt_with_locator(target.primary_locator, false)) {
        return result;
    }

    if (attempt_with_locator(target.fallback_locator, true)) {
        return result;
    }

    if (errors.empty()) {
        result.error_message = "Unable to launch mpv backend.";
        return result;
    }

    result.error_message = "Unable to launch mpv backend:";
    for (const auto& error : errors) {
        result.error_message += "\n- " + error;
    }
    return result;
#elif defined(__SWITCH__)
    result.backend_available = switch_mpv_backend_available();
    if (!result.backend_available) {
        result.error_message = switch_mpv_backend_reason();
        return result;
    }

    if (switch_mpv_open(target, result.error_message)) {
        result.started = true;
        result.attempted_locator =
            target.primary_locator.empty() ? target.fallback_locator : target.primary_locator;
        return result;
    }

    if (result.error_message.empty()) {
        result.error_message = "Unable to start switch playback backend.";
    }
    return result;
#else
    if (target.primary_locator.empty() && target.fallback_locator.empty()) {
        result.error_message = "The selected target does not expose an openable locator yet.";
        return result;
    }

    result.backend_available = false;
    result.error_message = "This build does not include a native playback backend yet.";
    return result;
#endif
}

}  // namespace switchbox::core
