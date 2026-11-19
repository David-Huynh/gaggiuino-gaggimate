#include "semver_extensions.h"

#include <cerrno>
#include <climits>
#include <cstdlib>

bool parseFirmwareVersion(const char *text, semver_t &version) {
    if (*text == 'v')
        ++text;

    // semver_parse also accepts short versions and unchecked integers.
    const char *component = text;
    for (int i = 0; i < 3; ++i) {
        if (*component < '0' || *component > '9')
            return false;
        char *end = nullptr;
        errno = 0;
        const long value = std::strtol(component, &end, 10);
        if (errno || value > INT_MAX)
            return false;
        if (i == 2) {
            if (*end && ((*end != '-' && *end != '+') || !end[1]))
                return false;
        } else {
            if (*end != '.')
                return false;
            component = end + 1;
        }
    }

    semver_t parsed{};
    if (semver_parse(text, &parsed) != 0) {
        semver_free(&parsed);
        return false;
    }
    semver_free(&version);
    version = parsed;
    return true;
}
