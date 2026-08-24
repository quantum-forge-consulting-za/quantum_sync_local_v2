#include "http_server.hpp"
#include <iostream>
#include <sstream>
#include <fstream>
#include <algorithm>
#include <chrono>
#include <thread>
#include <cstdio>
#include <cstring>
#include <filesystem>

using boost::asio::ip::tcp;

HttpServer::HttpServer(uint16_t port, const std::string& deviceName,
                       const std::string& musicDir, const std::string& stateDir)
    : port_(port)
    , deviceName_(deviceName)
    , musicDir_(musicDir)
    , stateDir_(stateDir)
{
}

HttpServer::~HttpServer() {
    stop();
}

void HttpServer::start() {
    if (running_) return;
    running_ = true;

    serverThread_ = std::thread([this]() {
        try {
            acceptLoop();
        } catch (const std::exception& e) {
            std::cerr << "HTTP server error: " << e.what() << std::endl;
            // A dead accept loop means the GUI is gone for good — exit so
            // systemd (Restart=always) brings the whole service back up.
            std::exit(1);
        }
    });

    std::cout << "QuantumSync Local web GUI on port " << port_ << std::endl;
}

void HttpServer::stop() {
    if (!running_.exchange(false)) return;

    // Closing the acceptor does not wake a blocking accept() on Linux, so
    // poke it with a throwaway local connection to let the loop observe
    // running_ == false and exit.
    try {
        boost::asio::io_context tmp;
        tcp::socket s(tmp);
        s.connect(tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), port_));
        boost::system::error_code ignored;
        s.close(ignored);
    } catch (...) {}

    if (acceptor_) {
        boost::system::error_code ec;
        acceptor_->close(ec);
    }
    ioContext_.stop();

    if (serverThread_.joinable()) {
        serverThread_.join();
    }
}

void HttpServer::acceptLoop() {
    acceptor_ = std::make_shared<tcp::acceptor>(ioContext_, tcp::endpoint(tcp::v4(), port_));
    acceptor_->set_option(boost::asio::socket_base::reuse_address(true));

    while (running_) {
        try {
            tcp::socket socket(ioContext_);
            acceptor_->accept(socket);

            try {
                handleSession(std::move(socket));
            } catch (const std::exception& e) {
                std::cerr << "HTTP session error: " << e.what() << std::endl;
            }
        } catch (const boost::system::system_error& e) {
            if (running_) {
                std::cerr << "HTTP accept error: " << e.what() << std::endl;
            }
        }
    }
}

void HttpServer::handleSession(tcp::socket socket) {
    boost::asio::streambuf request;
    boost::asio::read_until(socket, request, "\r\n\r\n");

    std::istream request_stream(&request);
    std::string method, path, version;
    request_stream >> method >> path >> version;

    // Parse headers for Content-Length
    std::string header_line;
    std::getline(request_stream, header_line);
    int content_length = 0;
    while (std::getline(request_stream, header_line) && header_line != "\r") {
        if (header_line.find("Content-Length:") == 0) {
            std::string len_str = header_line.substr(15);
            len_str.erase(0, len_str.find_first_not_of(" \t"));
            len_str.erase(len_str.find_last_not_of(" \r\n\t") + 1);
            try {
                content_length = std::stoi(len_str);
                if (content_length < 0 || content_length > 10240) content_length = 0;
            } catch (...) { content_length = 0; }
        }
    }

    // Read body if present
    std::string body;
    if (content_length > 0) {
        size_t already_read = request.size();
        if (already_read > 0) {
            std::vector<char> buf(already_read);
            request_stream.read(buf.data(), already_read);
            body.assign(buf.begin(), buf.end());
        }
        if (static_cast<int>(body.size()) < content_length) {
            size_t remaining = content_length - body.size();
            std::vector<char> extra(remaining);
            boost::asio::read(socket, boost::asio::buffer(extra));
            body.append(extra.begin(), extra.end());
        }
    }

    std::string content = handleRequest(method, path, body);

    std::string content_type = "text/html";
    if (path.find("/api/") == 0) content_type = "application/json";

    std::ostringstream response;
    response << "HTTP/1.1 200 OK\r\n";
    response << "Content-Type: " << content_type << "\r\n";
    response << "Content-Length: " << content.length() << "\r\n";
    response << "Access-Control-Allow-Origin: *\r\n";
    response << "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n";
    response << "Access-Control-Allow-Headers: Content-Type\r\n";
    response << "Connection: close\r\n\r\n";
    response << content;

    boost::asio::write(socket, boost::asio::buffer(response.str()));
}

std::string HttpServer::handleRequest(const std::string& method,
                                       const std::string& path,
                                       const std::string& body) {
    if (path == "/" || path == "/index.html") return getMainPage();
    else if (path == "/api/status" && method == "GET") return getStatusJson();
    else if (path == "/api/volume" && method == "POST") return handleVolume(body);
    else if (path == "/api/mute" && method == "POST") return handleMute(body);
    else if (path == "/api/playback" && method == "POST") return handlePlayback(body);
    else if (path == "/api/journal" && method == "GET") return getJournalJson();
    else if (path == "/api/folders" && method == "GET") return getFoldersJson();
    else if (path == "/api/folder" && method == "POST") return handleFolder(body);
    else if (path == "/api/update" && method == "POST") return handleLibraryUpdate();
    else if (path == "/logs" || path == "/logs.html") return getLogsPage();
    else if (method == "OPTIONS") return "";
    return "{\"error\":\"Not found\"}";
}

// ─── Shell command helper ──────────────────────────────────────

std::string HttpServer::execCommand(const std::string& cmd) {
    std::string result;
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return "";
    char buffer[256];
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        result += buffer;
    }
    pclose(pipe);
    // Trim trailing newline
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r')) {
        result.pop_back();
    }
    return result;
}

