#ifndef RUST_DEMANGLE_H_
#define RUST_DEMANGLE_H_

// Demangle "mangled".  On success, return true and write the
// demangled symbol name to "out".  Otherwise, return false.
// "out" is modified even if demangling is unsuccessful.
extern "C" {
bool RustDemangle(const char *mangled, char *out, int out_size);
}
#endif  // BASE_DEMANGLE_H_

