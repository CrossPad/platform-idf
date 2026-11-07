#include "BleControlMailbox.hpp"
#include <cassert>
#include <string>
int main() {
    crosspad::BleControlMailbox box;
    char command[192]; uint32_t generation;
    uint8_t response[481];
    const auto write = [&](const char *text) { return box.write(reinterpret_cast<const uint8_t *>(text), strlen(text)); };
    assert(box.read(response) == 1 && response[0] == 0);
    assert(write("UI_")); assert(!box.take(command, generation));
    assert(write("STATE\n")); assert(!write("APP_STOP\n"));
    assert(box.take(command, generation)); assert(std::string(command) == "UI_STATE");
    box.finish("UI: app=Sampler\r\n", generation);
    assert(box.read(response) == 18 && response[0] == 1);
    assert(write("APP_STOP\n")); assert(box.read(response) == 1 && response[0] == 0);
    assert(box.take(command, generation)); box.reset();
    box.finish("OK\r\n", generation); assert(box.read(response) == 1 && response[0] == 0);
    assert(!write("UI_STATE\nAPP_STOP\n"));
    assert(!write(std::string(192, 'x').c_str()));
    assert(write("CP_CAPS\n")); assert(box.take(command, generation));
    assert(std::string(command) == "CP_CAPS");
}
