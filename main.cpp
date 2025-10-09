// spellingbee_one_shot.cpp (always window, user-driven start, robust pause/retry, detach Chrome, no gotos)
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <stdexcept>
#include <sstream>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

using json = nlohmann::json;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

struct HttpResponse { long status = 0; std::string body; };
static size_t WriteToString(void* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* out = static_cast<std::string*>(userdata);
    out->append(static_cast<char*>(ptr), size * nmemb);
    return size * nmemb;
}

struct CurlSession {
    CURL* curl = nullptr;
    struct curl_slist* common_headers = nullptr;
    CurlSession() {
        curl = curl_easy_init();
        if (!curl) throw std::runtime_error("curl_easy_init failed");
        curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
        curl_easy_setopt(curl, CURLOPT_TCP_KEEPIDLE, 30L);
        curl_easy_setopt(curl, CURLOPT_TCP_KEEPINTVL, 15L);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteToString);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
        common_headers = curl_slist_append(common_headers, "Content-Type: application/json");
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, common_headers);
    }
    ~CurlSession() {
        if (common_headers) curl_slist_free_all(common_headers);
        if (curl) curl_easy_cleanup(curl);
    }
    HttpResponse request(const std::string& method, const std::string& url, const std::string& payload = "") {
        HttpResponse resp; resp.body.clear();
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp.body);
        curl_easy_setopt(curl, CURLOPT_HTTPGET, 0L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, nullptr);
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, nullptr);

        if (method == "GET") {
            curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
        } else if (method == "POST") {
            curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "POST");
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());
        } else if (method == "DELETE") {
            curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
            if (!payload.empty()) curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());
            else curl_easy_setopt(curl, CURLOPT_POSTFIELDS, "");
        } else {
            curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method.c_str());
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());
        }
        CURLcode code = curl_easy_perform(curl);
        if (code != CURLE_OK) {
            std::ostringstream oss; oss << "CURL error: " << curl_easy_strerror(code);
            throw std::runtime_error(oss.str());
        }
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &resp.status);
        return resp;
    }
    json request_json(const std::string& method, const std::string& url, const json& payload = {}) {
        auto resp = request(method, url, payload.is_null() ? "" : payload.dump());
        if (resp.status < 200 || resp.status >= 300) {
            std::ostringstream oss; oss << "HTTP " << resp.status << " from " << url << " body: " << resp.body;
            throw std::runtime_error(oss.str());
        }
        return resp.body.empty() ? json() : json::parse(resp.body);
    }
};

struct WD {
    std::string base = "http://localhost:9515";
    std::string sessionId;
    CurlSession* cs = nullptr;
    static constexpr const char* kElemKey = "element-6066-11e4-a52e-4f735466cecf";
    explicit WD(CurlSession& s) : cs(&s) {}

    json new_session() {
        // Always windowed + keep Chrome open when we exit (detach)
        json args = {"--disable-features=PaintHolding",
                     "--disable-extensions",
                     "--mute-audio",
                     "--remote-allow-origins=*"}; // Needed for ChromeDriver 111+ handshake
        json chromeOptions = {{"args", args}, {"detach", true}};
        json caps = {{"capabilities", {{"alwaysMatch", {{"browserName","chrome"},{"goog:chromeOptions",chromeOptions}}}}}};
        auto j = cs->request_json("POST", base + "/session", caps);
        sessionId = j.at("value").at("sessionId").get<std::string>();
        return j;
    }
    void delete_session() {
        if (!sessionId.empty()) {
            cs->request_json("DELETE", base + "/session/" + sessionId);
            sessionId.clear();
        }
    }
    void navigate(const std::string& url) {
        cs->request_json("POST", base + "/session/" + sessionId + "/url", json{{"url", url}});
    }
    void set_window_size(int w, int h) {
        cs->request_json("POST", base + "/session/" + sessionId + "/window/rect", json{{"width", w},{"height", h}});
    }
    std::string find_element_id_css(const std::string& css) {
        auto j = cs->request_json("POST", base + "/session/" + sessionId + "/element",
                                  json{{"using","css selector"},{"value",css}});
        return j.at("value").at(kElemKey).get<std::string>();
    }
    std::string get_element_text(const std::string& elemId) {
        auto j = cs->request_json("GET", base + "/session/" + sessionId + "/element/" + elemId + "/text");
        return j.at("value").is_null() ? std::string() : j.at("value").get<std::string>();
    }
    std::string get_element_attribute(const std::string& elemId, const std::string& attribute) {
        auto j = cs->request_json("GET", base + "/session/" + sessionId + "/element/" + elemId + "/attribute/" + attribute);
        return j.at("value").is_null() ? std::string() : j.at("value").get<std::string>();
    }
    void click_element(const std::string& elemId) {
        cs->request_json("POST", base + "/session/" + sessionId + "/element/" + elemId + "/click", json::object());
    }
    void send_all_words_as_keys(const std::vector<std::string>& words_upper) {
        json actions = json::array();
        json keyActions = {{"type","key"},{"id","keyboard"},{"actions", json::array()}};
        auto& seq = keyActions["actions"];
        for (const auto& w : words_upper) {
            for (char c : w) {
                char up = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                std::string s(1, up);
                seq.push_back({{"type","keyDown"},{"value",s}});
                seq.push_back({{"type","keyUp"},{"value",s}});
            }
            seq.push_back({{"type","keyDown"},{"value","\uE007"}});
            seq.push_back({{"type","keyUp"},{"value","\uE007"}});
        }
        actions.push_back(keyActions);
        cs->request_json("POST", base + "/session/" + sessionId + "/actions", json{{"actions", actions}});
    }
};

