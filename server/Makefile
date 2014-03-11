SRC=$(wildcard *.c)
OBJS=$(SRC:.c=)
	
clean:
	rm -f $(OBJS)
	gcc -pthread -Wall -g $(SRC) -o $(OBJS) -lssl -lcrypto

