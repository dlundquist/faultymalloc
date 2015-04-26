
malloc.so: malloc.o
	ld -shared -o malloc.so malloc.o -ldl

malloc.o: malloc.c
	gcc -Wall -std=gnu99 -fPIC -DPIC -g -c malloc.c
