#include <ctype.h>
#include <errno.h>
#include <fenv.h>
#include <fts.h>
#include <inttypes.h>
#include <locale.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <cwchar>

#include <cstdint>

#include "darwin_shim.h"
#include "engine.h"

namespace {

// With both register offsets at zero, va_arg takes everything from the stack, exactly where Darwin puts variadics.
struct AapcsVaList {
    void* stack;
    void* gr_top;
    void* vr_top;
    int gr_offs;
    int vr_offs;
};

va_list make_va_list(char* arguments) {
    static_assert(sizeof(AapcsVaList) == sizeof(va_list));
    const AapcsVaList raw{arguments, nullptr, nullptr, 0, 0};
    va_list list;
    memcpy(&list, &raw, sizeof raw);
    return list;
}

constexpr uint32_t kCtypeAlpha = 0x100, kCtypeControl = 0x200, kCtypeDigit = 0x400, kCtypeGraph = 0x800, kCtypeLower = 0x1000, kCtypePunct = 0x2000, kCtypeSpace = 0x4000, kCtypeUpper = 0x8000, kCtypeHex = 0x10000, kCtypeBlank = 0x20000, kCtypePrint = 0x40000;

uint32_t ctype_bits(int c) {
    if (c < 0 || c > 127) return 0;
    uint32_t bits = 0;
    if (isalpha(c)) bits |= kCtypeAlpha;
    if (iscntrl(c)) bits |= kCtypeControl;
    if (isdigit(c)) bits |= kCtypeDigit;
    if (isgraph(c)) bits |= kCtypeGraph;
    if (islower(c)) bits |= kCtypeLower;
    if (ispunct(c)) bits |= kCtypePunct;
    if (isspace(c)) bits |= kCtypeSpace;
    if (isupper(c)) bits |= kCtypeUpper;
    if (isxdigit(c)) bits |= kCtypeHex;
    if (isblank(c)) bits |= kCtypeBlank;
    if (isprint(c)) bits |= kCtypePrint;
    return bits;
}

// Darwin's isalpha and friends are inlined into the guest and read this table directly.
struct RuneLocale {
    char magic[8];
    char encoding[32];
    void* get_rune;
    void* put_rune;
    int32_t invalid;
    uint32_t type[256];
    int32_t lower[256];
    int32_t upper[256];
    char rest[3000];
};

RuneLocale* default_rune_locale() {
    static RuneLocale* locale = [] {
        auto* table = static_cast<RuneLocale*>(calloc(1, sizeof(RuneLocale)));
        memcpy(table->magic, "RuneMagA", 8);
        strcpy(table->encoding, "NONE");
        for (int c = 0; c < 256; ++c) {
            table->type[c] = ctype_bits(c);
            table->lower[c] = c < 128 ? tolower(c) : c;
            table->upper[c] = c < 128 ? toupper(c) : c;
        }
        return table;
    }();
    return locale;
}

int darwin_fpclassify_value(double value) {
    constexpr int kNan = 1, kInfinite = 2, kZero = 3, kNormal = 4, kSubnormal = 5;
    switch (fpclassify(value)) {
        case FP_NAN:
            return kNan;
        case FP_INFINITE:
            return kInfinite;
        case FP_ZERO:
            return kZero;
        case FP_SUBNORMAL:
            return kSubnormal;
        default:
            return kNormal;
    }
}

struct SinCos {
    double sine, cosine;
};
struct SinCosF {
    float sine, cosine;
};

}  // namespace

