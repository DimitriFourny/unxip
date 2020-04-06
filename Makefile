all:
	g++ -O3 unxip.cpp -o unxip -lz -lxml2 -I/usr/include/libxml2/

clean:
	rm unxip