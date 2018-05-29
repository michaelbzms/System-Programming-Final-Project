OBJECTS = ./objects/webserver.o ./objects/serve_thread.o ./objects/ServeRequestBuffer.o
SOURCE  = ./src/webserver.cpp ./src/serve_thread.cpp ./src/ServeRequestBuffer.cpp
HEADERS = ./headers/webserver.h ./headers/serve_thread.h ./headers/ServeRequestBuffer.h
OUT     = myhttpd
CC      = g++
FLAGS   = -g3


all: $(OBJECTS)
	$(CC) -o $(OUT) $(OBJECTS) -pthread $(FLAGS)

./objects/webserver.o: ./src/webserver.cpp
	$(CC) -c ./src/webserver.cpp $(FLAGS)
	mv webserver.o ./objects/webserver.o

./objects/serve_thread.o: ./src/serve_thread.cpp ./headers/serve_thread.h
	$(CC) -c ./src/serve_thread.cpp $(FLAGS)
	mv serve_thread.o ./objects/serve_thread.o

./objects/ServeRequestBuffer.o: ./src/ServeRequestBuffer.cpp ./headers/ServeRequestBuffer.h
	$(CC) -c ./src/ServeRequestBuffer.cpp $(FLAGS)
	mv ServeRequestBuffer.o ./objects/ServeRequestBuffer.o

clean:
	rm -f $(OUT) $(OBJECTS)

wc:
	wc $(SOURCE) $(HEADER)
