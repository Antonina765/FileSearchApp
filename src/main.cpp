#include <SFML/Graphics.hpp>
#include <iostream>
#include <filesystem>
#include <thread>
#include <vector>
#include <mutex>
#include <atomic>
#include <fstream>
#include <chrono>
#include <codecvt>
#include <locale>
#include <iomanip>

namespace fs = std::filesystem;

std::mutex resultMutex;
std::vector<std::string> searchResults;
std::atomic<bool> searching(false);
std::atomic<bool> cancelRequested(false);
std::atomic<int> threadCount(1);
sf::Text* countText = nullptr;

std::string homeDir = std::string(getenv("HOME") ? getenv("HOME") : "/Users/antoninaber/");

static void appendUtf8(std::string &out, uint32_t cp) {
    if (cp <= 0x7F) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | ((cp >> 6) & 0x1F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
        out.push_back(static_cast<char>(0xE0 | ((cp >> 12) & 0x0F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | ((cp >> 18) & 0x07)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

// UTF-8 → U32
static sf::String utf8ToU32(const std::string& utf8) {
    return sf::String::fromUtf8(utf8.begin(), utf8.end());
}

// U32 → UTF-8
static std::string u32ToUtf8(const sf::String& u32)
{
    std::string out;
    for (char32_t c : u32)
    {
        sf::Utf8::encode(c, std::back_inserter(out));
    }
    return out;
}

// Split a UTF-8 string into "visual" wrapped lines (attempt to break at '/' when possible)
static std::vector<std::string> wrapPath(const std::string& utf8, size_t maxWidth)
{
    std::vector<std::string> lines;

    sf::String u32 = sf::String::fromUtf8(utf8.begin(), utf8.end());
    sf::String current;

    for (size_t i = 0; i < u32.getSize(); ++i)
    {
        current += u32[i];
        if (current.getSize() >= maxWidth)
        {
            // Конвертируем sf::String -> std::string UTF-8
            sf::U8String u8line = current.toUtf8();
            std::string line(u8line.begin(), u8line.end());
            lines.push_back(line);
            current.clear();
        }
    }

    if (!current.isEmpty())
    {
        sf::U8String u8line = current.toUtf8();
        std::string line(u8line.begin(), u8line.end());
        lines.push_back(line);
    }

    return lines;
}

// Create log dir and open file (append)
static std::ofstream openLogFile() {
    fs::path logDir = fs::path(homeDir) / "Desktop" / "FileSearchApp" / "log";
    try {
        fs::create_directories(logDir);
    } catch (...) { /* ignore */ }
    fs::path logFile = logDir / "search_log.txt";
    std::ofstream f(logFile.string(), std::ios::app);
    return f;
}

static std::ofstream openThreadLog() {
    fs::path logDir = fs::path(homeDir) / "Desktop" / "FileSearchApp" / "log";
    try { fs::create_directories(logDir);} catch(...) {}
    fs::path logFile = logDir / "potoc.log";
    return std::ofstream(logFile.string(), std::ios::app);
}

// Timestamp string for log
static std::string timestampNow() {
    using namespace std::chrono;
    auto now = system_clock::now();
    std::time_t t = system_clock::to_time_t(now);
    std::tm tm{};
#if defined(_MSC_VER)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    std::ostringstream oss;
    oss << std::put_time(&tm, "%H:%M:%S");
    return oss.str();
}

sf::String toSf(const std::string& utf8)
{
    return sf::String::fromUtf8(utf8.begin(), utf8.end());
}

// ----------------- Search function -----------------
void searchFiles(const fs::path& dir, const std::string& filenamePart, bool searchEverywhere) {
    std::ofstream log = openLogFile();
    {
        std::lock_guard<std::mutex> lg(resultMutex);
        searchResults.clear();
    }
    log << "=== Search: " << timestampNow() << " ===\n";
    log << "Dir: " << dir.string() << "\n";
    log << "Query: " << filenamePart << "\n";

    try {
        std::vector<std::string> found;
        fs::path startDir = dir;
        if (searchEverywhere) startDir = fs::path(homeDir);

        // Collect files first
        std::vector<fs::path> allFiles;
        std::error_code ec;
        for (auto &entry : fs::recursive_directory_iterator(startDir, fs::directory_options::skip_permission_denied, ec)) {
            if (!entry.is_regular_file()) continue;
            allFiles.push_back(entry.path());
        }

        // Parallel processing
        std::vector<std::thread> workers;
        std::mutex foundMutex;

        auto workerFunc = [&](int start, int end) {
            for (int i = start; i < end; ++i) {
                if (cancelRequested.load()) return;

                auto toLower = [](const std::string &s) {
                    std::string out;
                    for (unsigned char c : s) out += std::tolower(c);
                    return out;
                };

                const auto &path = allFiles[i];
                std::string name = path.filename().string();

                // сравнение без учёта регистра
                if (toLower(name).find(toLower(filenamePart)) != std::string::npos) {
                    {
                        std::lock_guard<std::mutex> g(foundMutex);
                        found.push_back(path.string());
                    }

                    // Log this thread’s work
                    {
                    static std::mutex tlogMutex;
                    std::lock_guard<std::mutex> lg(tlogMutex);
                    auto tlog = openThreadLog();
                    tlog << timestampNow()
                        << " | thread=" << std::this_thread::get_id()
                        << " processed: " << path.string()
                        << "\n";
                    }
                }
            }
        };

        // Split work
        int total = allFiles.size();
        int chunk = std::max(1, total / threadCount.load());
        int start = 0;

        for (int i = 0; i < threadCount; ++i) {
            int end = std::min(start + chunk, total);
            workers.emplace_back(workerFunc, start, end);
            start = end;
        }

        // join workers
        for (auto &t : workers) t.join();

        {
            std::lock_guard<std::mutex> lg(resultMutex);
            // Вставляем количество найденных файлов в начало

            searchResults = found;
            std::cout << "Found files: " << found.size() << std::endl;

            if (found.empty()) {
                // not found in startDir
                if (!searchEverywhere) {
                    searchResults.clear();
                    searchResults.push_back("File not found in this directory");
                    // now search entire home for other occurrences
                    std::vector<std::string> globalFound;
                    for (auto& entry : fs::recursive_directory_iterator(homeDir)) {
                        if (cancelRequested.load()) break;
                        if (entry.is_regular_file()) {
                            std::string name = entry.path().filename().string();
                            if (name.find(filenamePart) != std::string::npos) {
                                globalFound.push_back(entry.path().string());
                                log << "Found elsewhere: " << entry.path().string() << "\n";
                            }
                        }
                    }
                    if (!globalFound.empty()) {
                        searchResults.push_back("Found elsewhere:");
                        searchResults.insert(searchResults.end(), globalFound.begin(), globalFound.end());
                    }
                } else {
                    searchResults = { "File not found" };
                }
            } else {
                // return all found
                searchResults = found;
                log << "=== End Search ===\n\n";
            }
        }
    } catch (const std::exception& e) {
        std::lock_guard<std::mutex> lg(resultMutex);
        searchResults = { std::string("Error: ") + e.what() };
        log << "Error: " << e.what() << "\n";
    }
    searching = false;
    log << "=== End Search ===\n\n";
}

static size_t cursorIndexFromMouseX(const sf::Text &textPrototype, const std::string &utf8str, float mouseX) {
    // local copy text proto
    sf::Text tmp(textPrototype);
    // iterate over codepoint positions (but use byte index for std::string)
    // we will iterate by byte but set tmp string to prefix bytes and compare width
    size_t bytes = 0;
    size_t chosen = 0;
    float baseLeft = tmp.getGlobalBounds().position.x;
    for (size_t i = 0; i <= utf8str.size(); ++i) {
        tmp.setString(utf8str.substr(0, i));
        float w = tmp.getGlobalBounds().size.x;
        float px = baseLeft + w;
        if (mouseX < px) {
            chosen = i;
            break;
        }
        chosen = i;
    }
    // chosen is byte index; clamp
    if (chosen > utf8str.size()) chosen = utf8str.size();
    return chosen;
}

int main() {

    std::cout << "Enter number of threads: ";
    int tc;
    std::cin >> tc;
    if (tc > 0) threadCount = tc;
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n'); 

     setlocale(LC_ALL, "");

    try {
        std::locale loc(""); // user's default locale (may be UTF-8)
        std::locale::global(loc);
        std::wcout.imbue(loc);
        std::cout.imbue(loc);
    } catch (...) {
    }

    // create log dir early
    try {
        fs::create_directories(fs::path(homeDir) / "Desktop" / "FileSearchApp" / "log");
    } catch (...) {}

    // Create fullscreen window 
    sf::RenderWindow window(sf::VideoMode::getDesktopMode(), "File Search App", sf::Style::Default, sf::State::Fullscreen);
    window.setFramerateLimit(60);

    // Load font (use Unicode-capable font)
    sf::Font font;
    std::vector<std::string> fontPaths = {
        "assets/DejaVuSans.ttf"
    };
    bool fontLoaded = false;
    for (auto &p : fontPaths) {
        if (fs::exists(p) && font.openFromFile(p)) {
            std::cout << "Loaded font: " << p << std::endl;
            fontLoaded = true;
            break;
        }
    }
    if (!fontLoaded) {
        std::cerr << "Failed to load any font.";
        return 1;
    }

    std::cout << font.getInfo().family << std::endl;

    // UI elements
    sf::Text title(font, "File Search App", 48);
    title.setPosition({50.f, 20.f});

    sf::RectangleShape dirBox({800.f, 50.f});
    dirBox.setPosition({50.f, 120.f});
    dirBox.setFillColor(sf::Color(50,50,50));
    sf::Text dirText(font, "", 24);
    dirText.setPosition({60.f, 130.f});

    sf::RectangleShape fileBox({800.f, 50.f});
    fileBox.setPosition({50.f, 200.f});
    fileBox.setFillColor(sf::Color(50,50,50));
    sf::Text fileText(font, "", 24);
    fileText.setPosition({60.f, 210.f});

    sf::RectangleShape searchButton({150.f,50.f});
    searchButton.setPosition({50.f,280.f});
    searchButton.setFillColor(sf::Color(70,130,180));
    sf::Text searchLabel(font, "Search", 24);
    searchLabel.setPosition({90.f, 290.f});

    sf::RectangleShape cancelButton({150.f,50.f});
    cancelButton.setPosition({220.f,280.f});
    cancelButton.setFillColor(sf::Color(180,70,70));
    sf::Text cancelLabel(font, "Cancel", 24);
    cancelLabel.setPosition({260.f, 290.f});

    // results drawn as separate lines (we will create sf::Text per line)
    float resultsStartY = 360.f;
    float lineSpacing = 26.f;
    float scrollOffset = 0.f;

    // input state
    bool typingDir = true;
    std::string dirInput, fileInput;
    size_t dirCursorPos = 0, fileCursorPos = 0;

    sf::RectangleShape cursor({2.f, 28.f});
    cursor.setFillColor(sf::Color::White);
    bool cursorVisible = true;
    sf::Clock cursorTimer;

    std::thread searchThread;

    auto updateCursorPosition = [&]() {
        const sf::Text& activeText = typingDir ? dirText : fileText;
        const std::string& input = typingDir ? dirInput : fileInput;
        size_t cursorPos = typingDir ? dirCursorPos : fileCursorPos;

        sf::Text temp(activeText);

        sf::String prefixU32 = utf8ToU32(input);
        prefixU32 = prefixU32.substring(0, cursorPos);

        temp.setString(prefixU32);

        sf::FloatRect bounds = temp.getGlobalBounds();
        cursor.setPosition({ bounds.position.x + bounds.size.x + 2.f, bounds.position.y });
    };

    updateCursorPosition();

    // main loop
    while (window.isOpen()) {

        while (auto event = window.pollEvent()) {
            if (event->is<sf::Event::Closed>())
                window.close();

            // key pressed
            if (auto kp = event->getIf<sf::Event::KeyPressed>()) {
                if (kp->code == sf::Keyboard::Key::Escape) {
                    window.close();
                } else if (kp->code == sf::Keyboard::Key::Tab) {
                    typingDir = !typingDir;
                } else if (kp->code == sf::Keyboard::Key::Left) {
                    if (typingDir) { if (dirCursorPos > 0) dirCursorPos--; }
                    else { if (fileCursorPos > 0) fileCursorPos--; }
                } else if (kp->code == sf::Keyboard::Key::Right) {
                    if (typingDir) { 
                        sf::String u32 = utf8ToU32(dirInput);
                        if (dirCursorPos < u32.getSize()) dirCursorPos++; }
                    else {
                        sf::String u32 = utf8ToU32(dirInput);
                        if (fileCursorPos < u32.getSize()) fileCursorPos++; }
                } else if (kp->code == sf::Keyboard::Key::Enter) {
                    // start search (same behavior as click Search)
                    if (!searching) {
                        searchResults.clear();
                        // determine startDir and searchEverywhere
                        bool searchEverywhere = false;
                        fs::path startDir;
                        if (dirInput.empty()) {
                            searchEverywhere = true;
                            startDir = fs::path(homeDir);
                        } else {
                            fs::path candidate = fs::path(homeDir) / fs::path(dirInput);
                            if (fs::exists(candidate) && fs::is_directory(candidate)) {
                                startDir = candidate;
                            } else if (fs::path(dirInput).is_absolute() && fs::exists(dirInput)) {
                                startDir = fs::path(dirInput);
                            } else {
                                // treat as relative to home but if doesn't exist -> still search everywhere (user entered a dir which doesn't exist)
                                startDir = fs::path(homeDir);
                                searchEverywhere = true;
                            }
                        }
                        searching = true;
                        cancelRequested = false;
                        if (searchThread.joinable()) searchThread.join();
                        searchThread = std::thread(searchFiles, startDir, fileInput, searchEverywhere);
                    }
                }
            }

            // text entered (handles unicode codepoints)
            if (auto te = event->getIf<sf::Event::TextEntered>()) {
                if (!searching) {
                    uint32_t code = te->unicode;
                    
                    if (code < 32 && code != 8 && code != 13) continue;

                    std::string &input = typingDir ? dirInput : fileInput;
                    size_t &cursorPos = typingDir ? dirCursorPos : fileCursorPos;

                    sf::String u32 = utf8ToU32(input);

                    if (code == 8) { // backspace
                        if (cursorPos > 0) {
                            u32.erase(cursorPos - 1);
                            cursorPos--;
                        }
                    } else if (code == 13 || code == '\r') {
                        // Enter - start search
                        if (!searching) {
                            searchResults.clear();
                            bool searchEverywhere = false;
                            fs::path startDir;
                            if (dirInput.empty()) {
                                searchEverywhere = true;
                                startDir = fs::path(homeDir);
                            } else {
                                fs::path candidate = fs::path(homeDir) / fs::path(dirInput);
                                if (fs::exists(candidate) && fs::is_directory(candidate)) {
                                    startDir = candidate;
                                } else if (fs::path(dirInput).is_absolute() && fs::exists(dirInput)) {
                                    startDir = fs::path(dirInput);
                                } else {
                                    startDir = fs::path(homeDir);
                                    searchEverywhere = true;
                                }
                            }
                            searching = true;
                            cancelRequested = false;
                            if (searchThread.joinable()) searchThread.join();
                            searchThread = std::thread(searchFiles, startDir, fileInput, searchEverywhere);
                        }
                    }  else {
                        u32.insert(cursorPos, static_cast<char32_t>(code));
                        cursorPos++;
                    }

                    // Обратно в UTF-8
                    input = u32ToUtf8(u32);

                    // Обновить отображение
                    if (typingDir)
                        dirText.setString(u32);
                    else
                        fileText.setString(u32);
                    
                }
            }

            // mouse pressed: focus fields, click to set cursor, click buttons
            if (auto mp = event->getIf<sf::Event::MouseButtonPressed>()) {
                sf::Vector2f pos((float)mp->position.x, (float)mp->position.y);

                if (dirBox.getGlobalBounds().contains(pos)) {
                    typingDir = true;
                    // set cursor position based on click X
                    size_t idx = cursorIndexFromMouseX(dirText, dirInput, pos.x);
                    dirCursorPos = idx;
                } else if (fileBox.getGlobalBounds().contains(pos)) {
                    typingDir = false;
                    size_t idx = cursorIndexFromMouseX(fileText, fileInput, pos.x);
                    fileCursorPos = idx;
                }

                if (searchButton.getGlobalBounds().contains(pos) && !searching) {
                    // start search
                    bool searchEverywhere = false;
                    fs::path startDir;
                    if (dirInput.empty()) {
                        searchEverywhere = true;
                        startDir = fs::path(homeDir);
                    } else {
                        fs::path candidate = fs::path(homeDir) / fs::path(dirInput);
                        if (fs::exists(candidate) && fs::is_directory(candidate)) {
                            startDir = candidate;
                        } else if (fs::path(dirInput).is_absolute() && fs::exists(dirInput)) {
                            startDir = fs::path(dirInput);
                        } else {
                            startDir = fs::path(homeDir);
                            searchEverywhere = true;
                        }
                    }
                    searchResults.clear();
                    searching = true;
                    cancelRequested = false;
                    if (searchThread.joinable()) searchThread.join();
                    searchThread = std::thread(searchFiles, startDir, fileInput, searchEverywhere);
                }

                if (cancelButton.getGlobalBounds().contains(pos)) {
                    cancelRequested = true;
                    searching = false;
                    {
                        std::lock_guard<std::mutex> lock(resultMutex);
                        searchResults.clear();
                    }
                    dirInput.clear(); fileInput.clear();
                    dirCursorPos = fileCursorPos = 0;
                    dirText.setString("");
                    fileText.setString("");
                }
            }

            // mouse wheel for scrolling results
            if (auto wheel = event->getIf<sf::Event::MouseWheelScrolled>()) {
                float maxHeight = 0.f;
                {
                    std::lock_guard<std::mutex> lock(resultMutex);
                    const size_t wrapChars = 145;
                    for (auto &s : searchResults) {
                        auto lines = wrapPath(s, wrapChars);
                        maxHeight += lines.size() * 28.f;
                    }
                }
                float visibleHeight = window.getSize().y - resultsStartY - 100.f;
                float maxScroll = std::max(0.f, maxHeight - visibleHeight);

                scrollOffset += -wheel->delta * 30.f; // прокрутка вверх/вниз
                if (scrollOffset < 0.f) scrollOffset = 0.f;
                if (scrollOffset > maxScroll) scrollOffset = maxScroll;
            }

        } // end event loop

        // clamp scrollOffset (optional)
        // подсчет максимальной высоты результатов
        float maxHeight = 0.f;
        {
            std::lock_guard<std::mutex> lock(resultMutex);
            const size_t wrapChars = 145;
            for (auto &s : searchResults) {
                auto lines = wrapPath(s, wrapChars);
                maxHeight += lines.size() * 28.f;
            }
        }
        float visibleHeight = window.getSize().y - resultsStartY - 50.f;
        float maxScroll = std::max(0.f, maxHeight - visibleHeight);

        // blink cursor
        if (cursorTimer.getElapsedTime().asSeconds() > 0.5f) {
            cursorVisible = !cursorVisible;
            cursorTimer.restart();
        }

        // prepare result lines
        std::vector<sf::Text> resultLines;
        {
            std::lock_guard<std::mutex> lg(resultMutex);
            // for each string in searchResults, wrap it into multiple visual lines
            const size_t wrapChars = 145; // approximate wrap width in characters (adjust if needed)
            for (const auto &s : searchResults) {
                auto lines = wrapPath(s, wrapChars);
                if (lines.empty()) lines = {""};
                for (auto &ln : lines) {
                    sf::Text t(font);
                    t.setFont(font);
                    t.setCharacterSize(20);
                    t.setString(toSf(ln));
                    t.setPosition({50.f, resultsStartY + resultLines.size() * lineSpacing + scrollOffset});
                    resultLines.push_back(t);
                }
            }
        }

        // update displayed inputs and cursor position
        dirText.setString(utf8ToU32(dirInput));
        fileText.setString(utf8ToU32(fileInput));

        // cursor position: position at text width of prefix
        if (typingDir) {
            sf::Text temp(dirText);
            temp.setString(dirInput.substr(0, dirCursorPos));
            sf::FloatRect b = temp.getGlobalBounds();
            cursor.setPosition({b.position.x + b.size.x + 2.f, b.position.y});
        } else {
            sf::Text temp(fileText);
            temp.setString(fileInput.substr(0, fileCursorPos));
            sf::FloatRect b = temp.getGlobalBounds();
            cursor.setPosition({b.position.x + b.size.x + 2.f, b.position.y});
        }

        // draw
        window.clear(sf::Color(30,30,30));
        window.draw(title);
        window.draw(dirBox);
        window.draw(dirText);
        window.draw(fileBox);
        window.draw(fileText);
        window.draw(searchButton);
        window.draw(searchLabel);
        window.draw(cancelButton);
        window.draw(cancelLabel);

        // draw result lines (we already set their positions)
        //for (auto &t : resultLines) window.draw(t);

        // Draw results below buttons, with scrolling
        float y = resultsStartY - scrollOffset;
        {
            std::lock_guard<std::mutex> lock(resultMutex);
            const size_t wrapChars = 145;
            for (const auto& s : searchResults) {
                auto lines = wrapPath(s, wrapChars);
                for (auto &ln : lines) {
                    sf::Text t(font, utf8ToU32(ln), 22);
                    t.setPosition({ 50.f, y });
                    t.setFillColor(sf::Color::White);

                    if (y > 330.f && y < window.getSize().y - 50.f) // область под кнопками
                        window.draw(t);

                    y += 28.f;
                }
            }
        }
        // draw cursor
        if (cursorVisible) window.draw(cursor);

        window.display();
    }

    // cleanup
    cancelRequested = true;
    if (searchThread.joinable()) searchThread.join();

    return 0;
}
