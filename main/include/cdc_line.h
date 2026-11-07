/**
 * @file cdc_line.h
 * @brief Assembles CDC command lines from a byte stream, one byte at a time.
 *
 * A command ends at its terminator (CR or LF) and nowhere else. A line longer
 * than the buffer is not dispatched at all, neither its head nor its tail:
 * from the byte that does not fit, everything up to the next terminator is
 * swallowed and the terminator reports TooLong once. Before this, the buffer
 * was emptied on overflow and the bytes after it collected as a new line, so
 * the tail of a long FS_PUT path ran as a command of its own (2026-11-07).
 *
 * Portable (no IDF): the host tests in main/tests/ drive it directly.
 */
#pragma once

#include <cstddef>

/** Bytes in the line buffer, NUL included: the longest command is one less.
 *  Sized for the longest FS_* line, `FS_GET <off> <len> <path>` with a path of
 *  FX_PATH_MAX - 1 characters (file_transfer.cpp checks it); fx_handle_cdc()
 *  copies a line into a buffer of this size too, so nothing is cut short. */
#define CDC_LINE_MAX 224

class CdcLineAssembler {
public:
    enum class Result { None, Line, TooLong };

    /** Feed one byte. Line: text()/size() hold the line (NUL-terminated) until
     *  the next feed(). TooLong: an over-long line just ended and was dropped. */
    Result feed(char c)
    {
        if (c == '\n' || c == '\r') {
            if (discarding_) {
                discarding_ = false;
                len_ = 0;
                return Result::TooLong;
            }
            if (len_ == 0) return Result::None;     /* CRLF, blank line */
            buf_[len_] = '\0';
            ready_ = len_;
            len_ = 0;
            return Result::Line;
        }
        ready_ = 0;
        if (discarding_) return Result::None;
        if (len_ < sizeof(buf_) - 1) {
            buf_[len_++] = c;
        } else {
            discarding_ = true;
        }
        return Result::None;
    }

    /** Forget a partial line, and an over-long one being swallowed: the stream
     *  became payload (OTA, FS_PUT, ASSETS_BEGIN). */
    void reset()
    {
        len_ = 0;
        ready_ = 0;
        discarding_ = false;
    }

    const char *text() const { return buf_; }
    size_t size() const { return ready_; }

private:
    char   buf_[CDC_LINE_MAX] = {};
    size_t len_ = 0;            /* bytes of the line being assembled */
    size_t ready_ = 0;          /* length of the line feed() just returned */
    bool   discarding_ = false; /* over the limit: drop up to the terminator */
};
