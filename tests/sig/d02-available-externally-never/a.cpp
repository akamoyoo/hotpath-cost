// `never` whose closure runs through an available_externally body: the code that runs is libstdc++.so's own copy
// (compiled separately), the IR only holds the header's inline copy. Which evidence does the table give?
#include <string>
#include <string_view>
#include <cstdio>
#define NI __attribute__((noinline))
NI long f_find(const std::string& s) { return (long)s.find('x', 1); }
NI long f_rfind(const std::string& s) { return (long)s.rfind("ab", 7, 2); }
NI long f_compare(const std::string& a, const std::string& b) { return a.compare(1, 3, b); }
NI long f_ffo(const std::string& s) { return (long)s.find_first_not_of("abc", 0); }
NI long f_copy(const std::string& s, char* d) { return (long)s.copy(d, 3, 0); }
int main(int argc, char** argv) { std::string s(argv[0]); char d[8]; long r = f_find(s) + f_rfind(s) + f_compare(s, s) + f_ffo(s) + f_copy(s, d); std::printf("%ld\n", r); }
