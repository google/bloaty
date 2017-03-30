
.PHONY: clean deepclean

# Disable -Wsign-compare because StringPiece currently unhelpfully defines
# size() as ssize_t instad of size_t.
CXXFLAGS=-std=c++11 -W -Wall -Wno-sign-compare -g -I third_party/re2 -I. -Isrc
RE2_H=third_party/re2/re2/re2.h
RE2_A=third_party/re2/obj/libre2.a

bloaty: src/main.cc src/libbloaty.a $(RE2_A)
	$(CXX) $(GC_SECTIONS) $(CXXFLAGS) -O2 -o $@ $^ -lpthread

OBJS=src/bloaty.o src/dwarf.o src/elf.o src/macho.o

$(OBJS): %.o : %.cc src/bloaty.h src/dwarf_constants.h $(RE2_H)
	$(CXX) $(CXXFLAGS) -O2 -c -o $@ $<

src/libbloaty.a: $(OBJS)
	ar rcs $@ $^

third_party/re2/obj/libre2.a: third_party/re2/Makefile
	$(MAKE) -C third_party/re2 CPPFLAGS="-ffunction-sections -fdata-sections -g"

# These targets share a pattern match to coerce make into only executing once
# See this discussion: http://stackoverflow.com/a/3077254/1780018
third%party/re2/Makefile third%party/re2/re2/re2.h third%party/googletest/CMakeLists.txt third%party/libFuzzer/build.sh: .gitmodules
	git submodule init && git submodule update
	@# Ensure .gitmodules cannot be newer
	touch -r .gitmodules $@

clean:
	rm -f bloaty src/*.o src/*.a
	rm -f tests/range_map_test tests/bloaty_test tests/bloaty_misc_test
	rm -rf *.dSYM

deepclean: clean
	rm -rf third_party/re2 third_party/googletest third_party/libFuzzer

## Tests #######################################################################

TESTFLAGS=-Ithird_party/googletest/googletest/include -Ithird_party/googletest/googlemock/include
TESTLIBS=src/libbloaty-test.a $(RE2_A) third_party/googletest/googlemock/gtest/libgtest_main.a third_party/googletest/googlemock/gtest/libgtest.a

ifeq ($(CXX), clang++)
  TESTFLAGS += -fsanitize=address
endif

TESTOBJS=$(OBJS:src/%.o=src/%.test.o)

$(TESTOBJS): %.test.o : %.cc src/bloaty.h src/dwarf_constants.h $(RE2_H)
	$(CXX) $(CXXFLAGS) $(TESTFLAGS) -c -o $@ $<

src/libbloaty-test.a: $(TESTOBJS)
	ar rcs $@ $^

test: tests/range_map_test tests/bloaty_test tests/bloaty_misc_test
	TOP=`pwd`; \
	tests/range_map_test && \
	(cd tests/testdata/linux-x86_64 && $$TOP/tests/bloaty_test) && \
	(cd tests/testdata/linux-x86 && $$TOP/tests/bloaty_test) && \
	(cd tests/testdata/misc && $$TOP/tests/bloaty_misc_test)

tests/range_map_test: tests/range_map_test.cc $(TESTLIBS)
	$(CXX) $(CXXFLAGS) $(TESTFLAGS) -o $@ $^ -lpthread

tests/bloaty_test: tests/bloaty_test.cc $(TESTLIBS)
	$(CXX) $(CXXFLAGS) $(TESTFLAGS) -o $@ $^ -lpthread

tests/bloaty_misc_test: tests/bloaty_misc_test.cc $(TESTLIBS)
	$(CXX) $(CXXFLAGS) $(TESTFLAGS) -o $@ $^ -lpthread

third_party/googletest/googlemock/gtest/libgtest_main.a: third_party/googletest/CMakeLists.txt
	cd third_party/googletest && cmake . && $(MAKE)

## Fuzzing #####################################################################

FUZZFLAGS=-fsanitize=address -fsanitize-coverage=trace-pc-guard
TESTCMD=-fsanitize-coverage=trace-pc-guard -c -x c++ /dev/null -o /dev/null 2> /dev/null
TESTRESULT=$(shell $(CXX) $(TESTCMD) && echo ok)
ifeq ($(TESTRESULT), ok)
fuzz: tests/fuzz_target
else
fuzz:
	echo "Fuzzing requires that CXX is a very recent Clang (ie from svn"
	false
endif

LIBFUZZER=third_party/libFuzzer/libFuzzer.a
FUZZLIBS=$(BLOATYLIBS) $(LIBFUZZER)

FUZZOBJS=$(OBJS:src/%.o=src/%.fuzz.o)

$(FUZZOBJS): %.fuzz.o : %.cc src/bloaty.h src/dwarf_constants.h $(RE2_H)
	$(CXX) $(CXXFLAGS) $(FUZZFLAGS) -c -o $@ $<

src/libbloaty-fuzz.a: $(FUZZOBJS)
	ar rcs $@ $^

$(LIBFUZZER): third_party/libFuzzer/build.sh
	cd third_party/libFuzzer && ./build.sh

tests/fuzz_target: tests/fuzz_target.cc src/libbloaty-fuzz.a $(LIBFUZZER) $(RE2_A)
	$(CXX) $(CXXFLAGS) $(FUZZFLAGS) -o $@ $^ -lpthread
