#ifndef UNDERLINING_H
#define UNDERLINING_H

#define MINIMUM_TERM_WIDTH_ALLOWED 80       // this also means that the maximum word length should be <= than this or problems will occur at printing (!)
#define TABS_WIDTH 8


void print_text(const char *text, char **query, int query_size, int term_width);


#endif //UNDERLINING_H
