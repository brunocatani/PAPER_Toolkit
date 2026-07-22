#pragma once

#include <cstddef>
#include <type_traits>

namespace redux::native_memory
{
    /*
     * FO4VR animation resources are opaque native objects whose pages can
     * disappear as graph/resource state changes. Every layout read crosses
     * this fail-closed boundary: page protection is checked first and SEH
     * protects the final copy from a concurrent stale-pointer fault.
     */
    [[nodiscard]] bool pointerLooksReadable(const void* pointer);
    [[nodiscard]] bool pointerRangeLooksReadable(const void* pointer, std::size_t byteCount);
    [[nodiscard]] bool guardedCopyFromMemory(const void* source, void* target, std::size_t byteCount);

    template <class T>
    [[nodiscard]] bool tryReadValue(const T* address, T& out)
    {
        static_assert(std::is_trivially_copyable_v<T>, "native reads require trivially copyable values");
        return guardedCopyFromMemory(address, &out, sizeof(T));
    }

    template <class T>
    [[nodiscard]] bool tryReadField(const void* base, std::ptrdiff_t offset, T& out)
    {
        if (!base) {
            return false;
        }
        const auto* address = reinterpret_cast<const T*>(
            reinterpret_cast<const std::byte*>(base) + offset);
        return tryReadValue(address, out);
    }
}
