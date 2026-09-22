#CC=gcc
CC=clang
#CC=icx
OPTFLAG=-O3
CFLAG=-std=c23 -Wall -Wextra -Werror -Wshadow -Wconversion

DEBUG=-g
#DEBUG=-g -Og
#DEBUG=-g -Og -fsanitize=address -fsanitize=undefined
#DEBUG= -fanalyzer
#DEBUG=-g -O0 --coverage

#
# Enable static analysis. Must be used with gcc-10 or newer compilers.
#DEBUG= -fanalyzer
#
# Enable this flag for code coverage. For example: gcov mcperft_internal.c
#DEBUG=-O0 --coverage
#DEBUG= --coverage
#
# Enable run-time address sanitizer.
#DEBUG= -fsanitize=address
#
# Enable run-time sanitizer for unpredictable behavior.
#DEBUG= -fsanitize=undefined
#
# Enable debugging with gdb
#DEBUG= -g -O0
#DEBUG= -g -Og

MARCH=


all: MARCH += -march=native
all: brdutil.o movegen.o bytebrd.o onecore.o mcperft_api.o mcperft_internal.o
	ar rcs chlib.a mcperft_api.o mcperft_internal.o movegen.o brdutil.o bytebrd.o onecore.o
	$(CC) $(MARCH) $(OPTFLAG) $(CFLAG) $(DEBUG) -o perft perft.c chlib.a
	$(CC) $(MARCH) $(OPTFLAG) $(CFLAG) $(DEBUG) -o codecov codecov.c chlib.a
	$(CC) $(MARCH) $(OPTFLAG) $(CFLAG) $(DEBUG) -o mcperft mcperft.c chlib.a 
	$(CC) $(MARCH) $(OPTFLAG) $(CFLAG) $(DEBUG) -o scperft scperft.c chlib.a

gen_build: MARCH += -mbmi -mbmi2 -mavx -mavx2 -msse4
gen_build: clean 
	mkdir -p bin
	$(CC) $(MARCH) $(OPTFLAG) $(CFLAG) $(DEBUG) -c onecore.c
	$(CC) $(MARCH) $(OPTFLAG) $(CFLAG) $(DEBUG) -c bytebrd.c
	$(CC) $(MARCH) $(OPTFLAG) $(CFLAG) $(DEBUG) -c movegen.c
	$(CC) $(MARCH) $(OPTFLAG) $(CFLAG) $(DEBUG) -c brdutil.c
	$(CC) $(MARCH) $(OPTFLAG) $(CFLAG) $(DEBUG) -c mcperft_api.c -o mcperft_api.o 
	$(CC) $(MARCH) $(OPTFLAG) $(CFLAG) $(DEBUG) -c mcperft_internal.c -o mcperft_internal.o 
	ar rcs chlib.a mcperft_api.o mcperft_internal.o movegen.o brdutil.o bytebrd.o onecore.o
	$(CC) $(MARCH) $(OPTFLAG) $(CFLAG) $(DEBUG) -static -o bin/perft-gen perft.c chlib.a
	$(CC) $(MARCH) $(OPTFLAG) $(CFLAG) $(DEBUG) -static -o bin/mcperft-gen mcperft.c chlib.a 
	$(CC) $(MARCH) $(OPTFLAG) $(CFLAG) $(DEBUG) -static -o bin/scperft-gen scperft.c chlib.a

zen4_build: MARCH += -march=znver4
zen4_build: clean2 
	mkdir -p bin
	$(CC) $(MARCH) $(OPTFLAG) $(CFLAG) $(DEBUG) -c onecore.c
	$(CC) $(MARCH) $(OPTFLAG) $(CFLAG) $(DEBUG) -c bytebrd.c
	$(CC) $(MARCH) $(OPTFLAG) $(CFLAG) $(DEBUG) -c movegen.c
	$(CC) $(MARCH) $(OPTFLAG) $(CFLAG) $(DEBUG) -c brdutil.c
	$(CC) $(MARCH) $(OPTFLAG) $(CFLAG) $(DEBUG) -c mcperft_api.c -o mcperft_api.o 
	$(CC) $(MARCH) $(OPTFLAG) $(CFLAG) $(DEBUG) -c mcperft_internal.c -o mcperft_internal.o 
	ar rcs chlib.a mcperft_api.o mcperft_internal.o movegen.o brdutil.o bytebrd.o onecore.o
	$(CC) $(MARCH) $(OPTFLAG) $(CFLAG) $(DEBUG) -static -o bin/perft-zen4 perft.c chlib.a
	$(CC) $(MARCH) $(OPTFLAG) $(CFLAG) $(DEBUG) -static -o bin/mcperft-zen4 mcperft.c chlib.a 
	$(CC) $(MARCH) $(OPTFLAG) $(CFLAG) $(DEBUG) -static -o bin/scperft-zen4 scperft.c chlib.a

mcperft_internal.o : mcperft_internal.c bytebrd_api.h mcperft_defs.h onecore_api.h brdutil_api.h
	$(CC) $(MARCH) $(OPTFLAG) $(CFLAG) $(DEBUG) -c mcperft_internal.c -o mcperft_internal.o 

mcperft_api.o : mcperft_api.c bytebrd_api.h mcperft_api.h mcperft_defs.h
	$(CC) $(MARCH) $(OPTFLAG) $(CFLAG) $(DEBUG) -c mcperft_api.c -o mcperft_api.o 

brdutil.o : brdutil.c brdutil_api.h
	$(CC) $(MARCH) $(OPTFLAG) $(CFLAG) $(DEBUG) -c brdutil.c

movegen.o: movegen.c movegen.h brdutil_api.h
	$(CC) $(MARCH) $(OPTFLAG) $(CFLAG) $(DEBUG) -c movegen.c

bytebrd.o: bytebrd.c movegen.h bytebrd_api.h
	$(CC) $(MARCH) $(OPTFLAG) $(CFLAG) $(DEBUG) -c bytebrd.c

onecore.o: onecore.c movegen.h onecore_api.h onecore.h bytebrd_api.h
	$(CC) $(MARCH) $(OPTFLAG) $(CFLAG) $(DEBUG) -c onecore.c

install: gen_build zen4_build 
	strip -g bin/*

clean:
	rm -f *.o *.gcno *.gcda *.gcov chlib.a codecov mcperft perft scperft gmon.out
	rm -rf board-db

clean2:
	rm -f *.o *.gcno *.gcda *.gcov chlib.a codecov mcperft perft scperft gmon.out
	rm -rf board-db

.PHONY: clean clean2 install zen4_build gen_build
