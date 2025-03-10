#pragma once

#include <util/generic/serialized_enum.h>
#include <util/generic/vector.h>

namespace NUnifiedAgent {
    namespace NPrivate {
        using TEnumNames = TVector<const TString*>;

        template <typename TEnum>
        TEnumNames BuildEnumNames() {
            const auto names = GetEnumNames<TEnum>();
            auto result = TEnumNames(names.size());
            size_t index = 0;
            for (const auto& p: names) {
                Y_VERIFY(static_cast<size_t>(p.first) == index);
                result[index++] = &p.second;
            }
            return result;
        }

        template <typename TEnum>
        inline const auto EnumNames = BuildEnumNames<TEnum>();
    }

    template <typename TEnum, typename = std::enable_if_t<std::is_enum_v<TEnum>>>
    inline const TString& NameOf(TEnum val) noexcept {
        return *NPrivate::EnumNames<TEnum>[static_cast<size_t>(val)];
    }
}
