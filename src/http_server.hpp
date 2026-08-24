#pragma once

#include <boost/asio.hpp>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <memory>

class HttpServer {
public:
    HttpServer(uint16_t port, const std::string& deviceName,
               const std::string& musicDir = "/opt/quantumsync-local/music",
               const std::string& stateDir = "/var/lib/quantumsync-local");
    ~HttpServer();

    void start();
    void stop();

private:
    void acceptLoop();
    void handleSession(boost::asio::ip::tcp::socket socket);
    std::string handleRequest(const std::string& method, const std::string& path,
                              const std::string& body);

    // API handlers
    std::string getStatusJson();
    std::string handleVolume(const std::string& body);
    std::string handleMute(const std::string& body);
    std::string handlePlayback(const std::string& body);
    std::string getJournalJson();
    std::string getFoldersJson();
    std::string handleFolder(const std::string& body);
    std::string handleLibraryUpdate();

    // Pages
    std::string getMainPage();
    std::string getLogsPage();

    // System stats (Linux /proc)
    double getUptimeSeconds();
    double getCpuPercent();
    int getMemUsedMb();
    int getMemTotalMb();

    // Shell command helpers
    std::string execCommand(const std::string& cmd);
    static std::string shellQuote(const std::string& s);
    static std::string jsonEscape(const std::string& s);
    int getVolume();
    void setVolume(int volume);

    // Folder selection helpers
    std::vector<std::string> listMusicFolders();
    std::string loadSelectedFolder();
    void saveSelectedFolder(const std::string& folder);

    uint16_t port_;
    std::string deviceName_;
    std::string musicDir_;
    std::string stateDir_;
    std::atomic<bool> running_{false};
    std::atomic<bool> muted_{false};
    int preMuteVolume_{50};

    boost::asio::io_context ioContext_;
    std::thread serverThread_;
    std::shared_ptr<boost::asio::ip::tcp::acceptor> acceptor_;
};
