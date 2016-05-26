
.PHONY: clean

CXXFLAGS=-std=c++11 -ffunction-sections -fdata-sections -Wall -g -I third_party/re2 -O2

bloaty: bloaty.o elf.o third_party/re2/obj/libre2.a
	g++ -Wl,-gc-sections -static -o bloaty bloaty.o elf.o third_party/re2/obj/libre2.a -lpthread

elf.o: elf.cc
bloaty.o: bloaty.cc

third_party/re2/obj/libre2.a: third_party/re2/Makefile
	make -j8 -C third_party/re2 CPPFLAGS="-ffunction-sections -fdata-sections -g"

third_party/re2/Makefile: .gitmodules
	git submodule init && git submodule update

clean:
	rm -f bloaty
	cd third_party/re2 && make clean
	rm -rf *.dSYM

