#pragma once

#include "macfsr_vulkan_shim.h"

#if !defined(_MSC_VER)
#include <cerrno>
#include <cstdlib>
#include <cwchar>

#ifndef _countof
#define _countof(array) (sizeof(array) / sizeof((array)[0]))
#endif

template <size_t Size>
inline int wcscpy_s(wchar_t (&destination)[Size], const wchar_t* source) {
    if (source == nullptr) {
        destination[0] = L'\0';
        return EINVAL;
    }
    const size_t length = std::wcslen(source);
    if (length >= Size) {
        destination[0] = L'\0';
        return ERANGE;
    }
    std::wmemcpy(destination, source, length + 1);
    return 0;
}

inline int wcstombs_s(
    size_t* converted,
    char* destination,
    size_t destinationSize,
    const wchar_t* source,
    size_t count
) {
    if (destination == nullptr || destinationSize == 0 || source == nullptr) {
        return EINVAL;
    }
    size_t result = std::wcstombs(destination, source, count < destinationSize ? count : destinationSize - 1);
    if (result == static_cast<size_t>(-1)) {
        destination[0] = '\0';
        return EILSEQ;
    }
    destination[result] = '\0';
    if (converted != nullptr) {
        *converted = result + 1;
    }
    return 0;
}
#endif
