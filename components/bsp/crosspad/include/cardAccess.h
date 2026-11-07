#pragma once
#include "CrossPad.h"
#include <string>


// stores state information for the sampler background task and its controls
struct CardAccess
{
    bool busy = false; // set to true when SampleStreamPlayer_BkgProc is running
    bool stopRequested = false; // set to true to pause execution, wait until busy is false
    bool terminateTask = false; // set to true to break task loop
};

struct FileItem
{
    std::string path;
    std::string name;
    bool isFolder;
};