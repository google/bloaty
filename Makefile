
.PHONY: clean

bloaty: bloaty.cc third_party/re2/obj/libre2.a
	g++ -g -std=c++11 -Wall -I third_party/re2 -O2 -o bloaty bloaty.cc third_party/re2/obj/libre2.a

third_party/re2/obj/libre2.a: third_party/re2/Makefile
	make -j8 -C third_party/re2

third_party/re2/Makefile: .gitmodules
	git submodule init && git submodule update

clean:
	rm -f bloaty
	cd third_party/re2 && make clean
	rm -rf *.dSYM

