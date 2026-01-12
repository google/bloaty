// Test file for dSYM generation tests
// Used to generate YAML test cases in tests/macho/dsym-*.test

int foo() { return 42; }

int main() {
  int result = foo();
  return result;
}
