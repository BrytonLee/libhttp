DEBUG = -g
CFLAGS = $(DEBUG)
OBJ = libhttp/mempool.o libhttp/http.o

all: $(OBJ)
	gcc -o http $(CFLAGS) http_test.c $(OBJ) 

clean:
	rm http $(OBJ)
