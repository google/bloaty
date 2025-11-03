// Test file for inline function support in bloaty
// This source is the reference implementation for tests/macho/inlines.test
// The test uses YAML generated from binaries compiled from this source.

static inline int add(int a, int b) { return a + b; }

static inline int multiply(int a, int b) { return a * b; }

int external_call(int x) { return add(x, 5) + multiply(x, 2); }

int main(void) {
  int x = 10;
  int y = external_call(x);
  return y;
}