extern "C" {

int darwin_printf_impl(const char* format, char* arguments) { return vprintf(format, make_va_list(arguments)); }
int darwin_fprintf_impl(FILE* file, const char* format, char* arguments) { return vfprintf(file, format, make_va_list(arguments)); }
int darwin_sprintf_impl(char* out, const char* format, char* arguments) { return vsprintf(out, format, make_va_list(arguments)); }
int darwin_snprintf_impl(char* out, size_t size, const char* format, char* arguments) { return vsnprintf(out, size, format, make_va_list(arguments)); }
int darwin_sprintf_chk_impl(char* out, int, size_t, const char* format, char* arguments) { return vsprintf(out, format, make_va_list(arguments)); }
int darwin_snprintf_chk_impl(char* out, size_t size, int, size_t, const char* format, char* arguments) { return vsnprintf(out, size, format, make_va_list(arguments)); }
int darwin_sscanf_impl(const char* in, const char* format, char* arguments) { return vsscanf(in, format, make_va_list(arguments)); }
int darwin_fscanf_impl(FILE* file, const char* format, char* arguments) { return vfscanf(file, format, make_va_list(arguments)); }
int darwin_asprintf_impl(char** out, const char* format, char* arguments) { return vasprintf(out, format, make_va_list(arguments)); }

void darwin_printf();
void darwin_fprintf();
void darwin_sprintf();
void darwin_snprintf();
void darwin_sprintf_chk();
void darwin_snprintf_chk();
void darwin_sscanf();
void darwin_fscanf();
void darwin_asprintf();
}

// The named arguments stay in registers; the register after them receives the stack pointer.
#define DARWIN_VARIADIC(name, position) \
    ".globl " #name "\n" #name          \
    ":\n"                               \
    "  mov x" #position                 \
    ", sp\n"                            \
    "  b " #name "_impl\n"

__asm__(".text\n" DARWIN_VARIADIC(darwin_printf, 1) DARWIN_VARIADIC(darwin_fprintf, 2) DARWIN_VARIADIC(darwin_sprintf, 2) DARWIN_VARIADIC(darwin_snprintf, 3) DARWIN_VARIADIC(darwin_sprintf_chk, 4)
            DARWIN_VARIADIC(darwin_snprintf_chk, 5) DARWIN_VARIADIC(darwin_sscanf, 2) DARWIN_VARIADIC(darwin_fscanf, 2) DARWIN_VARIADIC(darwin_asprintf, 2));

namespace {

int v_printf(const char* format, char* arguments) { return vprintf(format, make_va_list(arguments)); }
int v_fprintf(FILE* file, const char* format, char* arguments) { return vfprintf(file, format, make_va_list(arguments)); }
int v_sprintf(char* out, const char* format, char* arguments) { return vsprintf(out, format, make_va_list(arguments)); }
int v_snprintf(char* out, size_t size, const char* format, char* arguments) { return vsnprintf(out, size, format, make_va_list(arguments)); }
int v_snprintf_chk(char* out, size_t size, int, size_t, const char* format, char* arguments) { return vsnprintf(out, size, format, make_va_list(arguments)); }
int v_asprintf(char** out, const char* format, char* arguments) { return vasprintf(out, format, make_va_list(arguments)); }

void* memcpy_chk(void* to, const void* from, size_t size, size_t) { return memcpy(to, from, size); }
void* memmove_chk(void* to, const void* from, size_t size, size_t) { return memmove(to, from, size); }
void* memset_chk(void* to, int value, size_t size, size_t) { return memset(to, value, size); }
char* strcpy_chk(char* to, const char* from, size_t) { return strcpy(to, from); }
char* strncpy_chk(char* to, const char* from, size_t size, size_t) { return strncpy(to, from, size); }
char* strcat_chk(char* to, const char* from, size_t) { return strcat(to, from); }
char* strncat_chk(char* to, const char* from, size_t size, size_t) { return strncat(to, from, size); }
size_t strlcpy_chk(char* to, const char* from, size_t size, size_t) { return strlcpy(to, from, size); }
size_t strlcat_chk(char* to, const char* from, size_t size, size_t) { return strlcat(to, from, size); }

void memset_pattern16_impl(void* to, const void* pattern, size_t size) {
    auto* out = static_cast<char*>(to);
    for (size_t i = 0; i < size; ++i) out[i] = static_cast<const char*>(pattern)[i % 16];
}

int timingsafe_bcmp_impl(const void* a, const void* b, size_t size) {
    unsigned char difference = 0;
    for (size_t i = 0; i < size; ++i) difference |= static_cast<const unsigned char*>(a)[i] ^ static_cast<const unsigned char*>(b)[i];
    return difference != 0;
}

// bionic's GNU flavor may return a static string rather than fill the buffer; Darwin always fills it.
int strerror_r_darwin(int value, char* out, size_t size) {
    const char* text = strerror_r(value, out, size);
    if (text != out) strlcpy(out, text, size);
    return 0;
}

[[noreturn]] void assert_rtn(const char* function, const char* file, int line, const char* expression) {
    engine_log("STOP: guest assertion failed: %s, in %s at %s:%d", expression, function, file, line);
    abort();
}

// qsort has no context argument, and a comparator may itself sort.
thread_local const void* t_sort_block = nullptr;

void qsort_b_impl(void* base, size_t count, size_t size, const void* block) {
    using Invoke = int (*)(const void*, const void*, const void*);
    constexpr size_t kBlockInvokeWord = 2;
    const void* saved = t_sort_block;
    t_sort_block = block;
    qsort(base, count, size, [](const void* a, const void* b) {
        return reinterpret_cast<Invoke>(static_cast<void* const*>(t_sort_block)[kBlockInvokeWord])(t_sort_block, a, b);
    });
    t_sort_block = saved;
}

SinCos sincos_stret(double value) { return {sin(value), cos(value)}; }
SinCosF sincosf_stret(float value) { return {sinf(value), cosf(value)}; }

FILE* g_stdin_pointer = nullptr;
FILE* g_stdout_pointer = nullptr;
FILE* g_stderr_pointer = nullptr;

}  // namespace

