/*
 * Copyright (C) Rida Bazzi, 2017
 *
 * Do not share this file with anyone
 */
#include <iostream>
#include <istream>
#include <fstream> 
#include <vector>
#include <string>
#include <cstdio>

#include "inputbuf.h"

using namespace std;

// CLI support
// load entire file into input_buffer so GetChar() reads from it
// returns true on success, false if file cannot be opened
bool InputBuffer::InitFromFile(const string& filename)
{
    ifstream file(filename, ios::binary);
    if (!file.is_open())
    {
        return false;
    }

    // read entire file into input_buffer in reverse order
    // because GetChar() pops from the back (stack-based buffer)
    string content((istreambuf_iterator<char>(file)),
                    istreambuf_iterator<char>());
    file.close();

    // strip UTF-8 BOM (EF BB BF) if present — PowerShell adds this
    if (content.size() >= 3 &&
        (unsigned char)content[0] == 0xEF &&
        (unsigned char)content[1] == 0xBB &&
        (unsigned char)content[2] == 0xBF)
    {
        content = content.substr(3);
    }

    // strip UTF-16 LE BOM (FF FE) if present
    if (content.size() >= 2 &&
        (unsigned char)content[0] == 0xFF &&
        (unsigned char)content[1] == 0xFE)
    {
        content = content.substr(2);
    }

    // push the characters in reverse so first char is at back
    for (int i = (int)content.size() - 1;i >= 0;i--)
    {
        input_buffer.push_back(content[i]);
    }
    loaded_from_file = true;
    return true;
}

// REPL support
// loads a string directly into the input buffer
// characters are pushed in reverse order so that GetChar() reads left to write
void InputBuffer::InitFromString(const std::string& s)
{
    input_buffer.clear();
    loaded_from_file = true;  // treat as file, do not fall back to stdin

    // push in reverse so first char is at back 
    for (int i = (int)s.size() - 1;i >= 0;i--)
    {
        input_buffer.push_back(s[i]);
    }
}

bool InputBuffer::EndOfInput()
{
    if (!input_buffer.empty())
        return false;

    // CLI mode: file is exhausted when buffer is empty
    if (loaded_from_file)
        return true;

    //check stdin mode as befire
    return cin.eof();
}

char InputBuffer::UngetChar(char c)
{
    if (c != EOF)
        input_buffer.push_back(c);;
    return c;
}

void InputBuffer::GetChar(char& c)
{
    if (!input_buffer.empty()) {
        c = input_buffer.back();
        input_buffer.pop_back();
    } else if (loaded_from_file) {
        c = '\0';
    } else {
        cin.get(c);
    }
}

string InputBuffer::UngetString(string s)
{
    for (int i = 0; i < s.size(); i++)
        input_buffer.push_back(s[s.size()-i-1]);
    return s;
}