std::string HttpServer::shellQuote(const std::string& s) {
    // Wrap in single quotes; escape embedded single quotes as '\''
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    out += "'";
    return out;
}

std::string HttpServer::jsonEscape(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '"') out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else if (static_cast<unsigned char>(c) < 0x20) out += ' ';
        else out += c;
    }
    return out;
}

// ─── Folder selection ──────────────────────────────────────────

std::vector<std::string> HttpServer::listMusicFolders() {
    std::vector<std::string> folders;
    try {
        for (const auto& entry : std::filesystem::directory_iterator(musicDir_)) {
            if (!entry.is_directory()) continue;
            std::string name = entry.path().filename().string();
            if (name.empty() || name[0] == '.') continue;  // skip hidden dirs
            folders.push_back(name);
        }
    } catch (...) {}
    std::sort(folders.begin(), folders.end(), [](const std::string& a, const std::string& b) {
        return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end(),
            [](char x, char y) { return std::tolower(static_cast<unsigned char>(x))
                                      < std::tolower(static_cast<unsigned char>(y)); });
    });
    return folders;
}

std::string HttpServer::loadSelectedFolder() {
    std::ifstream f(stateDir_ + "/selected_folder");
    if (!f.is_open()) return "";
    std::string folder;
    std::getline(f, folder);
    // Trim whitespace
    folder.erase(0, folder.find_first_not_of(" \t\r\n"));
    folder.erase(folder.find_last_not_of(" \t\r\n") + 1);
    return folder;
}

void HttpServer::saveSelectedFolder(const std::string& folder) {
    try {
        std::ofstream f(stateDir_ + "/selected_folder", std::ios::trunc);
        if (f.is_open()) f << folder << "\n";
    } catch (...) {}
}

std::string HttpServer::getFoldersJson() {
    std::vector<std::string> folders = listMusicFolders();
    std::string current = loadSelectedFolder();

    std::ostringstream json;
    json << "{\"folders\":[";
    for (size_t i = 0; i < folders.size(); ++i) {
        if (i > 0) json << ",";
        json << "\"" << jsonEscape(folders[i]) << "\"";
    }
    json << "],\"current\":\"" << jsonEscape(current) << "\"}";
    return json.str();
}

std::string HttpServer::handleFolder(const std::string& body) {
    // Parse "folder" value from JSON body
    std::string folder;
    size_t pos = body.find("\"folder\"");
    if (pos == std::string::npos) return "{\"error\":\"Missing folder\"}";
    size_t colon = body.find(':', pos + 8);
    if (colon == std::string::npos) return "{\"error\":\"Missing folder\"}";
    size_t start = body.find('"', colon);
    if (start == std::string::npos) return "{\"error\":\"Missing folder\"}";
    start++;
    // Read until closing quote, honouring \" and \\ escapes
    std::string parsed;
    bool closed = false;
    for (size_t i = start; i < body.size(); ++i) {
        char c = body[i];
        if (c == '\\' && i + 1 < body.size()) {
            parsed += body[++i];
        } else if (c == '"') {
            closed = true;
            break;
        } else {
            parsed += c;
        }
    }
    if (!closed) return "{\"error\":\"Bad folder value\"}";
    folder = parsed;

    // Empty string means "All Music". Otherwise the name must exactly match
    // an existing top-level folder (this also blocks any path tricks).
    if (!folder.empty()) {
        std::vector<std::string> folders = listMusicFolders();
        if (std::find(folders.begin(), folders.end(), folder) == folders.end()) {
            return "{\"error\":\"Unknown folder\"}";
        }
    }

    // Swap the queue: clear, add the selection, play.
    // repeat/random are global MPD settings and stay as they were.
    execCommand("mpc clear 2>/dev/null");
    if (folder.empty()) {
        execCommand("mpc add / 2>/dev/null");
    } else {
        execCommand("mpc add " + shellQuote(folder) + " 2>/dev/null");
    }
    execCommand("mpc play 2>/dev/null");

    saveSelectedFolder(folder);

    return "{\"result\":\"OK\",\"folder\":\"" + jsonEscape(folder) + "\"}";
}

std::string HttpServer::handleLibraryUpdate() {
    // Rescan the music library (picks up files copied in over the network).
    // Runs in the background; auto_update usually handles this, but the
    // button gives a guaranteed manual refresh.
    execCommand("mpc update 2>/dev/null");
    return "{\"result\":\"OK\"}";
}

// ─── Volume via amixer ─────────────────────────────────────────

int HttpServer::getVolume() {
    // Try to parse amixer output for current volume percentage
    std::string output = execCommand("amixer sget Headphone 2>/dev/null || amixer sget Master 2>/dev/null || amixer sget PCM 2>/dev/null");
    // Look for [XX%] pattern
    size_t pos = output.find('[');
    while (pos != std::string::npos) {
        size_t end = output.find('%', pos);
        if (end != std::string::npos && end > pos + 1) {
            std::string pct = output.substr(pos + 1, end - pos - 1);
            try {
                int vol = std::stoi(pct);
                if (vol >= 0 && vol <= 100) return vol;
            } catch (...) {}
        }
        pos = output.find('[', pos + 1);
    }
    return 50;  // Fallback
}

void HttpServer::setVolume(int volume) {
    volume = std::clamp(volume, 0, 100);
    std::string cmd = "amixer sset Headphone " + std::to_string(volume) + "% 2>/dev/null"
                      " || amixer sset Master " + std::to_string(volume) + "% 2>/dev/null"
                      " || amixer sset PCM " + std::to_string(volume) + "% 2>/dev/null";
    execCommand(cmd);
}

// ─── API Handlers ──────────────────────────────────────────────