// ---------- Pause/Retry Utilities ----------
enum class StepResult { OK, SKIP, QUIT };

static std::string prompt_line(const std::string& prompt) {
    std::cout << prompt << std::flush;
    std::string s; std::getline(std::cin, s); return s;
}

static void pause_banner(const std::string& reason) {
    std::cout << "\n=== PAUSED ======================================\n"
              << reason << "\n"
              << "Fix the browser if needed, then:\n"
              << "  [Enter] retry  |  'skip' to skip  |  'quit' to stop workflow\n"
              << "=================================================\n";
}

template <typename Fn>
StepResult retry_with_pause(const char* what, Fn fn) {
    for (;;) {
        try {
            fn();
            return StepResult::OK;
        } catch (const std::exception& e) {
            std::cerr << "[WARN] " << what << " failed: " << e.what() << "\n";
            pause_banner(std::string("Step: ") + what);
            auto cmd = prompt_line("> ");
            if (cmd == "quit" || cmd == "q") return StepResult::QUIT;
            if (cmd == "skip" || cmd == "s")  return StepResult::SKIP;
            // else retry
        } catch (...) {
            std::cerr << "[WARN] " << what << " failed with unknown error.\n";
            pause_banner(std::string("Step: ") + what);
            auto cmd = prompt_line("> ");
            if (cmd == "quit" || cmd == "q") return StepResult::QUIT;
            if (cmd == "skip" || cmd == "s")  return StepResult::SKIP;
        }
    }
}

static inline void to_upper_inplace(std::string& s) {
    for (auto& c : s) c = std::toupper(static_cast<unsigned char>(c));
}

