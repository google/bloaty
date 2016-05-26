
.PHONY: clean

CXXFLAGS=-std=c++11 -ffunction-sections -fdata-sections -Wall -g -I third_party/re2 -O2

ifeq ($(shell uname), Darwin)
  OBJFORMAT = macho.o
  GC_SECTIONS = -Wl,-dead_strip
else
  OBJFORMAT = elf.o
  GC_SECTIONS = -Wl,-gc-sections
endif

bloaty: bloaty.o $(OBJFORMAT) third_party/re2/obj/libre2.a
	g++ $(GC_SECTIONS) -o bloaty bloaty.o $(OBJFORMAT) third_party/re2/obj/libre2.a -lpthread

elf.o: elf.cc
bloaty.o: bloaty.cc

third_party/re2/obj/libre2.a: third_party/re2/Makefile
	make -j8 -C third_party/re2 CPPFLAGS="-ffunction-sections -fdata-sections -g"

third_party/re2/Makefile: .gitmodules
	git submodule init && git submodule update

clean:
	rm -f bloaty bloaty.o elf.o macho.o
	cd third_party/re2 && make clean
	rm -rf *.dSYM
