#ifndef MAP_H
#define MAP_H


class Map{
    char **filepath;  // where filepath[i] is the filepath (path + name) for the i-th text file for this worker
    char ***lines;    // where lines[i][j] is the j-th line in the i-th text file for this worker
    int *linecounts;  // where linecounts[i] is how many lines the i-th text file has
    int filecount;    // total number of text files (possibly across different directories) for this worker
    int current;      // index of the currently last added textfile file
public:
    Map(const int num_of_files);
    ~Map();
    void add_new_textfile(const char *fpath, int num_of_lines);
    bool insert_line_to_textfile(int fileID, int lineID, const char *line, int line_length);
    /* Accessors */
    const char *getPTRforline(int fileID, const int lineID) const;
    char *getFilePath(int fileID) const;
    int getFileCount() const;
    int getFileLinesCount(int fileID) const;
    /* Public members */
    unsigned int byteCount;     // how many bytes in all textfiles on the map
    unsigned int wordCount;     //   -//-   words   -//-    -//-    -//-
    unsigned int lineCount;     //   -//-   lines   -//-    -//-    -//-
};


#endif //MAP_H
