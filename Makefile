
.PHONY: clean

# Disable -Wsign-compare because StringPiece currently unhelpfully defines
# size() as ssize_t instad of size_t.
CXXFLAGS=-std=c++11 -ffunction-sections -fdata-sections -Wall -Wno-sign-compare -g -I third_party/re2 -I. -Isrc -O2
RE2_H=third_party/re2/re2/re2.h

ifeq ($(shell uname), Darwin)
else
  GC_SECTIONS = -Wl,-gc-sections
endif

bloaty: src/main.o src/libbloaty.a third_party/re2/obj/libre2.a
	$(CXX) $(GC_SECTIONS) -o $@ $^ -lpthread

src/libbloaty.a: src/elf.o src/bloaty.o src/dwarf.o src/macho.o
	ar rcs $@ $^

src/bloaty.o: src/bloaty.cc src/bloaty.h $(RE2_H)
src/dwarf.o: src/dwarf.cc src/bloaty.h src/dwarf_constants.h $(RE2_H)
src/elf.o: src/elf.cc src/bloaty.h $(RE2_H)
src/macho.o: src/macho.cc src/bloaty.h $(RE2_H)
src/main.o: src/main.cc src/bloaty.h $(RE2_H)

third_party/re2/obj/libre2.a: third_party/re2/Makefile
	$(MAKE) -C third_party/re2 CPPFLAGS="-ffunction-sections -fdata-sections -g"

# These targets share a pattern match to coerce make into only executing once
# See this discussion: http://stackoverflow.com/a/3077254/1780018
third%party/re2/Makefile third%party/re2/re2/re2.h third%party/googletest/CMakeLists.txt: .gitmodules
	git submodule init && git submodule update
	@# Ensure .gitmodules cannot be newer
	touch .gitmodules -r $@

clean:
	rm -f bloaty src/*.o src/libbloaty.a
	rm -f tests/range_map_test tests/bloaty_test
	cd third_party/re2 && $(MAKE) clean
	rm -rf *.dSYM

## Tests #######################################################################

TESTFLAGS=-Ithird_party/googletest/googletest/include -Ithird_party/googletest/googlemock/include
TESTLIBS=third_party/googletest/googlemock/gtest/libgtest_main.a third_party/googletest/googlemock/gtest/libgtest.a

test: tests/range_map_test tests/bloaty_test
	TOP=`pwd`; \
	tests/range_map_test && \
	(cd tests/testdata/linux-x86_64 && $$TOP/tests/bloaty_test) && \
	(cd tests/testdata/linux-x86 && $$TOP/tests/bloaty_test)

tests/range_map_test: tests/range_map_test.cc src/libbloaty.a $(TESTLIBS) third_party/re2/obj/libre2.a
	$(CXX) $(CXXFLAGS) $(TESTFLAGS) -o $@ $^ -lpthread

tests/bloaty_test: tests/bloaty_test.cc src/libbloaty.a $(TESTLIBS) third_party/re2/obj/libre2.a
	$(CXX) $(CXXFLAGS) $(TESTFLAGS) -o $@ $^ -lpthread

third_party/googletest/googlemock/gtest/libgtest_main.a: third_party/googletest/CMakeLists.txt
	cd third_party/googletest && cmake . && $(MAKE)