std::string HttpServer::getStatusJson() {
    std::string currentTrack = execCommand("mpc current 2>/dev/null");
    if (currentTrack.empty()) currentTrack = "No music loaded";

    // Parse mpc status for state
    std::string mpcStatus = execCommand("mpc status 2>/dev/null");
    std::string state = "stopped";
    if (mpcStatus.find("[playing]") != std::string::npos) state = "playing";
    else if (mpcStatus.find("[paused]") != std::string::npos) state = "paused";

    // Track count
    std::string trackCount = execCommand("mpc playlist 2>/dev/null | wc -l");

    int volume = muted_.load() ? 0 : getVolume();

    std::ostringstream json;
    json << std::fixed;
    json.precision(1);
    json << "{"
         << "\"track\":\"" << jsonEscape(currentTrack) << "\","
         << "\"state\":\"" << state << "\","
         << "\"volume\":" << volume << ","
         << "\"muted\":" << (muted_.load() ? "true" : "false") << ","
         << "\"deviceName\":\"" << jsonEscape(deviceName_) << "\","
         << "\"folder\":\"" << jsonEscape(loadSelectedFolder()) << "\","
         << "\"trackCount\":" << (trackCount.empty() ? "0" : trackCount) << ","
         << "\"uptimeSec\":" << getUptimeSeconds() << ","
         << "\"cpuPercent\":" << getCpuPercent() << ","
         << "\"memUsedMb\":" << getMemUsedMb() << ","
         << "\"memTotalMb\":" << getMemTotalMb()
         << "}";
    return json.str();
}

std::string HttpServer::handleVolume(const std::string& body) {
    size_t pos = body.find("\"volume\"");
    if (pos != std::string::npos) {
        size_t colon = body.find(':', pos);
        if (colon != std::string::npos) {
            size_t end = body.find_first_of(",}", colon);
            if (end != std::string::npos) {
                std::string val = body.substr(colon + 1, end - colon - 1);
                val.erase(0, val.find_first_not_of(" \t"));
                try {
                    int volume = std::stoi(val);
                    volume = std::clamp(volume, 0, 100);
                    setVolume(volume);
                    if (muted_.load()) {
                        muted_ = false;  // Unmute when volume is explicitly set
                    }
                    return "{\"result\":\"OK\",\"volume\":" + std::to_string(volume) + "}";
                } catch (...) {}
            }
        }
    }
    return "{\"error\":\"Invalid volume\"}";
}

std::string HttpServer::handleMute(const std::string& body) {
    if (body.find("true") != std::string::npos) {
        preMuteVolume_ = getVolume();
        muted_ = true;
        setVolume(0);
        return "{\"result\":\"OK\",\"muted\":true}";
    } else {
        muted_ = false;
        setVolume(preMuteVolume_);
        return "{\"result\":\"OK\",\"muted\":false,\"volume\":" +
               std::to_string(preMuteVolume_) + "}";
    }
}

std::string HttpServer::handlePlayback(const std::string& body) {
    std::string action;

    // Parse action from JSON
    size_t pos = body.find("\"action\"");
    if (pos != std::string::npos) {
        size_t start = body.find('"', pos + 8);
        if (start != std::string::npos) {
            start++;
            size_t end = body.find('"', start);
            if (end != std::string::npos) {
                action = body.substr(start, end - start);
            }
        }
    }

    if (action == "play") {
        execCommand("mpc play 2>/dev/null");
    } else if (action == "pause") {
        execCommand("mpc pause 2>/dev/null");
    } else if (action == "toggle") {
        execCommand("mpc toggle 2>/dev/null");
    } else if (action == "next") {
        execCommand("mpc next 2>/dev/null");
    } else if (action == "prev") {
        execCommand("mpc prev 2>/dev/null");
    } else {
        return "{\"error\":\"Unknown action: " + action + "\"}";
    }

    return "{\"result\":\"OK\",\"action\":\"" + action + "\"}";
}

// ─── System stats (Linux /proc) ────────────────────────────────

double HttpServer::getUptimeSeconds() {
    try {
        std::ifstream f("/proc/uptime");
        if (f.is_open()) {
            double uptime = 0.0;
            f >> uptime;
            return uptime;
        }
    } catch (...) {}
    return -1.0;
}

double HttpServer::getCpuPercent() {
    auto readCpuTimes = []() -> std::pair<long long, long long> {
        std::ifstream f("/proc/stat");
        if (!f.is_open()) return {0, 0};
        std::string cpu;
        long long user, nice, system, idle, iowait, irq, softirq, steal;
        f >> cpu >> user >> nice >> system >> idle >> iowait >> irq >> softirq >> steal;
        long long totalIdle = idle + iowait;
        long long total = user + nice + system + idle + iowait + irq + softirq + steal;
        return {total, totalIdle};
    };

    auto [total1, idle1] = readCpuTimes();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    auto [total2, idle2] = readCpuTimes();

    long long totalDelta = total2 - total1;
    long long idleDelta = idle2 - idle1;

    if (totalDelta <= 0) return 0.0;
    return 100.0 * (1.0 - static_cast<double>(idleDelta) / static_cast<double>(totalDelta));
}

int HttpServer::getMemUsedMb() {
    try {
        std::ifstream f("/proc/meminfo");
        if (!f.is_open()) return -1;
        long long memTotal = 0, memAvailable = 0;
        std::string line;
        while (std::getline(f, line)) {
            if (line.find("MemTotal:") == 0) {
                std::istringstream iss(line.substr(9));
                iss >> memTotal;
            } else if (line.find("MemAvailable:") == 0) {
                std::istringstream iss(line.substr(13));
                iss >> memAvailable;
            }
        }
        return static_cast<int>((memTotal - memAvailable) / 1024);
    } catch (...) {}
    return -1;
}

