#ifndef PCSX_VITA_DISC_GROUPING_H
#define PCSX_VITA_DISC_GROUPING_H

#include <algorithm>
#include <stdint.h>
#include <string>
#include <vector>

namespace vita_disc_grouping {

struct Candidate {
    std::string filename;
    std::string title;
    uint64_t game_hash;
    std::string serial;
};

inline bool word_char(char ch)
{
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
        (ch >= '0' && ch <= '9') || (unsigned char)ch >= 0x80;
}

inline std::string normalized_title(const std::string &text)
{
    std::string out;
    bool separator = false;
    for (size_t i = 0; i < text.size(); ++i) {
        char ch = text[i];
        if (ch >= 'A' && ch <= 'Z')
            ch = (char)(ch + ('a' - 'A'));
        if (word_char(ch)) {
            if (separator && !out.empty())
                out.push_back(' ');
            out.push_back(ch);
            separator = false;
        } else {
            separator = true;
        }
    }
    return out;
}

inline int disc_number(const std::string &title, std::string *without_marker)
{
    std::string lower = title;
    for (size_t i = 0; i < lower.size(); ++i)
        if (lower[i] >= 'A' && lower[i] <= 'Z')
            lower[i] = (char)(lower[i] + ('a' - 'A'));

    static const char *markers[] = { "disc", "disk", "cd" };
    for (size_t i = 0; i < lower.size(); ++i) {
        if (i > 0 && word_char(lower[i - 1]))
            continue;
        for (unsigned m = 0; m < sizeof(markers) / sizeof(markers[0]); ++m) {
            const std::string marker(markers[m]);
            if (lower.compare(i, marker.size(), marker) != 0)
                continue;
            size_t j = i + marker.size();
            while (j < lower.size() && (lower[j] == ' ' || lower[j] == '-' ||
                                        lower[j] == '_'))
                ++j;
            if (j < lower.size() && lower[j] == '0')
                ++j;
            if (j >= lower.size() || lower[j] < '1' || lower[j] > '8' ||
                (j + 1 < lower.size() && word_char(lower[j + 1])))
                continue;

            size_t begin = i, end = j + 1;
            if (begin > 0 && end < title.size() &&
                ((title[begin - 1] == '(' && title[end] == ')') ||
                 (title[begin - 1] == '[' && title[end] == ']'))) {
                --begin;
                ++end;
            }
            if (without_marker)
                *without_marker = title.substr(0, begin) + title.substr(end);
            return lower[j] - '0';
        }
    }
    if (without_marker)
        *without_marker = title;
    return 0;
}

inline std::string playlist_stem(const std::string &title)
{
    std::string stem;
    bool pending_space = false;
    for (size_t i = 0; i < title.size() && stem.size() < 150; ++i) {
        const char ch = title[i];
        if (ch == '/' || ch == '\\' || ch == ':' || ch == '*' || ch == '?' ||
            ch == '"' || ch == '<' || ch == '>' || ch == '|' ||
            ch == '\r' || ch == '\n')
            continue;
        if (ch == ' ' || ch == '\t') {
            pending_space = true;
            continue;
        }
        if (pending_space && !stem.empty())
            stem.push_back(' ');
        pending_space = false;
        stem.push_back(ch);
    }
    while (!stem.empty() && (stem.back() == ' ' || stem.back() == '.' ||
                             stem.back() == '-'))
        stem.pop_back();
    return stem;
}

inline bool validate(const std::vector<Candidate> &discs,
                     std::vector<unsigned> *ordered,
                     std::string *playlist_name,
                     std::string *reason)
{
    if (discs.size() < 2 || discs.size() > 8) {
        *reason = "Select between 2 and 8 discs.";
        return false;
    }
    std::string first_title, first_key;
    uint64_t known_hash = 0;
    bool all_numbered = true;
    bool any_numbered = false;
    bool all_hashes_known = true;
    bool used_numbers[9] = {};
    std::vector<int> numbers(discs.size(), 0);

    for (size_t i = 0; i < discs.size(); ++i) {
        const Candidate &disc = discs[i];
        if (disc.filename.empty() || disc.filename[0] == '#' ||
            disc.filename.find_first_of("/\\\r\n") != std::string::npos) {
            *reason = "Invalid disc filename.";
            return false;
        }
        std::string stripped;
        const int number = disc_number(disc.title, &stripped);
        numbers[i] = number;
        all_numbered &= number != 0;
        any_numbered |= number != 0;
        all_hashes_known &= disc.game_hash != 0;
        const std::string key = normalized_title(stripped);
        if (key.empty()) {
            *reason = "Disc title is empty.";
            return false;
        }
        if (i == 0) {
            first_title = stripped;
            first_key = key;
        }
        if (known_hash && disc.game_hash && known_hash != disc.game_hash) {
            *reason = "Selected discs have different game IDs.";
            return false;
        }
        if (key != first_key && !(discs[0].game_hash && disc.game_hash &&
                                   discs[0].game_hash == disc.game_hash)) {
            *reason = "Selected discs are not the same game.";
            return false;
        }
        if (disc.game_hash)
            known_hash = disc.game_hash;
        if (number) {
            if (used_numbers[number]) {
                *reason = "Duplicate disc number.";
                return false;
            }
            used_numbers[number] = true;
        }
    }
    if (any_numbered && !all_numbered) {
        *reason = "Some discs have no disc number in their filename.";
        return false;
    }
    if (all_numbered && !used_numbers[1]) {
        *reason = "Select Disc 1 as well.";
        return false;
    }
    if (!all_numbered && !all_hashes_known) {
        *reason = "Cannot verify same game without disc numbers or IDs.";
        return false;
    }
    if (!all_numbered) {
        for (size_t i = 0; i < discs.size(); ++i) {
            for (size_t j = 0; j < i; ++j) {
                if (discs[i].serial.empty() || discs[i].serial == discs[j].serial) {
                    *reason = "Cannot distinguish the selected discs.";
                    return false;
                }
            }
        }
    }

    ordered->clear();
    for (unsigned i = 0; i < discs.size(); ++i)
        ordered->push_back(i);
    if (all_numbered)
        std::sort(ordered->begin(), ordered->end(), [&](unsigned a, unsigned b) {
            return numbers[a] < numbers[b];
        });
    if (all_numbered) {
        std::string title;
        disc_number(discs[(*ordered)[0]].title, &title);
        first_title = title;
    }

    const std::string stem = playlist_stem(first_title);
    if (stem.empty()) {
        *reason = "Cannot derive playlist filename.";
        return false;
    }
    *playlist_name = stem + ".m3u";
    return true;
}

} // namespace vita_disc_grouping

#endif
