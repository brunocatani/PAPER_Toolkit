#include "redux/NativeMemory.h"

#include <Windows.h>

#include <algorithm>
#include <cstdint>
#include <cstring>

namespace redux::native_memory
{
    namespace
    {
        [[nodiscard]] bool pageProtectionAllowsRead(const DWORD protection)
        {
            if ((protection & PAGE_GUARD) != 0 || (protection & PAGE_NOACCESS) != 0) {
                return false;
            }
            switch (protection & 0xFF) {
            case PAGE_READONLY:
            case PAGE_READWRITE:
            case PAGE_WRITECOPY:
            case PAGE_EXECUTE_READ:
            case PAGE_EXECUTE_READWRITE:
            case PAGE_EXECUTE_WRITECOPY:
                return true;
            default:
                return false;
            }
        }

        [[nodiscard]] bool pageProtectionAllowsWrite(const DWORD protection)
        {
            if ((protection & PAGE_GUARD) != 0 ||
                (protection & PAGE_NOACCESS) != 0) {
                return false;
            }
            switch (protection & 0xFF) {
            case PAGE_READWRITE:
            case PAGE_WRITECOPY:
            case PAGE_EXECUTE_READWRITE:
            case PAGE_EXECUTE_WRITECOPY:
                return true;
            default:
                return false;
            }
        }

        [[nodiscard]] bool rangeHasProtection(
            const void* pointer,
            const std::size_t byteCount,
            bool (*allowsProtection)(DWORD))
        {
            if (!pointer || byteCount == 0 || !pointerLooksReadable(pointer)) {
                return false;
            }
            const auto start = reinterpret_cast<std::uintptr_t>(pointer);
            const auto end = start + byteCount;
            if (end < start) {
                return false;
            }

            auto current = start;
            while (current < end) {
                MEMORY_BASIC_INFORMATION memoryInfo{};
                if (VirtualQuery(reinterpret_cast<LPCVOID>(current), &memoryInfo, sizeof(memoryInfo)) == 0 ||
                    memoryInfo.State != MEM_COMMIT || !allowsProtection(memoryInfo.Protect)) {
                    return false;
                }
                const auto regionBase = reinterpret_cast<std::uintptr_t>(memoryInfo.BaseAddress);
                const auto regionEnd = regionBase + memoryInfo.RegionSize;
                if (regionEnd <= current || regionEnd < regionBase) {
                    return false;
                }
                current = (std::min)(regionEnd, end);
            }
            return true;
        }
    }

    bool pointerLooksReadable(const void* pointer)
    {
        return reinterpret_cast<std::uintptr_t>(pointer) > 0x10000;
    }

    bool pointerRangeLooksReadable(const void* pointer, const std::size_t byteCount)
    {
        return rangeHasProtection(pointer, byteCount, pageProtectionAllowsRead);
    }

    bool pointerRangeLooksWritable(
        const void* pointer,
        const std::size_t byteCount)
    {
        return rangeHasProtection(pointer, byteCount, pageProtectionAllowsWrite);
    }

    bool guardedCopyFromMemory(const void* source, void* target, const std::size_t byteCount)
    {
        if (!source || !target || byteCount == 0 || !pointerRangeLooksReadable(source, byteCount)) {
            return false;
        }
#if defined(_MSC_VER)
        __try {
            std::memcpy(target, source, byteCount);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
#else
        std::memcpy(target, source, byteCount);
        return true;
#endif
    }
}
