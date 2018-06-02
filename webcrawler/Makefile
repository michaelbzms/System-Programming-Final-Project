JOBEXEC_DIR = "./jobExecutor"
OBJECTS = ./objects/webcrawler.o ./objects/crawl.o ./objects/crawling_monitoring.o ./objects/FIFO_Queue.o ./objects/str_history.o
SOURCE  = ./src/webcrawler.cpp ./src/crawl.cpp ./src/crawling_monitoring.cpp ./src/FIFO_Queue.cpp ./src/str_history.cpp
HEADERS = ./headers/crawl.h ./headers/crawling_monitoring.h ./headers/FIFO_Queue.h ./headers/str_history.h ./headers/executables_paths.h
OUT     = mycrawler
CC      = g++
FLAGS   = -g3


all: $(OBJECTS) JOBEXEC
	$(CC) -o $(OUT) $(OBJECTS) -pthread $(FLAGS)

./objects/webcrawler.o: ./src/webcrawler.cpp ./headers/executables_paths.h
	$(CC) -c ./src/webcrawler.cpp $(FLAGS)
	mv webcrawler.o ./objects/webcrawler.o

./objects/crawl.o: ./src/crawl.cpp ./headers/crawl.h
	$(CC) -c ./src/crawl.cpp $(FLAGS)
	mv crawl.o ./objects/crawl.o

./objects/crawling_monitoring.o: ./src/crawling_monitoring.cpp ./headers/crawling_monitoring.h ./headers/executables_paths.h
	$(CC) -c ./src/crawling_monitoring.cpp $(FLAGS)
	mv crawling_monitoring.o ./objects/crawling_monitoring.o

./objects/FIFO_Queue.o: ./src/FIFO_Queue.cpp ./headers/FIFO_Queue.h
	$(CC) -c ./src/FIFO_Queue.cpp $(FLAGS)
	mv FIFO_Queue.o ./objects/FIFO_Queue.o

./objects/str_history.o: ./src/str_history.cpp ./headers/str_history.h
	$(CC) -c ./src/str_history.cpp $(FLAGS)
	mv str_history.o ./objects/str_history.o

JOBEXEC:
	$(MAKE) -C $(JOBEXEC_DIR)         # compile jobExecutor

clean:
	rm -f $(OUT) $(OBJECTS)
	$(MAKE) clean -C $(JOBEXEC_DIR)   # make clean jobExecutor

wc:
	wc $(SOURCE) $(HEADERS)
	$(MAKE) wc -C $(JOBEXEC_DIR)
