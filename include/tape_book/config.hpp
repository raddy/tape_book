#pragma once

#if defined(_MSC_VER)
#  define TB_ALWAYS_INLINE __forceinline
#else
#  define TB_ALWAYS_INLINE inline __attribute__((always_inline))
#endif

#if defined(__GNUC__) || defined(__clang__)
#  define TB_LIKELY(x)   __builtin_expect(!!(x), 1)
#  define TB_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#  define TB_LIKELY(x)   (x)
#  define TB_UNLIKELY(x) (x)
#endif
