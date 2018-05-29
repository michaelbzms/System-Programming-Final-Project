#include <cstring>
#include <cstdio>
#include "../headers/util.h"

/* This file implements any utility/multi-purpose   *
 * functions for the program                        */

void ignore_whitespace(std::istream &input_stream) {           // ignore all continuous whitespace from the given istream
    while (input_stream.peek() == ' ' || input_stream.peek() == '\t' ) {
        input_stream.ignore(1);
    }
}

char *get_current_time() {
    time_t now = time(0);
    char *date_and_time = ctime(&now);
    bool first = true;
    for (int i = 0 ; i < strlen(date_and_time) ; i++){         // remove ':' from the time format
        if ( date_and_time[i] == ':' ){
            if (first) { date_and_time[i] = '\''; first = false; }  // ' for minutes
            else date_and_time[i] = '\"';                           // " for seconds
        }
    }
    date_and_time[strlen(date_and_time) - 1] = '\0';           // remove '\n'
    return date_and_time;
}
