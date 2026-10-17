/*
 * Copyright (C) Rida Bazzi, 2017
 *
 * Do not share this file with anyone
 */
#ifndef __INPUT_BUFFER__H__
#define __INPUT_BUFFER__H__

#include <string>
#include <vector>

class InputBuffer {
  public:
    void GetChar(char&);
    char UngetChar(char);
    std::string UngetString(std::string);
    bool EndOfInput();

    // loads entire file contents into the input buffer
    // returns true on success, false if file cannot be opened
    bool InitFromFile(const std::string& fileName);
    void InitFromString(const std::string& s);

  private:
    std::vector<char> input_buffer;

    bool loaded_from_file = false; // true if InitFromFile was called
};

#endif  //__INPUT_BUFFER__H__