static inline std::string to_lower_copy(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

static inline std::string trim_copy(const std::string& s) {
    const auto not_space = [](unsigned char ch) { return !std::isspace(ch); };
    auto begin = std::find_if(s.begin(), s.end(), not_space);
    auto end = std::find_if(s.rbegin(), s.rend(), not_space).base();
    if (begin >= end) return std::string();
    return std::string(begin, end);
}

static bool has_class_token(const std::string& classes_lower, const std::string& token) {
    std::istringstream iss(classes_lower);
    std::string part;
    while (iss >> part) {
        if (part == token) return true;
    }
    return false;
}

static std::string normalize_letters(const std::string& raw) {
    std::string result;
    result.reserve(raw.size());
    for (char ch : raw) {
        if (std::isspace(static_cast<unsigned char>(ch))) continue;
        if (!std::isalpha(static_cast<unsigned char>(ch))) {
            std::ostringstream oss;
            oss << "letters may only contain alphabetic characters (got '" << ch << "')";
            throw std::runtime_error(oss.str());
        }
        result.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    return result;
}

struct WordDictionaries {
    std::set<std::string> short_words;
    std::set<std::string> medium_words;
    std::set<std::string> extended_words;
    std::set<std::string> massive_words;
};

static const std::array<char, 6> kVowels = {'a', 'e', 'i', 'o', 'u', 'y'};

static bool contains_vowel(const std::string& word) {
    for (char c : word) {
        char lower = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (std::find(kVowels.begin(), kVowels.end(), lower) != kVowels.end()) {
            return true;
        }
    }
    return false;
}

static void load_word_file(const fs::path& file, std::set<std::string>& out) {
    std::ifstream in(file);
    if (!in) {
        std::ostringstream oss;
        oss << "failed to open dictionary file: " << file.string();
        throw std::runtime_error(oss.str());
    }
    std::string line;
    while (std::getline(in, line)) {
        auto trimmed = trim_copy(line);
        if (trimmed.empty()) continue;
        out.insert(to_lower_copy(trimmed));
    }
}

static WordDictionaries load_word_dictionaries(const fs::path& base_dir) {
    WordDictionaries dictionaries;
    load_word_file(base_dir / "wordlist.txt", dictionaries.short_words);
    load_word_file(base_dir / "wiki-100k.txt", dictionaries.medium_words);
    load_word_file(base_dir / "words.txt", dictionaries.extended_words);
    load_word_file(base_dir / "words400k.txt", dictionaries.extended_words);
    load_word_file(base_dir / "wlist_match1.txt", dictionaries.massive_words);
    return dictionaries;
}

static std::vector<std::string> find_valid_words(const std::set<std::string>& dictionary,
                                                 const std::string& letters) {
    if (letters.size() < 1) {
        throw std::runtime_error("letters input is empty");
    }
    const char required = letters.back();
    std::vector<std::string> results;
    results.reserve(dictionary.size());
    for (const auto& word : dictionary) {
        if (word.size() < 4) continue;
        bool illegal = false;
        for (char c : word) {
            if (letters.find(c) == std::string::npos) {
                illegal = true;
                break;
            }
        }
        if (illegal) continue;
        if (word.find(required) == std::string::npos) continue;
        if (!contains_vowel(word)) continue;
        results.push_back(word);
    }
    return results;
}

static fs::path find_default_dictionary_dir() {
    const std::array<fs::path, 3> candidates = {
        fs::path("WordListerApp/target/classes/com/uestechnology"),
        fs::path("WordListerApp/src/main/resources/com/uestechnology"),
        fs::path("WordListerApp/extracted/com/uestechnology")
    };
    for (const auto& candidate : candidates) {
        if (fs::exists(candidate) && fs::is_directory(candidate)) {
            return candidate;
        }
    }
    return {};
}

static void dump_cell_debug(const std::vector<std::tuple<char, bool, std::string, std::string>>& cells) {
    std::cerr << "[DEBUG] Hive cell attributes:\n";
    int idx = 0;
    for (const auto& entry : cells) {
        const auto letter = std::get<0>(entry);
        const auto is_center = std::get<1>(entry);
        const auto& classes = std::get<2>(entry);
        const auto& aria = std::get<3>(entry);
        std::cerr << "  #" << (idx + 1) << " letter='" << letter
                  << "' center=" << (is_center ? "true" : "false")
                  << " classes='" << classes << "' aria='" << aria << "'\n";
        ++idx;
    }
}

static std::string read_letters_from_board(WD& wd) {
    std::vector<std::tuple<char, bool, std::string, std::string>> cells;
    cells.reserve(7);
    for (int idx = 1; idx <= 7; ++idx) {
        std::ostringstream css;
        css << ".hive-cell:nth-child(" << idx << ")";
        const auto cell_id = wd.find_element_id_css(css.str());
        auto text = trim_copy(wd.get_element_text(cell_id));
        if (text.empty()) {
            std::ostringstream oss;
            oss << "no letter found for hive cell " << idx;
            throw std::runtime_error(oss.str());
        }
        char letter = text.front();
        if (!std::isalpha(static_cast<unsigned char>(letter))) {
            std::ostringstream oss;
            oss << "unexpected hive character '" << letter << "' at cell " << idx;
            throw std::runtime_error(oss.str());
        }
        char normalized = static_cast<char>(std::tolower(static_cast<unsigned char>(letter)));
        auto classes = wd.get_element_attribute(cell_id, "class");
        auto aria_label = wd.get_element_attribute(cell_id, "aria-label");
        std::string classes_lower = classes.empty() ? std::string() : to_lower_copy(classes);
        bool is_center = false;
        if (!classes_lower.empty()) {
            if (has_class_token(classes_lower, "hive-cell--center") ||
                has_class_token(classes_lower, "hive-cell_center") ||
                has_class_token(classes_lower, "is-center") ||
                (has_class_token(classes_lower, "center") && !has_class_token(classes_lower, "outer"))) {
                is_center = true;
            }
        }
        if (!is_center && !aria_label.empty()) {
            auto aria_lower = to_lower_copy(aria_label);
            if (aria_lower.find("center letter") != std::string::npos ||
                aria_lower == "center") {
                is_center = true;
            }
        }
        cells.emplace_back(normalized, is_center, classes, aria_label);
    }
    if (cells.size() != 7) {
        std::ostringstream oss;
        oss << "expected 7 hive cells but collected " << cells.size();
        throw std::runtime_error(oss.str());
    }
    std::size_t center_count = 0;
    for (const auto& entry : cells) {
        if (std::get<1>(entry)) ++center_count;
    }
    if (center_count == 0) {
        std::cerr << "[WARN] No center marker found in hive; falling back to nth-child(4)\n";
        dump_cell_debug(cells);
        std::get<1>(cells.at(3)) = true;
        center_count = 1;
    }
    if (center_count > 1) {
        dump_cell_debug(cells);
        throw std::runtime_error("multiple hive cells reported as center");
    }
    char center_letter = '\0';
    std::string letters;
    letters.reserve(7);
    for (const auto& entry : cells) {
        const char c = std::get<0>(entry);
        const bool is_center = std::get<1>(entry);
        if (is_center) {
            if (center_letter != '\0' && center_letter != c) {
                dump_cell_debug(cells);
                std::ostringstream oss;
                oss << "multiple differing center letters detected (" << center_letter << " vs " << c << ")";
                throw std::runtime_error(oss.str());
            }
            center_letter = c;
        } else {
            letters.push_back(c);
        }
    }
    if (center_letter == '\0') {
        dump_cell_debug(cells);
        throw std::runtime_error("could not determine center hive letter after processing");
    }
    if (letters.size() != 6) {
        dump_cell_debug(cells);
        std::ostringstream oss;
        oss << "expected 6 outer hive letters but collected " << letters.size();
        throw std::runtime_error(oss.str());
    }
    letters.push_back(center_letter);
    return letters;
}

enum class StopAction { Prompt, Keep, Rerun };

struct Config {
    StopAction stop_action = StopAction::Rerun;
    std::string letters_cli;
    fs::path dictionary_dir;

    bool has_cli_letters() const { return !letters_cli.empty(); }
};

static void print_usage(const char* prog) {
    std::cout << "Usage: " << prog << " [options]\n"
              << "Options:\n"
              << "  --stop-action=prompt|keep|rerun  Control what happens after the run stops.\n"
              << "  --keep-open-on-stop              Shortcut for --stop-action=keep.\n"
              << "  --rerun-on-stop                  Shortcut for --stop-action=rerun.\n"
              << "  --letters=ABCDEFg                Supply hive letters (center letter last).\n"
              << "  --dictionary-dir=PATH           Override word list directory.\n"
              << "  -h, --help                       Show this help message.\n";
}

static Config parse_args(int argc, char** argv) {
    Config cfg;
    cfg.dictionary_dir = find_default_dictionary_dir();
    const std::string stop_prefix = "--stop-action=";
    const std::string letters_prefix = "--letters=";
    const std::string dict_prefix = "--dictionary-dir=";

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            std::exit(0);
        }

        if (arg.rfind(stop_prefix, 0) == 0) {
            std::string value = arg.substr(stop_prefix.size());
            if (value == "prompt") {
                cfg.stop_action = StopAction::Prompt;
            } else if (value == "keep") {
                cfg.stop_action = StopAction::Keep;
            } else if (value == "rerun") {
                cfg.stop_action = StopAction::Rerun;
            } else {
                std::cerr << "Unknown stop action: " << value << "\n";
                print_usage(argv[0]);
                std::exit(1);
            }
            continue;
        }

        if (arg == "--keep-open-on-stop") {
            cfg.stop_action = StopAction::Keep;
            continue;
        }
        if (arg == "--rerun-on-stop") {
            cfg.stop_action = StopAction::Rerun;
            continue;
        }

        if (arg == "--letters") {
            if (i + 1 >= argc) {
                std::cerr << "--letters requires a value\n";
                print_usage(argv[0]);
                std::exit(1);
            }
            cfg.letters_cli = normalize_letters(argv[++i]);
            continue;
        }
        if (arg.rfind(letters_prefix, 0) == 0) {
            cfg.letters_cli = normalize_letters(arg.substr(letters_prefix.size()));
            continue;
        }

        if (arg == "--dictionary-dir") {
            if (i + 1 >= argc) {
                std::cerr << "--dictionary-dir requires a path\n";
                print_usage(argv[0]);
                std::exit(1);
            }
            cfg.dictionary_dir = fs::path(argv[++i]);
            continue;
        }
        if (arg.rfind(dict_prefix, 0) == 0) {
            cfg.dictionary_dir = fs::path(arg.substr(dict_prefix.size()));
            continue;
        }

        std::cerr << "Unknown argument: " << arg << "\n";
        print_usage(argv[0]);
        std::exit(1);
    }

    if (!cfg.letters_cli.empty() && cfg.letters_cli.size() != 7) {
        std::cerr << "letters must contain exactly 7 alphabetic characters (center letter last)\n";
        std::exit(1);
    }

    return cfg;
}

