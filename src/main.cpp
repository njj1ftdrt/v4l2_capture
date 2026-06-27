#include "camera_device.hpp"

#include <iostream>
#include <string>

int main(int argc, char* argv[]) {
    std::string device = "/dev/video10";

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        if ((arg == "--device" || arg == "-d") && i + 1 < argc) {
            device = argv[++i];
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: " << argv[0] << " --device /dev/video10\n";
            return 0;
        } else {
            std::cerr << "Unknown argument: " << arg << "\n";
            std::cerr << "Usage: " << argv[0] << " --device /dev/video10\n";
            return 1;
        }
    }

    try {
        CameraDevice camera(device);
        camera.open_device();
        camera.query_capability();
    } catch (const std::exception& e) {
        std::cerr << "[ERROR] " << e.what() << "\n";
        return 1;
    }

    return 0;
}
