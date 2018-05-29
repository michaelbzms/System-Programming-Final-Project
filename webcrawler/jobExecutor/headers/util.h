#ifndef UTIL_H
#define UTIL_H

#include <istream>


/* Utility data structs */
struct intnode{
    int val;
    intnode *next;
};

/* Pipe communication protocol */
enum Type {
    NOT_USED = 0,              // type field is not used
    ERROR = 1,                 // used to signal an ERROR happened
    END_OF_MESSAGES = 2,       // expect no more messages after this header (message_size should be 0)
    EXIT = 3,                  // this header notifies the worker to stop accepting request and to terminate
    SEARCH = 4,                // the message(s) after this header is (are) a request / answer(s) for a /search command
    MAXCOUNT = 5,              // the message after this header is a request / answer for a /maxcount command
    MINCOUNT = 6,              // the message after this header is a request / answer for a /mincount command
    WORDCOUNT = 7,             // the message after this header is a request / answer for a /wc command
    INIT = 8,                  // a header used when trying to initialize the worker by passing him its directories
};

struct Header{
    Type type;
    unsigned int message_size;
    Header() : type(NOT_USED), message_size(0) {}   // default constructor
    Header(Type tp, unsigned int msg_size) : type(tp), message_size(msg_size) {}
};

/* Utility functions */
void ignore_whitespace(std::istream &input_stream);
char *get_current_time();

#endif //UTIL_H
