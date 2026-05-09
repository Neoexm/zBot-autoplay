#ifndef _autobot_debug_hpp
#define _autobot_debug_hpp

#include <Geode/Geode.hpp>

#include <cstdint>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>

using namespace geode::prelude;

namespace autobot {

class AutoBotDebug {
public:
    static AutoBotDebug* get() {
        static AutoBotDebug* instance = new AutoBotDebug();
        return instance;
    }

    std::filesystem::path getLogDirectory() const {
        return resolvePreferredLogDirectory();
    }

    std::filesystem::path getLatestLogPath() const {
        return getLogDirectory() / "autobot-latest.log";
    }

    void beginSession(std::string const& reason) {
        openFresh();
        auto path = getLatestLogPath();
        log::info("[AutoBot] Debug log path | {}", path.string());
        logEvent("log-path", path.string());
        logEvent("session-start", reason);
    }

    void endSession(std::string const& reason) {
        if (!m_stream.is_open()) return;
        logEvent("session-end", reason);
        m_stream.flush();
        m_stream.close();
    }

    void logEvent(std::string const& tag, std::string const& message) {
        ensureOpen();
        if (!m_stream.is_open()) return;

        m_stream << timestamp() << " | " << tag << " | " << message << '\n';
        if ((++m_lineCount % 32u) == 0u) {
            m_stream.flush();
        }
    }

    void flush() {
        if (m_stream.is_open()) {
            m_stream.flush();
        }
    }

    void bufferTickEvent(std::string const& message) {
        m_recentTickEvents.push_back(message);
        while (m_recentTickEvents.size() > kRecentTickBufferCapacity) {
            m_recentTickEvents.pop_front();
        }
    }

    void flushBufferedTickEvents(std::string const& reason) {
        if (m_recentTickEvents.empty()) return;
        logEvent("tick-buffer-start", reason);
        for (auto const& line : m_recentTickEvents) {
            logEvent("tick-buffer", line);
        }
        logEvent("tick-buffer-end", reason);
        m_recentTickEvents.clear();
    }

    void clearBufferedTickEvents() {
        m_recentTickEvents.clear();
    }

private:
    static constexpr std::size_t kRecentTickBufferCapacity = 480;
    std::ofstream m_stream;
    std::uint64_t m_lineCount = 0;
    std::deque<std::string> m_recentTickEvents;

    static std::filesystem::path ensureDirectory(std::filesystem::path const& dir) {
        std::error_code ec;
        if (!dir.empty() && !std::filesystem::exists(dir, ec)) {
            std::filesystem::create_directories(dir, ec);
        }
        return dir;
    }

    std::filesystem::path resolveWorkspaceSrcDirectory() const {
#ifdef AUTOBOT_WORKSPACE_SRC_DIR
        std::filesystem::path configured = AUTOBOT_WORKSPACE_SRC_DIR;
        if (!configured.empty() && std::filesystem::exists(configured)) {
            return configured;
        }
#endif

        std::filesystem::path sourcePath = std::filesystem::path(__FILE__);
        std::error_code ec;
        if (std::filesystem::exists(sourcePath, ec)) {
            sourcePath = std::filesystem::weakly_canonical(sourcePath, ec);
        }

        auto current = sourcePath.parent_path();
        for (int i = 0; i < 6 && !current.empty(); ++i) {
            if (current.filename() == "src" && std::filesystem::exists(current)) {
                return current;
            }
            current = current.parent_path();
        }

        auto directSrc = sourcePath.parent_path().parent_path();
        if (!directSrc.empty() && directSrc.filename() == "src" && std::filesystem::exists(directSrc)) {
            return directSrc;
        }

        return {};
    }

    std::filesystem::path resolvePreferredLogDirectory() const {
        auto workspaceSrc = resolveWorkspaceSrcDirectory();
        if (!workspaceSrc.empty() && std::filesystem::exists(workspaceSrc)) {
            return ensureDirectory(workspaceSrc / "autobot-debug");
        }

        return ensureDirectory(Mod::get()->getSaveDir() / "autobot-debug");
    }

    void ensureOpen() {
        if (m_stream.is_open()) return;
        openAppend();
    }

    void openFresh() {
        auto path = getLatestLogPath();
        if (m_stream.is_open()) {
            m_stream.close();
        }
        m_stream.open(path, std::ios::out | std::ios::trunc);
        m_lineCount = 0;
        m_recentTickEvents.clear();
    }

    void openAppend() {
        auto path = getLatestLogPath();
        if (m_stream.is_open()) return;
        m_stream.open(path, std::ios::out | std::ios::app);
    }

    std::string timestamp() const {
        auto now = std::time(nullptr);
        std::tm localTime{};
#ifdef _WIN32
        localtime_s(&localTime, &now);
#else
        localtime_r(&now, &localTime);
#endif

        std::ostringstream out;
        out << std::put_time(&localTime, "%Y-%m-%d %H:%M:%S");
        return out.str();
    }
};

} // namespace autobot

#endif
