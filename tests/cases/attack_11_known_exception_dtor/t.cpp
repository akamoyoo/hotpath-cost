// The hot-path `known` list declares the destructors of the std exception classes (D0, D1, D2 of
// runtime_error, logic_error, ...) to be without class. ~runtime_error releases its message, a
// reference-counted string in libstdc++.so: when the last reference goes, it calls operator delete.
// Here an error kept in a std::optional is cleared by the entry (Clang calls D1 directly).
#include <optional>
#include <stdexcept>
#include <string>
#include <cstdio>
std::optional<std::runtime_error> last_error;
__attribute__((noinline)) void entry() { last_error.reset(); }
int main(int argc, char**) {
    last_error.emplace(std::string(argc * 100, 'x'));
    entry();
    std::printf("%d\n", (int)last_error.has_value());
}
