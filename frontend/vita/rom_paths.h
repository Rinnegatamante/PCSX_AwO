#ifndef PCSX_VITA_ROM_PATHS_H
#define PCSX_VITA_ROM_PATHS_H

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "paths.h"

namespace vita_rom_paths {

enum { kMaxExtraFolders = 16, kMaxFolderLength = 240 };
static const char kConfigPath[] = PCSX_AWO_APP_ROOT "/rom_folders.txt";

inline std::string normalize(const std::string &input)
{
    const size_t first = input.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return std::string();
    const size_t last = input.find_last_not_of(" \t\r\n");
    const std::string text = input.substr(first, last - first + 1);
    if (text.size() > kMaxFolderLength)
        return std::string();

    const size_t colon = text.find(':');
    if (colon < 2 || colon > 12 || text.find(':', colon + 1) != std::string::npos)
        return std::string();
    std::string result;
    for (size_t i = 0; i < colon; ++i) {
        char ch = text[i];
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
              (ch >= '0' && ch <= '9') || ch == '_'))
            return std::string();
        if (ch >= 'A' && ch <= 'Z')
            ch = (char)(ch + ('a' - 'A'));
        result.push_back(ch);
    }
    result.push_back(':');

    size_t i = colon + 1;
    while (i < text.size()) {
        while (i < text.size() && (text[i] == '/' || text[i] == '\\'))
            ++i;
        const size_t start = i;
        while (i < text.size() && text[i] != '/' && text[i] != '\\') {
            const char ch = text[i];
            if ((unsigned char)ch < 32 || ch == ':' || ch == '"' ||
                ch == '*' || ch == '?' || ch == '<' || ch == '>' || ch == '|')
                return std::string();
            ++i;
        }
        const std::string part = text.substr(start, i - start);
        if (part == "." || part.empty())
            continue;
        if (part == "..")
            return std::string();
        if (result.back() != ':')
            result.push_back('/');
        result += part;
    }
    return result.size() <= kMaxFolderLength ? result : std::string();
}

inline std::string key(const std::string &path)
{
    std::string value = normalize(path);
    for (size_t i = 0; i < value.size(); ++i)
        if (value[i] >= 'A' && value[i] <= 'Z')
            value[i] = (char)(value[i] + ('a' - 'A'));
    return value;
}

inline std::string join(const std::string &folder, const std::string &name)
{
    return folder + (folder.empty() || folder.back() == ':' ? "" : "/") + name;
}

inline std::string parent(const std::string &folder)
{
    const std::string path = normalize(folder);
    const size_t slash = path.find_last_of('/');
    if (slash != std::string::npos)
        return path.substr(0, slash);
    const size_t colon = path.find(':');
    if (colon != std::string::npos && colon + 1 < path.size())
        return path.substr(0, colon + 1);
    return std::string();
}

inline bool add(std::vector<std::string> &folders, const std::string &path)
{
    const std::string value = normalize(path);
    const std::string normalized_key = key(value);
    if (normalized_key.empty() || normalized_key == key(PCSX_AWO_ROMS_DIR) ||
        folders.size() >= kMaxExtraFolders)
        return false;
    for (size_t i = 0; i < folders.size(); ++i)
        if (key(folders[i]) == normalized_key)
            return false;
    folders.push_back(value);
    return true;
}

inline bool remove(std::vector<std::string> &folders, size_t index)
{
    if (index >= folders.size())
        return false;
    folders.erase(folders.begin() + index);
    return true;
}

inline std::vector<std::string> load(const char *filename = kConfigPath)
{
    std::vector<std::string> result;
    FILE *file = fopen(filename, "rb");
    if (!file)
        return result;
    char line[512];
    while (fgets(line, sizeof(line), file)) {
        const size_t len = strlen(line);
        if (len == sizeof(line) - 1 && line[len - 1] != '\n') {
            int ch;
            while ((ch = fgetc(file)) != '\n' && ch != EOF) {}
            continue;
        }
        if (line[0] == '#')
            continue;
        add(result, line);
    }
    fclose(file);
    return result;
}

inline bool save(const std::vector<std::string> &folders,
                 const char *filename = kConfigPath)
{
    FILE *file = fopen(filename, "wb");
    if (!file)
        return false;
    bool ok = fputs("# PCSX AwO custom ROM folders\n", file) >= 0;
    for (size_t i = 0; i < folders.size(); ++i)
        ok = fputs((folders[i] + "\n").c_str(), file) >= 0 && ok;
    return fclose(file) == 0 && ok;
}

} // namespace vita_rom_paths

#endif
