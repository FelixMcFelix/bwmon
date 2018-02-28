CFLAGS=-Wall -Werror -lpcap -lpthread -lsupc++ -std=c++17 -g

bwmon: bwmon.cc
	g++ bwmon.cc -o bwmon $(CFLAGS)

clean:
	rm -f bwmon *.o