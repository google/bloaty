
.PHONY: clean

CXXFLAGS=-std=c++11 -ffunction-sections -fdata-sections -Wall -g -I third_party/re2 -I third_party/leveldb/include -O2

ifeq ($(shell uname), Darwin)
else
  EXTRA = dwarf.o
  EXTRA_LDFLAGS=-ldwarf -lelf
  GC_SECTIONS = -Wl,-gc-sections
endif

bloaty: bloaty.o macho.o elf.o $(EXTRA) third_party/re2/obj/libre2.a third_party/leveldb/out-static/libleveldb.a
	$(CXX) $(GC_SECTIONS) -o bloaty bloaty.o macho.o elf.o $(EXTRA) $(EXTRA_LDFLAGS) third_party/re2/obj/libre2.a third_party/leveldb/out-static/libleveldb.a -lpthread

elf.o: elf.cc bloaty.h
bloaty.o: bloaty.cc bloaty.h
dwarf.o: dwarf.cc bloaty.h

third_party/re2/obj/libre2.a: third_party/re2/Makefile
	make -j8 -C third_party/re2 CPPFLAGS="-ffunction-sections -fdata-sections -g"

third_party/leveldb/out-static/libleveldb.a: third_party/leveldb/Makefile
	make -j8 -C third_party/leveldb CPPFLAGS="-ffunction-sections -fdata-sections -g" out-static/libleveldb.a

third_party/re2/Makefile third_party/leveldb/Makefile: .gitmodules
	git submodule init && git submodule update

clean:
	rm -f bloaty bloaty.o elf.o macho.o
	cd third_party/re2 && make clean
	cd third_party/leveldb && make clean
	rm -rf *.dSYM
