#include "switchbox/core/update_check.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "switchbox/core/build_info.hpp"

#if __has_include(<libavformat/avio.h>) && __has_include(<libavformat/avformat.h>) && __has_include(<libavutil/dict.h>) && __has_include(<libavutil/error.h>)
#define SWITCHBOX_HAS_UPDATE_CHECK_FFMPEG_IO 1
extern "C" {
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
}
#else
#define SWITCHBOX_HAS_UPDATE_CHECK_FFMPEG_IO 0
#endif

namespace switchbox::core {

namespace {

using json = nlohmann::json;

constexpr std::string_view kReleaseApiUrl =
    "https://api.github.com/repos/WangBin-star/SwitchBOX/releases/latest";
constexpr std::string_view kTimeoutUs = "15000000";

bool is_cancelled(const std::shared_ptr<std::atomic_bool>& cancel_flag) {
    return cancel_flag != nullptr && cancel_flag->load();
}

std::string trim(std::string value) {
    const auto is_space = [](unsigned char character) {
        return std::isspace(character) != 0;
    };

    value.erase(value.begin(), std::find_if_not(value.begin(), value.end(), is_space));
    value.erase(std::find_if_not(value.rbegin(), value.rend(), is_space).base(), value.end());
    return value;
}

std::string sanitize_text(std::string value) {
    for (char& character : value) {
        if (character == '\r' || character == '\n' || character == '\t') {
            character = ' ';
        }
    }

    value.erase(
        std::remove_if(value.begin(), value.end(), [](unsigned char character) {
            return (character < 32 && character != ' ') || character == 127;
        }),
        value.end());
    return trim(std::move(value));
}

#if SWITCHBOX_HAS_UPDATE_CHECK_FFMPEG_IO

struct InterruptContext {
    const std::shared_ptr<std::atomic_bool>* cancel_flag = nullptr;
};

int interrupt_http_io(void* opaque) {
    auto* context = static_cast<InterruptContext*>(opaque);
    if (context == nullptr || context->cancel_flag == nullptr) {
        return 0;
    }

    return is_cancelled(*context->cancel_flag) ? 1 : 0;
}

std::string av_error_to_string(int error_code) {
    std::array<char, AV_ERROR_MAX_STRING_SIZE> buffer {};
    av_strerror(error_code, buffer.data(), buffer.size());
    return std::string(buffer.data());
}

std::string join_http_headers(const std::vector<std::string>& header_fields) {
    std::string joined;
    for (const auto& header : header_fields) {
        const std::string sanitized = sanitize_text(header);
        if (sanitized.empty()) {
            continue;
        }

        if (!joined.empty()) {
            joined += "\r\n";
        }
        joined += sanitized;
    }
    return joined;
}

void apply_http_options(AVDictionary** options) {
    const std::string user_agent = "SwitchBOX/" + BuildInfo::version_string();
    const std::vector<std::string> header_fields = {
        "Accept: application/vnd.github+json",
        "X-GitHub-Api-Version: 2022-11-28",
        "Connection: close",
    };

    av_dict_set(options, "user_agent", user_agent.c_str(), 0);
    av_dict_set(options, "timeout", std::string(kTimeoutUs).c_str(), 0);
    av_dict_set(options, "rw_timeout", std::string(kTimeoutUs).c_str(), 0);
    av_dict_set(options, "reconnect", "1", 0);
    av_dict_set(options, "reconnect_streamed", "1", 0);
    av_dict_set(options, "reconnect_on_network_error", "1", 0);
    av_dict_set(options, "reconnect_delay_max", "2", 0);
    av_dict_set(options, "multiple_requests", "0", 0);
    av_dict_set(options, "http_persistent", "0", 0);
    av_dict_set(options, "seekable", "0", 0);
    av_dict_set(options, "tls_verify", "0", 0);

    const std::string headers = join_http_headers(header_fields);
    if (!headers.empty()) {
        av_dict_set(options, "headers", headers.c_str(), 0);
    }
}

std::string fetch_text_via_ffmpeg(
    const std::string& url,
    const std::shared_ptr<std::atomic_bool>& cancel_flag,
    std::string& error_message) {
    error_message.clear();
    avformat_network_init();

    AVDictionary* options = nullptr;
    AVIOContext* io_context = nullptr;
    apply_http_options(&options);

    InterruptContext interrupt_context {&cancel_flag};
    AVIOInterruptCB interrupt_callback {
        .callback = interrupt_http_io,
        .opaque = &interrupt_context,
    };

    const int open_result =
        avio_open2(&io_context, url.c_str(), AVIO_FLAG_READ, &interrupt_callback, &options);
    av_dict_free(&options);
    if (open_result < 0) {
        if (is_cancelled(cancel_flag) || open_result == AVERROR_EXIT) {
            error_message = "Cancelled";
        } else {
            error_message = "Open release API failed: " + av_error_to_string(open_result);
        }
        return {};
    }

    std::string text;
    auto buffer = std::make_unique<std::vector<unsigned char>>(8 * 1024);
    while (true) {
        if (is_cancelled(cancel_flag)) {
            error_message = "Cancelled";
            avio_closep(&io_context);
            return {};
        }

        const int read_result =
            avio_read(io_context, buffer->data(), static_cast<int>(buffer->size()));
        if (read_result == AVERROR_EOF || read_result == 0) {
            break;
        }

        if (read_result < 0) {
            if (is_cancelled(cancel_flag) || read_result == AVERROR_EXIT) {
                error_message = "Cancelled";
            } else {
                error_message = "Read release API failed: " + av_error_to_string(read_result);
            }
            avio_closep(&io_context);
            return {};
        }

        text.append(reinterpret_cast<const char*>(buffer->data()), static_cast<size_t>(read_result));
    }

    avio_closep(&io_context);
    return text;
}

#endif

}  // namespace

UpdateCheckResult fetch_latest_release_version(const std::shared_ptr<std::atomic_bool>& cancel_flag) {
    UpdateCheckResult result;

#if SWITCHBOX_HAS_UPDATE_CHECK_FFMPEG_IO
    result.backend_available = true;

    std::string error_message;
    const std::string payload =
        fetch_text_via_ffmpeg(std::string(kReleaseApiUrl), cancel_flag, error_message);
    if (is_cancelled(cancel_flag) || error_message == "Cancelled") {
        result.error_message = "Cancelled";
        return result;
    }

    if (!error_message.empty()) {
        result.error_message = std::move(error_message);
        return result;
    }

    if (payload.empty()) {
        result.error_message = "Empty response from release API.";
        return result;
    }

    try {
        const json document = json::parse(payload);
        result.latest_version = sanitize_text(document.value("tag_name", std::string{}));
        if (result.latest_version.empty()) {
            result.latest_version = sanitize_text(document.value("name", std::string{}));
        }
        result.release_url = sanitize_text(document.value("html_url", std::string{}));

        if (result.latest_version.empty()) {
            const std::string api_message = sanitize_text(document.value("message", std::string{}));
            result.error_message = api_message.empty()
                ? "Latest release version is missing in API response."
                : api_message;
            return result;
        }

        result.success = true;
        return result;
    } catch (const std::exception& exception) {
        result.error_message = "Parse release API failed: " + sanitize_text(exception.what());
        return result;
    } catch (...) {
        result.error_message = "Parse release API failed: unknown exception.";
        return result;
    }
#else
    result.error_message = "Update check backend is not available in this build.";
    return result;
#endif
}

}  // namespace switchbox::core
