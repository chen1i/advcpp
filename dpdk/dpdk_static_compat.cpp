// Small runtime compatibility hooks for DPDK static archives.
//
// Some DPDK builds use libbsd for strlcpy().  The dynamic libdpdk package
// records that dependency, but a small static smoke-test binary should not
// require libbsd.so on the target host just for this symbol.

#include <cstddef>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <climits>
#include <cstdlib>

extern "C" int __isoc99_vsscanf(const char *str, const char *format,
                                std::va_list args);

extern "C" std::size_t strlcpy(char *dst, const char *src, std::size_t size) {
  const std::size_t src_len = std::strlen(src);

  if (size != 0) {
    const std::size_t copy_len = src_len < size ? src_len : size - 1;
    std::memcpy(dst, src, copy_len);
    dst[copy_len] = '\0';
  }

  return src_len;
}

static bool ascii_space(char c) {
  return c == ' ' || c == '\f' || c == '\n' || c == '\r' || c == '\t' ||
         c == '\v';
}

static int digit_value(char c) {
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'z')
    return c - 'a' + 10;
  if (c >= 'A' && c <= 'Z')
    return c - 'A' + 10;
  return -1;
}

static unsigned long long parse_unsigned_compat(const char *nptr, char **endptr,
                                                int base, bool *negative) {
  const char *p = nptr;
  while (ascii_space(*p))
    ++p;

  *negative = false;
  if (*p == '+' || *p == '-') {
    *negative = *p == '-';
    ++p;
  }

  if (base == 0) {
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
      base = 16;
      p += 2;
    } else if (p[0] == '0') {
      base = 8;
    } else {
      base = 10;
    }
  } else if (base == 16 && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
    p += 2;
  }

  const char *digits_begin = p;
  unsigned long long value = 0;
  bool overflow = false;
  for (;; ++p) {
    int digit = digit_value(*p);
    if (digit < 0 || digit >= base)
      break;

    if (value > (ULLONG_MAX - static_cast<unsigned>(digit)) /
                    static_cast<unsigned>(base)) {
      overflow = true;
      value = ULLONG_MAX;
    } else if (!overflow) {
      value = value * static_cast<unsigned>(base) +
              static_cast<unsigned>(digit);
    }
  }

  if (p == digits_begin) {
    if (endptr)
      *endptr = const_cast<char *>(nptr);
    return 0;
  }

  if (overflow)
    errno = ERANGE;
  if (endptr)
    *endptr = const_cast<char *>(p);
  return value;
}

extern "C" __attribute__((weak)) long
__isoc23_strtol(const char *nptr, char **endptr, int base) {
  bool negative = false;
  unsigned long long value = parse_unsigned_compat(nptr, endptr, base, &negative);
  if (negative) {
    if (value > static_cast<unsigned long long>(LONG_MAX) + 1ULL) {
      errno = ERANGE;
      return LONG_MIN;
    }
    if (value == static_cast<unsigned long long>(LONG_MAX) + 1ULL)
      return LONG_MIN;
    return -static_cast<long>(value);
  }
  if (value > static_cast<unsigned long long>(LONG_MAX)) {
    errno = ERANGE;
    return LONG_MAX;
  }
  return static_cast<long>(value);
}

extern "C" __attribute__((weak)) unsigned long
__isoc23_strtoul(const char *nptr, char **endptr, int base) {
  bool negative = false;
  unsigned long long value = parse_unsigned_compat(nptr, endptr, base, &negative);
  if (value > ULONG_MAX) {
    errno = ERANGE;
    value = ULONG_MAX;
  }
  unsigned long result = static_cast<unsigned long>(value);
  return negative ? static_cast<unsigned long>(-result) : result;
}

extern "C" __attribute__((weak)) unsigned long long
__isoc23_strtoull(const char *nptr, char **endptr, int base) {
  bool negative = false;
  unsigned long long value = parse_unsigned_compat(nptr, endptr, base, &negative);
  return negative ? ~value + 1ULL : value;
}

extern "C" __attribute__((weak)) int
__isoc23_vsscanf(const char *str, const char *format, std::va_list args) {
  return __isoc99_vsscanf(str, format, args);
}

extern "C" __attribute__((weak)) int
__isoc23_sscanf(const char *str, const char *format, ...) {
  std::va_list args;
  va_start(args, format);
  int ret = __isoc99_vsscanf(str, format, args);
  va_end(args);
  return ret;
}

struct bitmask {
  unsigned long size;
  unsigned long *maskp;
};

extern "C" int numa_available(void) { return 0; }

extern "C" struct bitmask *numa_allocate_nodemask(void) {
  constexpr unsigned long bits = 1024;
  constexpr std::size_t words = bits / (sizeof(unsigned long) * CHAR_BIT);

  auto *mask = static_cast<struct bitmask *>(
      std::calloc(1, sizeof(struct bitmask)));
  if (!mask)
    return nullptr;

  mask->maskp = static_cast<unsigned long *>(
      std::calloc(words, sizeof(unsigned long)));
  if (!mask->maskp) {
    std::free(mask);
    return nullptr;
  }

  mask->size = bits;
  mask->maskp[0] = 1UL;
  return mask;
}

extern "C" void numa_bitmask_free(struct bitmask *mask) {
  if (!mask)
    return;
  std::free(mask->maskp);
  std::free(mask);
}

extern "C" void numa_set_localalloc(void) {}

extern "C" void numa_set_preferred(int) {}

extern "C" long get_mempolicy(int *mode, unsigned long *nodemask,
                              unsigned long maxnode, void *, unsigned) {
  if (mode)
    *mode = 0;
  if (nodemask && maxnode > 0)
    nodemask[0] = 1UL;
  return 0;
}

extern "C" long set_mempolicy(int, const unsigned long *, unsigned long) {
  return 0;
}
