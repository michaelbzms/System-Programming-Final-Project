#include <iostream>
#include <cstring>
#include "../headers/underlining.h"


using namespace std;


/* Local functions */
bool query_involves_word(char *word, char **query, int query_size);


void print_text(const char *text, char **query, int query_size, int term_width) {
    if (term_width < MINIMUM_TERM_WIDTH_ALLOWED) {
        term_width = MINIMUM_TERM_WIDTH_ALLOWED;
    }
    int size = (int) (strlen(text) + 1);
    char *text_copy = new char[size];       // a copy of text as strtok ruins its argument
    for (int i = 0 ; i < size ; i++){
        text_copy[i] = text[i];
    }
    char *word = strtok(text_copy, " \t");
    int k = 0;                             // indexes the character in word for which we want to print a ' ' or '^' next
    int I = 0;                             // indexes the character in text which is next to be printed
    while ( I < strlen(text) ){            // prints a document line and its corresponding "^^^" line on every loop until the entire text has been printed
        int word_count = 0;                // count how many words printed in each line (so that we know how many ' 's we have to print for the "^^^" line)
        int tabs_count = 0;                // count how many '\t's in this line
        // print document line, whilst also counting how many words are being printed
        // the following loop prints all next characters from text that make up words that can fit a terminal line
        int preI = I;
        for ( ; text[I] != '\0' && I < preI + term_width - tabs_count * (TABS_WIDTH - 1) ; I++) {
            cout << text[I];
            if ( text[I] == '\t' ) tabs_count++;
            // if the next character is the end of the file or a white space (and this char was neither) then we ve just finished printing a word:
            if ( text[I] != ' ' && text[I] != '\t' && (text[I+1] == ' ' || text[I+1] == '\t' || text[I+1] == '\0') ) {
                word_count++;
            }
                // else if the next character is the first letter of a word:
            else if ( ( text[I] == ' ' || text[I] == '\t' ) && text[I+1] != ' ' && text[I+1] != '\t' ) {
                // estimate if that word can fit in this terminal line
                bool word_does_not_fit = true;
                for (int i = I + 1; i < preI + term_width - tabs_count * (TABS_WIDTH - 1) ; i++) {
                    if (text[i] == '\0' || text[i] == ' ' || text[i] == '\t') {     // if there is a ' ','\t' or '\0' character to be printed before this terminal line "ends"
                        word_does_not_fit = false;                    // then the word before that character does fit in the terminal line
                        break;
                    }
                }
                if (word_does_not_fit) {         // else if there isn't, then this word would not fit
                    I++;
                    while (text[I] == ' ' || text[I] == '\t') {       // ignore white space
                        I++;
                        if ( text[I] == '\t' ) tabs_count++;
                    }
                    break;                       // so stop printing in this terminal line
                }
            }
        }
        cout << endl;
        // print "^^^" line, based on the word_count from the document line above
        for ( int i = 0 ; i < word_count && word != NULL ; i++ ){
            int word_size = strlen(word);                             // although actual word_size might differ from strlen(word) for words with non-ascii characters (!)
            if (query_involves_word(word, query, query_size)){        // print as many ' ' or '^' (depending on whether this word is involved in the query) as is the word for which we are printing them
                for (int j = 0 ; word[j] != '\0' ; j++) { cout << '^'; }
            } else {
                for (int j = 0 ; word[j] != '\0' ; j++) { cout << ' '; }
            }
            k += word_size;
            for (int j = k ; j < size && ( text[j] == ' ' || text[j] == '\t' ) ; j++) {    // print the same white space as in the text
                cout << text[j];
                k++;
            }
            word = strtok(NULL, " \t");         // move to the next word
        }
        cout << endl;
    }
    delete[] text_copy;
}


/* Local function Implementation */
bool query_involves_word(char *word, char **query, int query_size) {     // checks if a specific word is in a specific query
    for (int i = 0 ; i < query_size ; i++){
        if (strcmp(word, query[i]) == 0){
            return true;
        }
    }
    return false;
}

