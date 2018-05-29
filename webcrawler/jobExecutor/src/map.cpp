#include <iostream>
#include <cstring>
#include "../headers/map.h"


using namespace std;


/* Map implementation */
Map::Map(const int num_of_files) : current(-1), filecount(num_of_files), byteCount(0), wordCount(0), lineCount(0) {
    linecounts = new int[num_of_files];
    for (int i = 0 ; i < num_of_files ; i++) linecounts[i] = 0;   // initial value
    lines = new char**[num_of_files];
    for (int i = 0 ; i < num_of_files ; i++) lines[i] = NULL;     // initial value
    filepath = new char*[num_of_files];
    for (int i = 0 ; i < num_of_files ; i++) filepath[i] = NULL;  // initial value
}

Map::~Map() {
    for (int i = 0 ; i <= current ; i++){                         // delete as many files as you have already allocated
        for (int j = 0 ; j < linecounts[i] ; j++){
            if (lines[i][j] != NULL) delete[] lines[i][j];
        }
        delete[] lines[i];
        delete[] filepath[i];
    }
    delete[] linecounts;
    delete[] lines;
    delete[] filepath;
}

void Map::add_new_textfile(const char *fpath, const int num_of_lines) {
    ++current;
    filepath[current] = new char[strlen(fpath) + 1];
    strcpy(filepath[current], fpath);
    lines[current] = new char*[num_of_lines];
    for (int i = 0 ; i < num_of_lines ; i++) lines[current][i] = NULL;
    linecounts[current] = num_of_lines;
}

bool Map::insert_line_to_textfile(const int fileID, const int lineID, const char *line, const int line_length) {
    if ( fileID < 0 || fileID > current ) {
        cerr << "fileID " << fileID << " does not exist on the map" << endl;
        return false;
    } else if ( lineID < 0 || lineID >= linecounts[fileID] ){
        cerr << "lineID " << lineID << " does not exist on the map" << endl;
        return false;
    }
    if ( fileID != current ){
        cerr << "Warning: inserting a line on a textfile that is not the current one!" << endl;
    }
    lines[fileID][lineID] = new char[line_length];
    strcpy(lines[fileID][lineID], line);
    return true;
}

const char *Map::getPTRforline(const int fileID, const int lineID) const {
    if ( fileID < 0 || fileID > current ){
        cerr << "fileID does not exist on the map" << endl;
        return NULL;
    } else if ( lineID < 0 || lineID >= linecounts[fileID] ){
        cerr << "lineID does not exist on the map" << endl;
        return NULL;
    }
    return lines[fileID][lineID];
}

char *Map::getFilePath(const int fileID) const {
    if ( fileID < 0 || fileID > current ){
        return NULL;
    } else{
        return filepath[fileID];
    }
}

int Map::getFileCount() const {
    return filecount;
}

int Map::getFileLinesCount(int fileID) const {
    if (fileID < 0 || fileID > current ) return -1;
    return linecounts[fileID];
}
