OBJS1  = ./objects/jobExecutor.o ./objects/underlining.o
OBJS2  = ./objects/worker.o ./objects/textfiles_parsing.o ./objects/inverted_index.o ./objects/map.o
COMMON = ./objects/util.o
SOURCE = ./src/jobExecutor.cpp ./src/worker.cpp ./src/textfiles_parsing.cpp ./src/inverted_index.cpp ./src/map.cpp ./src/util.cpp ./src/underlining.cpp
HEADER = ./headers/textfiles_parsing.h ./headers/inverted_index.h ./headers/map.h ./headers/util.h ./headers/underlining.h
OUT1   = ./bin/jobExecutor
OUT2   = ./bin/worker
CC     = g++
FLAGS  = -g3

all: $(OBJS1) $(OBJS2) $(COMMON)
	$(CC) $(OBJS1) $(COMMON) -o $(OUT1) $(FLAGS)   # jobExecutor executable
	$(CC) $(OBJS2) $(COMMON) -o $(OUT2) $(FLAGS)   # worker executable

./objects/jobExecutor.o: ./src/jobExecutor.cpp
	$(CC) -c ./src/jobExecutor.cpp $(FLAGS)
	mv ./jobExecutor.o ./objects/jobExecutor.o

./objects/underlining.o: ./src/underlining.cpp ./headers/underlining.h
	$(CC) -c ./src/underlining.cpp $(FLAGS)
	mv ./underlining.o ./objects/underlining.o

./objects/worker.o: ./src/worker.cpp
	$(CC) -c ./src/worker.cpp $(FLAGS)
	mv ./worker.o ./objects/worker.o

./objects/textfiles_parsing.o: ./src/textfiles_parsing.cpp ./headers/textfiles_parsing.h
	$(CC) -c ./src/textfiles_parsing.cpp $(FLAGS)
	mv ./textfiles_parsing.o ./objects/textfiles_parsing.o

./objects/inverted_index.o: ./src/inverted_index.cpp ./headers/inverted_index.h
	$(CC) -c ./src/inverted_index.cpp $(FLAGS)
	mv ./inverted_index.o ./objects/inverted_index.o

./objects/map.o: ./src/map.cpp ./headers/map.h
	$(CC) -c ./src/map.cpp $(FLAGS)
	mv ./map.o ./objects/map.o

./objects/util.o: ./src/util.cpp ./headers/util.h
	$(CC) -c ./src/util.cpp $(FLAGS)
	mv ./util.o ./objects/util.o

clearlogs:
	rm -f log/*

clean:
	rm -f $(OBJS1) $(OBJS2) $(COMMON) $(OUT1) $(OUT2)
	rm -f log/*           # also remove all worker logs

wc:
	wc $(SOURCE) $(HEADER)