#define DARWIN_FN(name) {#name, reinterpret_cast<void*>(name)}
#define DARWIN_TYPED(name, type) {#name, reinterpret_cast<void*>(static_cast<type>(name))}

void add_darwin_libc_symbols(DarwinSymbolTable& table) {
    g_stdin_pointer = stdin;
    g_stdout_pointer = stdout;
    g_stderr_pointer = stderr;
    table.insert({
        {"printf", reinterpret_cast<void*>(darwin_printf)},
        {"fprintf", reinterpret_cast<void*>(darwin_fprintf)},
        {"sprintf", reinterpret_cast<void*>(darwin_sprintf)},
        {"snprintf", reinterpret_cast<void*>(darwin_snprintf)},
        {"__sprintf_chk", reinterpret_cast<void*>(darwin_sprintf_chk)},
        {"__snprintf_chk", reinterpret_cast<void*>(darwin_snprintf_chk)},
        {"sscanf", reinterpret_cast<void*>(darwin_sscanf)},
        {"fscanf", reinterpret_cast<void*>(darwin_fscanf)},
        {"asprintf", reinterpret_cast<void*>(darwin_asprintf)},
        {"vprintf", reinterpret_cast<void*>(v_printf)},
        {"vfprintf", reinterpret_cast<void*>(v_fprintf)},
        {"vsprintf", reinterpret_cast<void*>(v_sprintf)},
        {"vsnprintf", reinterpret_cast<void*>(v_snprintf)},
        {"__vsnprintf_chk", reinterpret_cast<void*>(v_snprintf_chk)},
        {"vasprintf", reinterpret_cast<void*>(v_asprintf)},
        {"__memcpy_chk", reinterpret_cast<void*>(memcpy_chk)},
        {"__memmove_chk", reinterpret_cast<void*>(memmove_chk)},
        {"__memset_chk", reinterpret_cast<void*>(memset_chk)},
        {"__strcpy_chk", reinterpret_cast<void*>(strcpy_chk)},
        {"__strncpy_chk", reinterpret_cast<void*>(strncpy_chk)},
        {"__strcat_chk", reinterpret_cast<void*>(strcat_chk)},
        {"__strncat_chk", reinterpret_cast<void*>(strncat_chk)},
        {"__strlcpy_chk", reinterpret_cast<void*>(strlcpy_chk)},
        {"__strlcat_chk", reinterpret_cast<void*>(strlcat_chk)},
        {"memset_pattern16", reinterpret_cast<void*>(memset_pattern16_impl)},
        {"timingsafe_bcmp", reinterpret_cast<void*>(timingsafe_bcmp_impl)},
        {"strerror_r", reinterpret_cast<void*>(strerror_r_darwin)},
        {"__assert_rtn", reinterpret_cast<void*>(assert_rtn)},
        {"qsort_b", reinterpret_cast<void*>(qsort_b_impl)},
        {"__sincos_stret", reinterpret_cast<void*>(sincos_stret)},
        {"__sincosf_stret", reinterpret_cast<void*>(sincosf_stret)},
        {"$s6Darwin6stdoutSpySo7__sFILEVGvg", reinterpret_cast<void*>(+[]() -> FILE* { return stdout; })},
        {"$s6Darwin6stderrSpySo7__sFILEVGvg", reinterpret_cast<void*>(+[]() -> FILE* { return stderr; })},
        {"__stdinp", &g_stdin_pointer},
        {"__stdoutp", &g_stdout_pointer},
        {"__stderrp", &g_stderr_pointer},
        {"_DefaultRuneLocale", default_rune_locale()},
        {"__maskrune", reinterpret_cast<void*>(+[](int c, unsigned long mask) -> int { return static_cast<int>(ctype_bits(c) & mask); })},
        {"__istype", reinterpret_cast<void*>(+[](int c, unsigned long mask) -> int { return (ctype_bits(c) & mask) != 0; })},
        {"__isctype", reinterpret_cast<void*>(+[](int c, unsigned long mask) -> int { return (ctype_bits(c) & mask) != 0; })},
        {"__tolower", reinterpret_cast<void*>(+[](int c) -> int { return tolower(c); })},
        {"__toupper", reinterpret_cast<void*>(+[](int c) -> int { return toupper(c); })},
        {"tolower", reinterpret_cast<void*>(+[](int c) -> int { return tolower(c); })},
        {"toupper", reinterpret_cast<void*>(+[](int c) -> int { return toupper(c); })},
        {"__fpclassifyd", reinterpret_cast<void*>(+[](double value) -> int { return darwin_fpclassify_value(value); })},
        {"__fpclassifyf", reinterpret_cast<void*>(+[](float value) -> int { return darwin_fpclassify_value(value); })},
        {"__isfinited", reinterpret_cast<void*>(+[](double value) -> int { return isfinite(value); })},
        {"__exp10", reinterpret_cast<void*>(+[](double value) -> double { return pow(10.0, value); })},
        {"__exp10f", reinterpret_cast<void*>(+[](float value) -> float { return powf(10.0f, value); })},
        {"fopen$DARWIN_EXTSN", reinterpret_cast<void*>(fopen)},
        {"realpath$DARWIN_EXTSN", reinterpret_cast<void*>(realpath)},
        {"atexit", reinterpret_cast<void*>(+[](void (*)()) -> int { return 0; })},
        DARWIN_TYPED(strlen, size_t (*)(const char*)),
        DARWIN_TYPED(strnlen, size_t (*)(const char*, size_t)),
        DARWIN_TYPED(strcmp, int (*)(const char*, const char*)),
        DARWIN_TYPED(strncmp, int (*)(const char*, const char*, size_t)),
        DARWIN_TYPED(strcasecmp, int (*)(const char*, const char*)),
        DARWIN_TYPED(strncasecmp, int (*)(const char*, const char*, size_t)),
        DARWIN_TYPED(strcpy, char* (*)(char*, const char*)),
        DARWIN_TYPED(strncpy, char* (*)(char*, const char*, size_t)),
        DARWIN_TYPED(stpncpy, char* (*)(char*, const char*, size_t)),
        DARWIN_TYPED(strcat, char* (*)(char*, const char*)),
        DARWIN_TYPED(strncat, char* (*)(char*, const char*, size_t)),
        DARWIN_TYPED(strlcpy, size_t (*)(char*, const char*, size_t)),
        DARWIN_TYPED(strlcat, size_t (*)(char*, const char*, size_t)),
        DARWIN_TYPED(strchr, const char* (*)(const char*, int)),
        DARWIN_TYPED(strrchr, const char* (*)(const char*, int)),
        DARWIN_TYPED(strstr, const char* (*)(const char*, const char*)),
        DARWIN_TYPED(strcasestr, const char* (*)(const char*, const char*)),
        DARWIN_TYPED(strpbrk, const char* (*)(const char*, const char*)),
        DARWIN_TYPED(strspn, size_t (*)(const char*, const char*)),
        DARWIN_TYPED(strcspn, size_t (*)(const char*, const char*)),
        DARWIN_TYPED(strdup, char* (*)(const char*)),
        DARWIN_TYPED(strndup, char* (*)(const char*, size_t)),
        DARWIN_TYPED(strtok, char* (*)(char*, const char*)),
        DARWIN_TYPED(strtok_r, char* (*)(char*, const char*, char**)),
        DARWIN_TYPED(strerror, char* (*)(int)),
        DARWIN_TYPED(memchr, const void* (*)(const void*, int, size_t)),
        DARWIN_TYPED(memcmp, int (*)(const void*, const void*, size_t)),
        DARWIN_TYPED(memset, void* (*)(void*, int, size_t)),
        DARWIN_TYPED(memmem, void* (*)(const void*, size_t, const void*, size_t)),
        DARWIN_FN(wcslen),
        DARWIN_FN(atoi),
        DARWIN_FN(atoll),
        DARWIN_FN(strtod),
        DARWIN_FN(strtol),
        DARWIN_FN(strtoll),
        DARWIN_FN(strtoul),
        DARWIN_FN(strtoull),
        DARWIN_FN(strtoumax),
        DARWIN_FN(bsearch),
        DARWIN_FN(qsort),
        DARWIN_TYPED(div, div_t (*)(int, int)),
        DARWIN_FN(rand),
        DARWIN_FN(srand),
        DARWIN_FN(rand_r),
        DARWIN_FN(random),
        DARWIN_FN(initstate),
        DARWIN_FN(setstate),
        DARWIN_FN(realloc),
        DARWIN_FN(posix_memalign),
        DARWIN_FN(arc4random_buf),
        DARWIN_FN(arc4random_uniform),
        DARWIN_FN(localeconv),
        DARWIN_FN(setenv),
        DARWIN_FN(unsetenv),
        DARWIN_FN(mkstemp),
        DARWIN_FN(remove),
        DARWIN_FN(rename),
        DARWIN_FN(_exit),
        DARWIN_FN(perror),
        DARWIN_FN(puts),
        DARWIN_FN(putchar),
        DARWIN_FN(getchar),
        DARWIN_FN(fdopen),
        DARWIN_FN(feof),
        DARWIN_FN(ferror),
        DARWIN_FN(fflush),
        DARWIN_FN(fgetc),
        DARWIN_FN(fgets),
        DARWIN_FN(fileno),
        DARWIN_FN(flockfile),
        DARWIN_FN(funlockfile),
        DARWIN_FN(fputc),
        DARWIN_FN(fputs),
        DARWIN_FN(fseeko),
        DARWIN_FN(ftello),
        DARWIN_FN(funopen),
        DARWIN_FN(fwrite),
        DARWIN_FN(setvbuf),
        DARWIN_FN(fts_open),
        DARWIN_FN(fts_read),
        DARWIN_FN(fts_close),
        DARWIN_FN(fts_set),
        DARWIN_FN(feclearexcept),
        DARWIN_FN(fetestexcept),
        DARWIN_TYPED(frexp, double (*)(double, int*)),
        DARWIN_TYPED(ldexp, double (*)(double, int)),
        DARWIN_TYPED(ldexpf, float (*)(float, int)),
        DARWIN_TYPED(llrint, long long (*)(double)),
        DARWIN_TYPED(llrintf, long long (*)(float)),
        DARWIN_TYPED(lrint, long (*)(double)),
        DARWIN_TYPED(lrintf, long (*)(float)),
        DARWIN_TYPED(lroundf, long (*)(float)),
        DARWIN_TYPED(modf, double (*)(double, double*)),
        DARWIN_TYPED(remainder, double (*)(double, double)),
        DARWIN_TYPED(scalbn, double (*)(double, int)),
    });
}