struct AttemptResult {
    bool session_active = false;
    bool user_quit = false;
    bool fatal_error = false;
    std::string fatal_message;
};

static AttemptResult run_attempt(WD& wd,
                                 bool have_session,
                                 bool do_full_setup,
                                 int attempt_index,
                                 const Config& config,
                                 const WordDictionaries& dictionaries) {
    AttemptResult result;
    bool quit = false;
    bool session_active = have_session;
    std::vector<std::string> words_upper;
    std::string letters_lower;

    try {
        if (!session_active) {
            auto r = retry_with_pause("start session", [&] { wd.new_session(); });
            if (r == StepResult::QUIT) {
                quit = true;
            } else if (r == StepResult::OK) {
                session_active = true;
                do_full_setup = true;
            }
        }

        if (!session_active) {
            result.session_active = false;
            result.user_quit = quit;
            return result;
        }

        if (!quit && do_full_setup) {
            auto r = retry_with_pause("navigate", [&] { wd.navigate("https://www.nytimes.com/puzzles/spelling-bee"); });
            if (r == StepResult::QUIT) quit = true;
        }

        if (!quit && do_full_setup) {
            auto r = retry_with_pause("resize window", [&] { wd.set_window_size(1680, 939); });
            if (r == StepResult::QUIT) quit = true;
        }

        if (!quit) {
            if (do_full_setup) {
                pause_banner("Browser ready? Clear modals/login, then press Enter to begin.");
                (void)prompt_line("> ");
            } else {
                std::cout << "\n--- Restarting word list (attempt " << attempt_index << ") ---\n";
            }
        }

        if (!quit) {
            auto r = retry_with_pause("focus hive", [&] {
                auto focusId = wd.find_element_id_css(".hive-cell:nth-child(4)");
                wd.click_element(focusId);
            });
            if (r == StepResult::QUIT) quit = true;
        }

        if (!quit) {
            if (config.has_cli_letters()) {
                letters_lower = config.letters_cli;
            } else {
                auto r = retry_with_pause("read hive letters", [&] {
                    letters_lower = read_letters_from_board(wd);
                });
                if (r == StepResult::QUIT) quit = true;
                else if (r == StepResult::SKIP) quit = true;
            }
        }

        if (!quit && letters_lower.empty()) {
            throw std::runtime_error("no hive letters available");
        }

        if (!quit) {
            auto computed = find_valid_words(dictionaries.massive_words, letters_lower);
            words_upper.clear();
            words_upper.reserve(computed.size());
            for (auto word : computed) {
                to_upper_inplace(word);
                words_upper.push_back(std::move(word));
            }

            auto letters_display = letters_lower;
            to_upper_inplace(letters_display);
            std::string outer = letters_display.substr(0, letters_display.size() - 1);
            char center = letters_display.back();
            std::cout << "\nLetters: " << outer << " (center " << center << ")";
            std::cout << "\nGenerated " << words_upper.size() << " candidate words." << std::endl;
        }

        if (!quit && !words_upper.empty()) {
            auto r = retry_with_pause("send_all_words_as_keys", [&] { wd.send_all_words_as_keys(words_upper); });
            if (r == StepResult::QUIT) quit = true;
        }

        if (!quit && !words_upper.empty()) {
            std::cout << "\nAll done typing.\n";
        } else if (!quit && words_upper.empty()) {
            std::cout << "\nNo words found to send.\n";
        }

        result.session_active = !wd.sessionId.empty();
        result.user_quit = quit;
        return result;
    } catch (const std::exception& e) {
        result.session_active = !wd.sessionId.empty();
        result.user_quit = false;
        result.fatal_error = true;
        result.fatal_message = e.what();
        return result;
    } catch (...) {
        result.session_active = !wd.sessionId.empty();
        result.user_quit = false;
        result.fatal_error = true;
        result.fatal_message = "unknown error";
        return result;
    }
}