int HttpServer::getMemTotalMb() {
    try {
        std::ifstream f("/proc/meminfo");
        if (!f.is_open()) return -1;
        std::string line;
        while (std::getline(f, line)) {
            if (line.find("MemTotal:") == 0) {
                std::istringstream iss(line.substr(9));
                long long kb = 0;
                iss >> kb;
                return static_cast<int>(kb / 1024);
            }
        }
    } catch (...) {}
    return -1;
}

// ─── Journal API ───────────────────────────────────────────────

std::string HttpServer::getJournalJson() {
    std::string result;
    FILE* pipe = popen("journalctl -u quantumsync-local -u quantumsync-local-mpd -n 100 --no-pager 2>&1", "r");
    if (!pipe) {
        return "{\"error\":\"Failed to read journal\",\"lines\":[],\"count\":0}";
    }

    std::vector<std::string> lines;
    char buffer[1024];
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        std::string line(buffer);
        if (!line.empty() && line.back() == '\n') line.pop_back();
        lines.push_back(line);
    }
    pclose(pipe);

    std::ostringstream json;
    json << "{\"lines\":[";
    for (size_t i = 0; i < lines.size(); ++i) {
        if (i > 0) json << ",";
        json << "\"";
        for (char c : lines[i]) {
            if (c == '"') json << "\\\"";
            else if (c == '\\') json << "\\\\";
            else if (c == '\n') json << "\\n";
            else if (c == '\r') json << "\\r";
            else if (c == '\t') json << "\\t";
            else if (static_cast<unsigned char>(c) < 0x20) json << " ";
            else json << c;
        }
        json << "\"";
    }
    json << "],\"count\":" << lines.size() << "}";
    return json.str();
}

// ─── Embedded HTML GUI ─────────────────────────────────────────

