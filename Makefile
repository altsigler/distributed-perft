#CC=gcc
#CC=clang
CC=icx
OPTFLAG=-O3
CFLAG=-std=c23 -Wall -Wextra -Werror -Wshadow -Wconversion

DEBUG=-g
#DEBUG=-g -O0
#DEBUG=-g -O0 -fsanitize=address -fsanitize=undefined
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
uname_s := $(shell uname -s)
ifeq ($(uname_s),Linux)
MARCH += -march=native
endif


all: brdutil.o movegen.o bytebrd.o onecore.o mcperft_api.o mcperft_internal.o
	ar rcs chlib.a mcperft_api.o mcperft_internal.o movegen.o brdutil.o bytebrd.o onecore.o
	$(CC) $(MARCH) $(OPTFLAG) $(CFLAG) $(DEBUG) -o perft perft.c chlib.a
	$(CC) $(MARCH) $(OPTFLAG) $(CFLAG) $(DEBUG) -o codecov codecov.c chlib.a
	$(CC) $(MARCH) $(OPTFLAG) $(CFLAG) $(DEBUG) -o mcperft mcperft.c chlib.a 
	$(CC) $(MARCH) $(OPTFLAG) $(CFLAG) $(DEBUG) -o scperft scperft.c chlib.a

mcperft_internal.o : mcperft_internal.c bytebrd_api.h mcperft_defs.h mcperft.h onecore_api.h brdutil_api.h
	$(CC) $(MARCH) $(OPTFLAG) $(CFLAG) $(DEBUG) -c mcperft_internal.c -o mcperft_internal.o 

mcperft_api.o : mcperft_api.c bytebrd_api.h mcperft_api.h mcperft_defs.h mcperft.h
	$(CC) $(MARCH) $(OPTFLAG) $(CFLAG) $(DEBUG) -c mcperft_api.c -o mcperft_api.o 

brdutil.o : brdutil.c brdutil_api.h
	$(CC) $(MARCH) $(OPTFLAG) $(CFLAG) $(DEBUG) -c brdutil.c

movegen.o: movegen.c movegen.h brdutil_api.h
	$(CC) $(MARCH) $(OPTFLAG) $(CFLAG) $(DEBUG) -c movegen.c

bytebrd.o: bytebrd.c movegen.h bytebrd_api.h
	$(CC) $(MARCH) $(OPTFLAG) $(CFLAG) $(DEBUG) -c bytebrd.c

onecore.o: onecore.c movegen.h onecore_api.h onecore.h bytebrd_api.h
	$(CC) $(MARCH) $(OPTFLAG) $(CFLAG) $(DEBUG) -c onecore.c

install:
	mkdir -p bin
	$(CC) $(MARCH) $(OPTFLAG) $(CFLAG) $(DEBUG) -static -o bin/perft perft.c chlib.a
	$(CC) $(MARCH) $(OPTFLAG) $(CFLAG) $(DEBUG) -static -o bin/mcperft mcperft.c chlib.a 
	$(CC) $(MARCH) $(OPTFLAG) $(CFLAG) $(DEBUG) -static -o bin/scperft scperft.c chlib.a
	strip -g bin/*

clean:
	rm -f *.o *.gcno *.gcda *.gcov chlib.a codecov mcperft perft scperft gmon.out
	rm -rf board-db
