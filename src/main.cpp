#include "camera_device.hpp"

#include <iostream>
#include <string>

int main(int argc, char* argv[]) {
    std::string device = "/dev/video10";
    bool list_formats = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        if ((arg == "--device" || arg == "-d") && i + 1 < argc) {
            device = argv[++i];
        } else if (arg == "--list-formats") {
            list_formats = true;
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: " << argv[0]
                      << " --device /dev/video10 [--list-formats]\n";
            return 0;
        } else {
            std::cerr << "Unknown argument: " << arg << "\n";
            std::cerr << "Usage: " << argv[0]
                      << " --device /dev/video10 [--list-formats]\n";
            return 1;
        }
    }

    try {
        CameraDevice camera(device);
        camera.open_device();
        camera.query_capability();

        if (list_formats) {
            camera.list_formats();
        }
    } catch (const std::exception& e) {
        std::cerr << "[ERROR] " << e.what() << "\n";
        return 1;
    }

    return 0;
}