std::string HttpServer::getMainPage() {
    return R"HTML(<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>QuantumSync Local</title>
    <link rel="icon" type="image/svg+xml" href="data:image/svg+xml;base64,PHN2ZyB3aWR0aD0iNTEyIiBoZWlnaHQ9IjUxMiIgdmlld0JveD0iMCAwIDUxMiA1MTIiIGZpbGw9Im5vbmUiIHhtbG5zPSJodHRwOi8vd3d3LnczLm9yZy8yMDAwL3N2ZyI+CiAgPCEtLSBRdWFudHVtIFggV29ya3Mg4oCUIE1hcmsgb25seSAoaWNvbikuIFJlZmluZWQgYXRvbSArIGNvZGUgYnJhY2tldHMuIC0tPgogIDwhLS0gTm8gZmlsdGVycywgbm8gYmluYXJ5IHRleHQ6IGJ1aWx0IHRvIHJlYWQgY2xlYW4gYXQgYW55IHNpemUgaW5jbC4gZmF2aWNvbi4gLS0+CiAgPGRlZnM+CiAgICA8bGluZWFyR3JhZGllbnQgaWQ9InF4d0dyYWQiIHgxPSIwIiB5MT0iMCIgeDI9IjUxMiIgeTI9IjUxMiIgZ3JhZGllbnRVbml0cz0idXNlclNwYWNlT25Vc2UiPgogICAgICA8c3RvcCBvZmZzZXQ9IjAiIHN0b3AtY29sb3I9IiMwRjczQzMiLz4KICAgICAgPHN0b3Agb2Zmc2V0PSIxIiBzdG9wLWNvbG9yPSIjMDBCNkYxIi8+CiAgICA8L2xpbmVhckdyYWRpZW50PgogICAgPGxpbmVhckdyYWRpZW50IGlkPSJxeHdDb3JlIiB4MT0iMjA2IiB5MT0iMjA2IiB4Mj0iMzA2IiB5Mj0iMzA2IiBncmFkaWVudFVuaXRzPSJ1c2VyU3BhY2VPblVzZSI+CiAgICAgIDxzdG9wIG9mZnNldD0iMCIgc3RvcC1jb2xvcj0iIzAwQjZGMSIvPgogICAgICA8c3RvcCBvZmZzZXQ9IjEiIHN0b3AtY29sb3I9IiMwRjczQzMiLz4KICAgIDwvbGluZWFyR3JhZGllbnQ+CiAgPC9kZWZzPgoKICA8IS0tIE9yYml0YWwgcmluZ3MgKDMsIGV2ZW5seSByb3RhdGVkKSAtLT4KICA8ZyBzdHJva2U9InVybCgjcXh3R3JhZCkiIHN0cm9rZS13aWR0aD0iMTAiIGZpbGw9Im5vbmUiIG9wYWNpdHk9IjAuODUiPgogICAgPGVsbGlwc2UgY3g9IjI1NiIgY3k9IjI1NiIgcng9IjE1MCIgcnk9IjY2Ii8+CiAgICA8ZWxsaXBzZSBjeD0iMjU2IiBjeT0iMjU2IiByeD0iMTUwIiByeT0iNjYiIHRyYW5zZm9ybT0icm90YXRlKDYwIDI1NiAyNTYpIi8+CiAgICA8ZWxsaXBzZSBjeD0iMjU2IiBjeT0iMjU2IiByeD0iMTUwIiByeT0iNjYiIHRyYW5zZm9ybT0icm90YXRlKDEyMCAyNTYgMjU2KSIvPgogIDwvZz4KCiAgPCEtLSBDb2RlIGJyYWNrZXRzIOKAlCBoZWF2aWVyLCBzaW1wbGUsIGZyYW1lIHRoZSBhdG9tIC0tPgogIDxwYXRoIGQ9Ik0xMzIgMTUwIEw4NiAyNTYgTDEzMiAzNjIiIHN0cm9rZT0iIzAwQjZGMSIgc3Ryb2tlLXdpZHRoPSIyNiIKICAgICAgICBzdHJva2UtbGluZWNhcD0icm91bmQiIHN0cm9rZS1saW5lam9pbj0icm91bmQiIGZpbGw9Im5vbmUiLz4KICA8cGF0aCBkPSJNMzgwIDE1MCBMNDI2IDI1NiBMMzgwIDM2MiIgc3Ryb2tlPSIjMDBCNkYxIiBzdHJva2Utd2lkdGg9IjI2IgogICAgICAgIHN0cm9rZS1saW5lY2FwPSJyb3VuZCIgc3Ryb2tlLWxpbmVqb2luPSJyb3VuZCIgZmlsbD0ibm9uZSIvPgoKICA8IS0tIE9yYml0aW5nIGVsZWN0cm9ucyAoMywgb25lIHBlciByaW5nIGludGVyc2VjdGlvbikgLS0+CiAgPGNpcmNsZSBjeD0iMjU2IiBjeT0iMTkwIiByPSIxNiIgZmlsbD0iIzBGNzNDMyIvPgogIDxjaXJjbGUgY3g9IjMxMyIgY3k9IjI4OSIgcj0iMTYiIGZpbGw9IiMwRjczQzMiLz4KICA8Y2lyY2xlIGN4PSIxOTkiIGN5PSIyODkiIHI9IjE2IiBmaWxsPSIjMEY3M0MzIi8+CgogIDwhLS0gTnVjbGV1cyAtLT4KICA8Y2lyY2xlIGN4PSIyNTYiIGN5PSIyNTYiIHI9IjUwIiBmaWxsPSJ1cmwoI3F4d0NvcmUpIi8+CiAgPGNpcmNsZSBjeD0iMjU2IiBjeT0iMjU2IiByPSIyMCIgZmlsbD0iI0ZGRkZGRiIvPgo8L3N2Zz4K">
    <link rel="preconnect" href="https://fonts.googleapis.com">
    <link href="https://fonts.googleapis.com/css2?family=Exo+2:wght@300;700;900&display=swap" rel="stylesheet">
    <style>
        * { margin: 0; padding: 0; box-sizing: border-box; }
        body {
            font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif;
            background: linear-gradient(135deg, #0f0c29, #302b63, #24243e);
            color: #fff;
            min-height: 100vh;
            display: flex;
            align-items: center;
            justify-content: center;
        }
        .card {
            background: rgba(255,255,255,0.08);
            border-radius: 16px;
            padding: 32px;
            width: 380px;
            backdrop-filter: blur(10px);
            border: 1px solid rgba(255,255,255,0.1);
        }
        .brand-header { text-align: center; margin-bottom: 16px; }
        .brand-header svg { display: block; margin: 0 auto 6px; height: 52px; width: auto; }
        .brand-caption { color: rgba(255,255,255,0.35); font-size: 0.7em; text-transform: uppercase; letter-spacing: 2px; }
        .subtitle { color: rgba(255,255,255,0.6); font-size: 0.95em; margin: 4px 0 20px; text-align: center; font-weight: 500; }
        .footer-brand {
            margin-top: 18px; padding-top: 14px;
            border-top: 1px solid rgba(255,255,255,0.06);
            text-align: center;
        }
        .footer-brand img { max-height: 40px; max-width: 200px; display: block; margin: 0 auto 6px; }
        .footer-powered { color: rgba(255,255,255,0.3); font-size: 0.7em; letter-spacing: 1px; }
        .footer-powered span { color: rgba(255,255,255,0.45); font-weight: 600; }

        .now-playing {
            background: rgba(255,255,255,0.05);
            border-radius: 10px;
            padding: 16px;
            margin-bottom: 20px;
            text-align: center;
        }
        .np-label { color: rgba(255,255,255,0.4); font-size: 0.75em; text-transform: uppercase; letter-spacing: 1px; margin-bottom: 6px; }
        .np-track { font-size: 1em; font-weight: 500; margin-bottom: 4px; word-break: break-word; min-height: 1.2em; }
        .np-state {
            display: inline-block;
            font-size: 0.75em;
            padding: 2px 10px;
            border-radius: 10px;
            background: rgba(255,255,255,0.1);
            color: rgba(255,255,255,0.6);
        }
        .np-state.playing { background: rgba(76,175,80,0.2); color: #81c784; }

        .controls {
            display: flex;
            justify-content: center;
            gap: 16px;
            margin-bottom: 24px;
        }
        .ctrl-btn {
            width: 48px; height: 48px;
            border-radius: 50%;
            border: 1px solid rgba(255,255,255,0.15);
            background: rgba(255,255,255,0.06);
            color: #fff;
            font-size: 1.2em;
            cursor: pointer;
            display: flex;
            align-items: center;
            justify-content: center;
            transition: all 0.2s;
        }
        .ctrl-btn:hover { background: rgba(124,77,255,0.3); border-color: #7c4dff; }
        .ctrl-btn.play-btn { width: 56px; height: 56px; font-size: 1.4em; background: rgba(124,77,255,0.2); border-color: #7c4dff; }
        .ctrl-btn.play-btn:hover { background: rgba(124,77,255,0.5); }

        .folder-section { margin-bottom: 20px; }
        .folder-section label { display: block; margin-bottom: 8px; color: rgba(255,255,255,0.6); font-size: 0.85em; }
        .folder-select {
            width: 100%; padding: 10px 12px; border-radius: 8px;
            background: rgba(255,255,255,0.06); border: 1px solid rgba(255,255,255,0.15);
            color: #fff; font-size: 0.95em; outline: none; cursor: pointer;
            transition: border-color 0.2s;
        }
        .folder-select:hover, .folder-select:focus { border-color: #7c4dff; }
        .folder-select option { background: #24243e; color: #fff; }

        .volume-section { margin-bottom: 20px; }
        .volume-section label { display: block; margin-bottom: 8px; color: rgba(255,255,255,0.6); font-size: 0.85em; }
        .volume-row { display: flex; align-items: center; gap: 12px; }
        .mute-btn {
            background: none; border: 1px solid rgba(255,255,255,0.15);
            color: rgba(255,255,255,0.6); border-radius: 6px;
            padding: 4px 8px; cursor: pointer; font-size: 0.9em;
            transition: all 0.2s;
        }
        .mute-btn:hover { border-color: #7c4dff; color: #7c4dff; }
        .mute-btn.muted { background: rgba(244,67,54,0.2); border-color: #f44336; color: #f44336; }
        input[type=range] {
            flex: 1; height: 6px; -webkit-appearance: none; appearance: none;
            background: rgba(255,255,255,0.2); border-radius: 3px; outline: none;
        }
        input[type=range]::-webkit-slider-thumb {
            -webkit-appearance: none; width: 20px; height: 20px;
            background: #7c4dff; border-radius: 50%; cursor: pointer;
        }
        .vol-num { min-width: 36px; text-align: right; font-weight: 600; }

        .sys-info { padding-top: 16px; border-top: 1px solid rgba(255,255,255,0.06); }
        .status-row {
            display: flex; justify-content: space-between; align-items: center;
            padding: 6px 0; font-size: 0.85em;
        }
        .status-label { color: rgba(255,255,255,0.5); }
        .status-value { font-weight: 500; }
        .back-link { color: #7c4dff; text-decoration: none; font-size: 0.8em; }
        .back-link:hover { text-decoration: underline; }
        .rescan-btn {
            background: none; border: none; color: #7c4dff;
            font-size: 0.8em; cursor: pointer; padding: 0;
            text-decoration: none; font-family: inherit;
        }
        .rescan-btn:hover { text-decoration: underline; }
        .rescan-btn:disabled { color: rgba(255,255,255,0.3); cursor: default; text-decoration: none; }
    </style>
</head>
<body>
    <div class="card">
        <div class="brand-header">
            <svg viewBox="0 0 960 280" fill="none" xmlns="http://www.w3.org/2000/svg">
  <defs>
    <linearGradient id="hGrad" x1="40" y1="40" x2="240" y2="240" gradientUnits="userSpaceOnUse">
      <stop offset="0" stop-color="#0F73C3"/>
      <stop offset="1" stop-color="#00B6F1"/>
    </linearGradient>
    <linearGradient id="hCore" x1="116" y1="116" x2="164" y2="164" gradientUnits="userSpaceOnUse">
      <stop offset="0" stop-color="#00B6F1"/>
      <stop offset="1" stop-color="#0F73C3"/>
    </linearGradient>
    <linearGradient id="hX" x1="0" y1="0" x2="1" y2="1">
      <stop offset="0" stop-color="#00B6F1"/>
      <stop offset="1" stop-color="#9013FE"/>
    </linearGradient>
  </defs>
  <g transform="translate(20,20) scale(0.46875)">
    <g stroke="url(#hGrad)" stroke-width="10" fill="none" opacity="0.85">
      <ellipse cx="256" cy="256" rx="150" ry="66"/>
      <ellipse cx="256" cy="256" rx="150" ry="66" transform="rotate(60 256 256)"/>
      <ellipse cx="256" cy="256" rx="150" ry="66" transform="rotate(120 256 256)"/>
    </g>
    <path d="M132 150 L86 256 L132 362" stroke="#00B6F1" stroke-width="26"
          stroke-linecap="round" stroke-linejoin="round" fill="none"/>
    <path d="M380 150 L426 256 L380 362" stroke="#00B6F1" stroke-width="26"
          stroke-linecap="round" stroke-linejoin="round" fill="none"/>
    <circle cx="256" cy="190" r="16" fill="#0F73C3"/>
    <circle cx="313" cy="289" r="16" fill="#0F73C3"/>
    <circle cx="199" cy="289" r="16" fill="#0F73C3"/>
    <circle cx="256" cy="256" r="50" fill="url(#hCore)"/>
    <circle cx="256" cy="256" r="20" fill="#FFFFFF"/>
  </g>
  <text x="320" y="128" font-family="'Exo 2', 'Exo2', sans-serif" font-size="78"
        font-weight="700" letter-spacing="2" fill="#FFFFFF">QUANTUM</text>
  <text x="320" y="212" font-family="'Exo 2', 'Exo2', sans-serif" font-size="78"
        font-weight="300" letter-spacing="10" fill="#B0B9BF">WORKS</text>
  <text x="700" y="212" font-family="'Exo 2', 'Exo2', sans-serif" font-size="92"
        font-weight="900" fill="url(#hX)">X</text>
</svg>
            <div class="brand-caption">QuantumSync Local &middot; v2</div>
        </div>
        <div class="subtitle" id="deviceName">Loading...</div>

        <div class="now-playing">
            <div class="np-label">Now Playing</div>
            <div class="np-track" id="trackName">--</div>
            <span class="np-state" id="playState">--</span>
        </div>

        <div class="controls">
            <button class="ctrl-btn" id="prevBtn" title="Previous">&#9198;</button>
            <button class="ctrl-btn play-btn" id="playBtn" title="Play/Pause">&#9654;</button>
            <button class="ctrl-btn" id="nextBtn" title="Next">&#9197;</button>
        </div>

        <div class="folder-section">
            <label>Music Folder</label>
            <select class="folder-select" id="folderSelect">
                <option value="">All Music</option>
            </select>
        </div>

        <div class="volume-section">
            <label>Volume</label>
            <div class="volume-row">
                <button class="mute-btn" id="muteBtn" title="Mute">&#128264;</button>
                <input type="range" id="volumeSlider" min="0" max="100" value="50">
                <span class="vol-num" id="volumeVal">50%</span>
            </div>
        </div>

        <div class="sys-info">
            <div class="status-row">
                <span class="status-label">Tracks</span>
                <span class="status-value" id="trackCount">--</span>
            </div>
            <div class="status-row">
                <span class="status-label">Library</span>
                <button class="rescan-btn" id="rescanBtn" title="Rescan music folder for new files">Rescan</button>
            </div>
            <div class="status-row">
                <span class="status-label">CPU</span>
                <span class="status-value" id="cpuInfo">--</span>
            </div>
            <div class="status-row">
                <span class="status-label">Memory</span>
                <span class="status-value" id="memInfo">--</span>
            </div>
            <div class="status-row">
                <span class="status-label">Uptime</span>
                <span class="status-value" id="uptimeInfo">--</span>
            </div>
            <div class="status-row">
                <span class="status-label"></span>
                <a href="/logs" class="back-link">View Logs</a>
            </div>
        </div>

        <div class="footer-brand">
            <!--CLIENT_LOGO-->
            <div class="footer-powered">Powered by <span>Quantum X Works</span></div>
        </div>
    </div>

    <script>
        const slider = document.getElementById('volumeSlider');
        const volVal = document.getElementById('volumeVal');
        const playBtn = document.getElementById('playBtn');
        const prevBtn = document.getElementById('prevBtn');
        const nextBtn = document.getElementById('nextBtn');
        const muteBtn = document.getElementById('muteBtn');
        const folderSelect = document.getElementById('folderSelect');
        const rescanBtn = document.getElementById('rescanBtn');
        let currentState = 'stopped';
        let isMuted = false;
        let currentFolder = '';

        function loadFolders() {
            fetch('/api/folders')
                .then(r => r.json())
                .then(data => {
                    const folders = data.folders || [];
                    currentFolder = data.current || '';
                    // Rebuild options only if the folder list changed
                    const existing = Array.from(folderSelect.options).slice(1).map(o => o.value);
                    if (existing.length !== folders.length ||
                        existing.some((v, i) => v !== folders[i])) {
                        folderSelect.innerHTML = '';
                        const allOpt = document.createElement('option');
                        allOpt.value = '';
                        allOpt.textContent = 'All Music';
                        folderSelect.appendChild(allOpt);
                        folders.forEach(f => {
                            const opt = document.createElement('option');
                            opt.value = f;
                            opt.textContent = f;
                            folderSelect.appendChild(opt);
                        });
                    }
                    if (document.activeElement !== folderSelect) {
                        folderSelect.value = currentFolder;
                        if (folderSelect.value !== currentFolder) folderSelect.selectedIndex = 0;
                    }
                })
                .catch(() => {});
        }

        function formatUptime(sec) {
            if (sec < 0) return '--';
            const h = Math.floor(sec / 3600);
            const m = Math.floor((sec % 3600) / 60);
            if (h > 0) return h + 'h ' + m + 'm';
            return m + 'm';
        }

        function updateStatus() {
            fetch('/api/status')
                .then(r => r.json())
                .then(data => {
                    document.getElementById('deviceName').textContent = data.deviceName || 'Local Player';
                    document.getElementById('trackName').textContent = data.track || 'No music loaded';

                    const stateEl = document.getElementById('playState');
                    currentState = data.state;
                    stateEl.textContent = data.state.charAt(0).toUpperCase() + data.state.slice(1);
                    stateEl.className = 'np-state' + (data.state === 'playing' ? ' playing' : '');

                    // Update play button icon
                    playBtn.innerHTML = (data.state === 'playing') ? '&#10074;&#10074;' : '&#9654;';

                    // Volume
                    isMuted = data.muted;
                    if (!isMuted) {
                        slider.value = data.volume;
                        volVal.textContent = data.volume + '%';
                    } else {
                        volVal.textContent = 'Muted';
                    }
                    muteBtn.className = 'mute-btn' + (isMuted ? ' muted' : '');
                    muteBtn.innerHTML = isMuted ? '&#128263;' : '&#128264;';

                    // Folder (keep dropdown in sync unless the user is busy with it)
                    if (data.folder !== undefined && document.activeElement !== folderSelect) {
                        currentFolder = data.folder;
                        folderSelect.value = currentFolder;
                        if (folderSelect.value !== currentFolder) folderSelect.selectedIndex = 0;
                    }

                    document.getElementById('trackCount').textContent = data.trackCount || '0';
                    document.getElementById('cpuInfo').textContent =
                        (data.cpuPercent >= 0 ? data.cpuPercent.toFixed(1) + '%' : '--');
                    document.getElementById('memInfo').textContent =
                        (data.memUsedMb >= 0 ? data.memUsedMb + ' / ' + (data.memTotalMb || '?') + ' MB' : '--');
                    document.getElementById('uptimeInfo').textContent = formatUptime(data.uptimeSec);
                })
                .catch(() => {});
        }

        // Volume slider
        let debounce = null;
        slider.addEventListener('input', function() {
            volVal.textContent = this.value + '%';
            clearTimeout(debounce);
            debounce = setTimeout(() => {
                fetch('/api/volume', {
                    method: 'POST',
                    headers: {'Content-Type': 'application/json'},
                    body: JSON.stringify({volume: parseInt(this.value)})
                });
            }, 100);
        });

        // Playback controls
        playBtn.addEventListener('click', () => {
            fetch('/api/playback', {
                method: 'POST',
                headers: {'Content-Type': 'application/json'},
                body: JSON.stringify({action: 'toggle'})
            }).then(() => setTimeout(updateStatus, 300));
        });

        prevBtn.addEventListener('click', () => {
            fetch('/api/playback', {
                method: 'POST',
                headers: {'Content-Type': 'application/json'},
                body: JSON.stringify({action: 'prev'})
            }).then(() => setTimeout(updateStatus, 300));
        });

        nextBtn.addEventListener('click', () => {
            fetch('/api/playback', {
                method: 'POST',
                headers: {'Content-Type': 'application/json'},
                body: JSON.stringify({action: 'next'})
            }).then(() => setTimeout(updateStatus, 300));
        });

        muteBtn.addEventListener('click', () => {
            fetch('/api/mute', {
                method: 'POST',
                headers: {'Content-Type': 'application/json'},
                body: JSON.stringify({muted: !isMuted})
            }).then(() => setTimeout(updateStatus, 300));
        });

        // Folder selector
        folderSelect.addEventListener('change', function() {
            const folder = this.value;
            this.blur();
            fetch('/api/folder', {
                method: 'POST',
                headers: {'Content-Type': 'application/json'},
                body: JSON.stringify({folder: folder})
            }).then(r => r.json())
              .then(data => {
                  if (data.error) {
                      // Folder vanished? Refresh the list.
                      loadFolders();
                  }
                  currentFolder = data.folder !== undefined ? data.folder : folder;
                  setTimeout(updateStatus, 400);
              })
              .catch(() => {});
        });

        // Library rescan
        rescanBtn.addEventListener('click', () => {
            rescanBtn.disabled = true;
            rescanBtn.textContent = 'Scanning...';
            fetch('/api/update', {method: 'POST'})
                .finally(() => {
                    setTimeout(() => {
                        rescanBtn.disabled = false;
                        rescanBtn.textContent = 'Rescan';
                        loadFolders();
                        updateStatus();
                    }, 3000);
                });
        });

        updateStatus();
        loadFolders();
        setInterval(updateStatus, 3000);
        setInterval(loadFolders, 15000);
    </script>
</body>
</html>)HTML";
}

// ─── Logs Page ─────────────────────────────────────────────────

std::string HttpServer::getLogsPage() {
    return R"HTML(<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>QuantumSync Local - Logs</title>
    <style>
        * { margin: 0; padding: 0; box-sizing: border-box; }
        body {
            font-family: 'Courier New', monospace;
            background: #1a1a2e;
            color: #e0e0e0;
            min-height: 100vh;
            padding: 20px;
        }
        .header {
            display: flex; justify-content: space-between; align-items: center;
            margin-bottom: 16px; flex-wrap: wrap; gap: 10px;
        }
        h1 { font-size: 1.2em; color: #7c4dff; }
        .controls { display: flex; gap: 10px; align-items: center; }
        .btn {
            padding: 6px 14px; border: 1px solid #7c4dff; background: transparent;
            color: #7c4dff; border-radius: 6px; cursor: pointer; font-size: 0.85em;
            transition: all 0.2s;
        }
        .btn:hover { background: #7c4dff; color: #fff; }
        .btn.active { background: #7c4dff; color: #fff; }
        .status-badge {
            font-size: 0.75em; padding: 3px 8px; border-radius: 10px;
            background: #2d2d44; color: #aaa;
        }
        .status-badge.live { background: #1b5e20; color: #81c784; }
        #logContainer {
            background: #0d0d1a; border-radius: 8px; padding: 16px;
            max-height: calc(100vh - 100px); overflow-y: auto;
            font-size: 0.8em; line-height: 1.6;
            border: 1px solid #2d2d44;
        }
        .log-line { white-space: pre-wrap; word-break: break-all; padding: 1px 0; }
        .log-line:hover { background: rgba(124, 77, 255, 0.1); }
        .back-link { color: #7c4dff; text-decoration: none; font-size: 0.9em; }
        .back-link:hover { text-decoration: underline; }
    </style>
</head>
<body>
    <div class="header">
        <div>
            <h1>QuantumSync Local Logs</h1>
            <a href="/" class="back-link">&larr; Back to Player</a>
        </div>
        <div class="controls">
            <span class="status-badge" id="statusBadge">Paused</span>
            <button class="btn active" id="autoRefreshBtn">Auto-refresh: ON</button>
            <button class="btn" id="refreshBtn">Refresh Now</button>
        </div>
    </div>
    <div id="logContainer">Loading...</div>
    <script>
        let autoRefresh = true;
        let timer = null;

        function fetchLogs() {
            fetch('/api/journal')
                .then(r => r.json())
                .then(data => {
                    const container = document.getElementById('logContainer');
                    const wasAtBottom = container.scrollHeight - container.scrollTop - container.clientHeight < 50;
                    container.textContent = '';
                    (data.lines || []).forEach(line => {
                        const div = document.createElement('div');
                        div.className = 'log-line';
                        div.textContent = line;
                        container.appendChild(div);
                    });
                    if (wasAtBottom) container.scrollTop = container.scrollHeight;
                    document.getElementById('statusBadge').textContent =
                        data.count + ' lines | ' + new Date().toLocaleTimeString();
                    document.getElementById('statusBadge').className =
                        'status-badge' + (autoRefresh ? ' live' : '');
                })
                .catch(() => {
                    document.getElementById('statusBadge').textContent = 'Error';
                    document.getElementById('statusBadge').className = 'status-badge';
                });
        }

        function toggleAutoRefresh() {
            autoRefresh = !autoRefresh;
            const btn = document.getElementById('autoRefreshBtn');
            btn.textContent = 'Auto-refresh: ' + (autoRefresh ? 'ON' : 'OFF');
            btn.className = 'btn' + (autoRefresh ? ' active' : '');
            if (autoRefresh) {
                fetchLogs();
                timer = setInterval(fetchLogs, 3000);
            } else {
                clearInterval(timer);
            }
        }

        document.getElementById('autoRefreshBtn').addEventListener('click', toggleAutoRefresh);
        document.getElementById('refreshBtn').addEventListener('click', fetchLogs);

        fetchLogs();
        timer = setInterval(fetchLogs, 3000);
    </script>
</body>
</html>)HTML";
}