// ---------- Main ----------
int main(int argc, char** argv) {
    Config config = parse_args(argc, argv);

    if (config.dictionary_dir.empty()) {
        std::cerr << "[FATAL] Could not locate word list directory. Specify --dictionary-dir=PATH." << std::endl;
        return 1;
    }
    if (!fs::exists(config.dictionary_dir) || !fs::is_directory(config.dictionary_dir)) {
        std::cerr << "[FATAL] Dictionary directory not found: " << config.dictionary_dir << std::endl;
        return 1;
    }

    WordDictionaries dictionaries;
    try {
        dictionaries = load_word_dictionaries(config.dictionary_dir);
    } catch (const std::exception& e) {
        std::cerr << "[FATAL] " << e.what() << std::endl;
        return 1;
    }

    if (dictionaries.massive_words.empty()) {
        std::cerr << "[FATAL] word list 'wlist_match1.txt' appears to be empty in "
                  << config.dictionary_dir << std::endl;
        return 1;
    }

    std::cout << "Loaded word lists from " << config.dictionary_dir
              << " (massive set size: " << dictionaries.massive_words.size() << ")" << std::endl;

    curl_global_init(CURL_GLOBAL_DEFAULT);
    bool want_close = true;
    try {
        CurlSession curl;
        WD wd(curl);

        if (const char* url = std::getenv("WEBDRIVER_URL"); url && *url) {
            wd.base = url;
        }

        bool exit_program = false;
        int attempt_index = 0;
        bool need_full_setup = true;

        while (!exit_program) {
            ++attempt_index;
            const bool have_session = !wd.sessionId.empty();
            bool do_full_setup = need_full_setup || !have_session;
            AttemptResult attempt = run_attempt(wd, have_session, do_full_setup, attempt_index, config, dictionaries);

            if (attempt.fatal_error) {
                std::cerr << "\n[FATAL] " << attempt.fatal_message << "\n";
            }

            if (attempt.user_quit) {
                exit_program = true;
                want_close = true;
                break;
            }

            switch (config.stop_action) {
                case StopAction::Prompt: {
                    std::string ans = prompt_line("\nClose the browser now?  (press Enter = close)  Type 'keep' to leave it open: ");
                    if (ans == "keep" || ans == "k") {
                        want_close = false;
                    } else {
                        want_close = true;
                    }
                    exit_program = true;
                    break;
                }
                case StopAction::Keep: {
                    want_close = false;
                    exit_program = true;
                    break;
                }
                case StopAction::Rerun: {
                    want_close = false;
                    std::string ans;
                    if (attempt.fatal_error) {
                        ans = prompt_line("Fix any issues, then press Enter to rerun, 'quit' to exit, or 'close' to close the browser and exit: ");
                    } else {
                        ans = prompt_line("Press Enter to rerun from the beginning, or type 'quit' to exit, 'close' to close the browser and exit: ");
                    }
                    if (ans == "quit" || ans == "q") {
                        exit_program = true;
                    } else if (ans == "close" || ans == "c") {
                        want_close = true;
                        exit_program = true;
                    } else {
                        exit_program = false;
                    }
                    break;
                }
            }

            if (attempt.fatal_error && config.stop_action != StopAction::Rerun) {
                exit_program = true;
            }

            need_full_setup = attempt.fatal_error || !attempt.session_active;
        }

        if (!wd.sessionId.empty() && want_close) {
            try { wd.delete_session(); } catch (...) {}
        }
    } catch (const std::exception& e) {
        std::cerr << "\n[FATAL] " << e.what() << "\n";
        std::string ans = prompt_line("Type 'keep' to leave the browser open, otherwise press Enter to close: ");
        if (ans == "keep" || ans == "k") {
            want_close = false;
        }
        curl_global_cleanup();
        return 1;
    }
    curl_global_cleanup();
    return 0;
}
