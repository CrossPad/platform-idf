#pragma once

#include <stdio.h>

#ifdef __cplusplus
#include <ArduinoJson.h>

class ArduinoJsonFileReader {
private:
    FILE* file_;
public:
    ArduinoJsonFileReader(FILE* file) : file_(file) {}
    int read() {
        return fgetc(file_);
    }
};

class ArduinoJsonFileWriter {
private:
    FILE* file_;
public:
    ArduinoJsonFileWriter(FILE* file) : file_(file) {}
    size_t write(uint8_t c) {
        return fputc(c, file_) != EOF ? 1 : 0;
    }
    size_t write(const uint8_t* s, size_t n) {
        return fwrite(s, 1, n, file_);
    }
};
#endif
