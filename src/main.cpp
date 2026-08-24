#include "http_server.hpp"
#include <iostream>
#include <fstream>
#include <csignal>
#include <atomic>

std::atomic<bool> g_shouldRun{true};

void signalHandler(int) {
    std::cout << "\nShutting down..." << std::endl;
    g_shouldRun = false;
}

std::string readConfig(const std::string& path, const std::string& key, const std::string& defaultVal) {
    std::ifstream file(path);
    if (!file.is_open()) return defaultVal;

    std::string line;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') continue;
        size_t pos = line.find('=');
        if (pos == std::string::npos) continue;
        if (line.substr(0, pos) == key) {
            std::string value = line.substr(pos + 1);
            // Trim whitespace
            value.erase(0, value.find_first_not_of(" \t\r\n"));
            value.erase(value.find_last_not_of(" \t\r\n") + 1);
            return value;
        }
    }
    return defaultVal;
}

int main() {
    std::cout << "QuantumSync Local v2.0" << std::endl;

    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    // Read config
    std::string configPath = "/etc/quantumsync-local/config.conf";
    std::string deviceName = readConfig(configPath, "DEVICE_NAME", "Local Player");
    std::string musicDir = readConfig(configPath, "MUSIC_DIR", "/opt/quantumsync-local/music");
    std::string stateDir = readConfig(configPath, "STATE_DIR", "/var/lib/quantumsync-local");
    int port = 1706;
    try {
        port = std::stoi(readConfig(configPath, "HTTP_PORT", "1706"));
    } catch (...) {}

    std::cout << "Device: " << deviceName << std::endl;

    // Start HTTP server
    HttpServer server(static_cast<uint16_t>(port), deviceName, musicDir, stateDir);
    server.start();

    // Block until signal
    while (g_shouldRun) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    server.stop();
    std::cout << "QuantumSync Local stopped." << std::endl;
    return 0;
}
