DEBUG = -g
CFLAGS = $(DEBUG) -D__HTTP_ACCEPT_LOCK__ -lpthread
#CFLAGS = $(DEBUG)
OBJ = libhttp/mempool.o libhttp/http.o process.o worker.o main.o

all: $(OBJ)
	gcc -o httpd $(CFLAGS) $(OBJ) 

clean:
	rm httpd $(OBJ)
