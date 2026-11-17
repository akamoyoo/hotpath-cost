#!/usr/bin/env python3
"""Make variants of hffix.hpp:

  patch.py ORIG OUT inline   details::itoa and details::utoa marked always_inline (nothing else changed)
  patch.py ORIG OUT fast     itoa/utoa rewritten: digit count by comparison, digits written from the end,
                             two at a time, no reversal; itoa delegates to utoa
  patch.py ORIG OUT both     both changes
"""
import re, sys

orig, out, mode = sys.argv[1:4]
s = open(orig).read()

ITOA = re.compile(r"template<typename Int_type> char\* itoa\(Int_type number, char\* buffer, char\* end\)\n\{.*?\n    return b;\n\}\n", re.S)
UTOA = re.compile(r"template<typename Uint_type> char\* utoa\(Uint_type number, char\* buffer, char\* end\)\n\{.*?\n    return b;\n\}\n", re.S)
assert len(ITOA.findall(s)) == 1 and len(UTOA.findall(s)) == 1

ATTR = "inline __attribute__((always_inline)) " if mode in ("inline", "both") else ""

FAST = '''template<typename Uint_type> ATTRchar* utoa(Uint_type number, char* buffer, char* end)
{
    // Count the digits by comparison, then write them from the last one backwards, two at a time.
    static const char digits2[] =
        "00010203040506070809101112131415161718192021222324252627282930313233343536373839"
        "40414243444546474849505152535455565758596061626364656667686970717273747576777879"
        "8081828384858687888990919293949596979899";
    const int maxd = std::numeric_limits<Uint_type>::digits10 + 1;
    int n = 1;
    Uint_type p = 10;
    while (n < maxd && number >= p) { ++n; if (n < maxd) p *= 10; }
    if (end - buffer < n) details::throw_range_error();
    char* b = buffer + n;
    while (number >= 100) {
        unsigned r = (unsigned)(number % 100);
        number /= 100;
        b -= 2; b[0] = digits2[2 * r]; b[1] = digits2[2 * r + 1];
    }
    if (number >= 10) { b -= 2; b[0] = digits2[2 * number]; b[1] = digits2[2 * number + 1]; }
    else { *--b = (char)('0' + number); }
    return buffer + n;
}

template<typename Int_type> ATTRchar* itoa(Int_type number, char* buffer, char* end)
{
    typedef typename std::make_unsigned<Int_type>::type U;
    if (number < 0) {
        if (buffer >= end) details::throw_range_error();
        *buffer++ = '-';
        return utoa<U>(U(0) - U(number), buffer, end);
    }
    return utoa<U>(U(number), buffer, end);
}
'''.replace("ATTR", ATTR)

if mode in ("fast", "both"):
    s = UTOA.sub("", s)
    s = ITOA.sub(lambda m: FAST, s)
    s = s.replace("namespace hffix {", "#include <limits>\n#include <type_traits>\n\nnamespace hffix {", 1)
elif mode == "inline":
    s = ITOA.sub(lambda m: m.group(0).replace("template<typename Int_type> char* itoa", "template<typename Int_type> " + ATTR + "char* itoa"), s)
    s = UTOA.sub(lambda m: m.group(0).replace("template<typename Uint_type> char* utoa", "template<typename Uint_type> " + ATTR + "char* utoa"), s)
else:
    sys.exit("mode: inline | fast | both")
open(out, "w").write(s)
