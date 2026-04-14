/*
    Copyright 2020-2021 natinusala

    Licensed under the Apache License, Version 2.0 (the "License");
    you may not use this file except in compliance with the License.
    You may obtain a copy of the License at

        http://www.apache.org/licenses/LICENSE-2.0

    Unless required by applicable law or agreed to in writing, software
    distributed under the License is distributed on an "AS IS" BASIS,
    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
    See the License for the specific language governing permissions and
    limitations under the License.
*/

#include <borealis/core/application.hpp>
#include <borealis/core/assets.hpp>
#include <borealis/core/i18n.hpp>
#ifdef USE_BOOST_FILESYSTEM
#include <boost/filesystem.hpp>
namespace fs = boost::filesystem;
#elif __has_include(<filesystem>)
#include <filesystem>
namespace fs = std::filesystem;
#elif __has_include("experimental/filesystem")
#include <experimental/filesystem>
namespace fs = std::experimental::filesystem;
#else
#error "Failed to include <filesystem> header!"
#endif
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>

#ifndef BRLS_I18N_PREFIX
#define BRLS_I18N_PREFIX ""
#endif

namespace brls
{

static nlohmann::json defaultLocale = {};
static nlohmann::json currentLocale = {};
static std::vector<std::string> translationSearchPaths = {};

static nlohmann::json normalizeLocaleJson(const std::string& name, nlohmann::json strings)
{
    if (strings.is_object())
    {
        auto sameNameRoot = strings.find(name);
        if (sameNameRoot != strings.end())
        {
            return *sameNameRoot;
        }
    }

    return strings;
}

static void loadLocaleDirectory(const fs::path& localePath, std::string locale, nlohmann::json* target)
{
    std::error_code error;

    if (!fs::exists(localePath, error) || error)
    {
        return;
    }
    else if (!fs::is_directory(localePath, error) || error)
    {
        Logger::error("Cannot load locale {}: {} isn't a readable directory", locale, localePath.string());
        return;
    }

    fs::directory_iterator iterator(localePath, error);
    fs::directory_iterator end;

    if (error)
    {
        Logger::error("Cannot iterate locale {} directory {}: {}", locale, localePath.string(), error.message());
        return;
    }

    for (; iterator != end; iterator.increment(error))
    {
        if (error)
        {
            Logger::error("Error while iterating locale {} directory {}: {}", locale, localePath.string(), error.message());
            break;
        }

        const fs::directory_entry& entry = *iterator;

        if (fs::is_directory(entry.path(), error))
        {
            if (error)
            {
                error.clear();
            }
            continue;
        }

        std::string name = entry.path().filename().string();
        if (!endsWith(name, ".json"))
            continue;

        std::ifstream jsonStream(entry.path().string());
        nlohmann::json strings;

        try
        {
            jsonStream >> strings;
        }
        catch (const std::exception& e)
        {
            Logger::error("Error while loading \"{}\": {}", entry.path().string(), e.what());
            continue;
        }

        const std::string key = name.substr(0, name.length() - 5);
        (*target)[key]        = normalizeLocaleJson(key, std::move(strings));
    }
}

static void loadLocale(std::string locale, nlohmann::json* target)
{
    if (locale.empty())
        return;
#ifdef USE_LIBROMFS
    auto localePath = romfs::list("i18n/" + locale);
    if (localePath.empty())
    {
        Logger::error("Cannot load locale {}: directory i18n/{} doesn't exist", locale, locale);
        return;
    }
    for (auto& entry : localePath)
    {
        std::string path = entry.string();
        std::string name = entry.filename().string();
        if (!endsWith(name, ".json"))
            continue;

        const std::string key = name.substr(0, name.length() - 5);
        (*target)[key]        = normalizeLocaleJson(key, nlohmann::json::parse(romfs::get(path).string()));
    }
#else
    loadLocaleDirectory(BRLS_ASSET("i18n/" + locale), locale, target);
#endif /* USE_LIBROMFS */

    for (const std::string& basePath : translationSearchPaths)
    {
        loadLocaleDirectory(fs::path(basePath) / locale, locale, target);
    }
}

void loadTranslations()
{
    clearTranslations();
    loadLocale(LOCALE_DEFAULT, &defaultLocale);

    std::string currentLocaleName = Application::getLocale();
    if (currentLocaleName != LOCALE_DEFAULT)
        loadLocale(currentLocaleName, &currentLocale);
}

void reloadTranslations()
{
    loadTranslations();
}

void clearTranslations()
{
    defaultLocale = nlohmann::json::object();
    currentLocale = nlohmann::json::object();
}

void setTranslationSearchPaths(const std::vector<std::string>& searchPaths)
{
    translationSearchPaths.clear();
    translationSearchPaths.reserve(searchPaths.size());

    for (const std::string& searchPath : searchPaths)
    {
        if (!searchPath.empty())
            translationSearchPaths.push_back(searchPath);
    }
}

void addTranslationSearchPath(const std::string& searchPath)
{
    if (!searchPath.empty())
        translationSearchPaths.push_back(searchPath);
}

namespace internal
{
    std::string getRawStr(std::string stringName)
    {
        nlohmann::json::json_pointer pointer;

        try
        {
            pointer = nlohmann::json::json_pointer("/" + std::string(BRLS_I18N_PREFIX) + stringName);
        }
        catch (const std::exception& e)
        {
            Logger::error("Error while getting string \"{}\": {}", stringName, e.what());
            return stringName;
        }

        // First look for translated string in current locale
        try
        {
            return currentLocale[pointer].get<std::string>();
        }
        catch (...)
        {
        }

        // Then look for default locale
        try
        {
            return defaultLocale[pointer].get<std::string>();
        }
        catch (...)
        {
        }

        // Fallback to returning the string name
        return stringName;
    }
} // namespace internal

inline namespace literals
{
    std::string operator"" _i18n(const char* str, size_t len)
    {
        return internal::getRawStr(std::string(str, len));
    }

} // namespace literals

} // namespace brls
