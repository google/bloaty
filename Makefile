
.PHONY: clean

# Disable -Wsign-compare because StringPiece currently unhelpfully defines
# size() as ssize_t instad of size_t.
CXXFLAGS=-std=c++11 -ffunction-sections -fdata-sections -Wall -Wno-sign-compare -g -I third_party/re2 -I. -O2

ifeq ($(shell uname), Darwin)
else
  GC_SECTIONS = -Wl,-gc-sections
endif

bloaty: src/main.o src/bloaty.o src/macho.o src/elf.o src/dwarf.o third_party/re2/obj/libre2.a
	$(CXX) $(GC_SECTIONS) -o bloaty $^ -lpthread

src/main.o: src/main.cc src/bloaty.h
src/elf.o: src/elf.cc src/bloaty.h
src/bloaty.o: src/bloaty.cc src/bloaty.h
src/dwarf.o: src/dwarf.cc src/bloaty.h src/dwarf_constants.h

third_party/re2/obj/libre2.a: third_party/re2/Makefile
	make -j8 -C third_party/re2 CPPFLAGS="-ffunction-sections -fdata-sections -g"

third_party/re2/Makefile: .gitmodules
	git submodule init && git submodule update

clean:
	rm -f bloaty src/*.o
	cd third_party/re2 && make clean
	rm -rf *.dSYM
