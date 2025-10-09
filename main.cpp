// spellingbee_one_shot.cpp (always window, user-driven start, robust pause/retry, detach Chrome, no gotos)
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using json = nlohmann::json;
using namespace std::chrono_literals;

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

enum class StopAction { Prompt, Keep, Rerun };

struct Config {
    StopAction stop_action = StopAction::Rerun;
};

static void print_usage(const char* prog) {
    std::cout << "Usage: " << prog << " [options]\n"
              << "Options:\n"
              << "  --stop-action=prompt|keep|rerun  Control what happens after the run stops.\n"
              << "  --keep-open-on-stop              Shortcut for --stop-action=keep.\n"
              << "  --rerun-on-stop                  Shortcut for --stop-action=rerun.\n"
              << "  -h, --help                       Show this help message.\n";
}

static Config parse_args(int argc, char** argv) {
    Config cfg;
    const std::string stop_prefix = "--stop-action=";
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            std::exit(0);
        } else if (arg.rfind(stop_prefix, 0) == 0) {
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
        } else if (arg == "--keep-open-on-stop") {
            cfg.stop_action = StopAction::Keep;
        } else if (arg == "--rerun-on-stop") {
            cfg.stop_action = StopAction::Rerun;
        } else {
            std::cerr << "Unknown argument: " << arg << "\n";
            print_usage(argv[0]);
            std::exit(1);
        }
    }
    return cfg;
}

struct AttemptResult {
    bool session_active = false;
    bool user_quit = false;
    bool fatal_error = false;
    std::string fatal_message;
};

static AttemptResult run_attempt(WD& wd, bool have_session, bool do_full_setup, int attempt_index) {
    AttemptResult result;
    bool quit = false;
    bool session_active = have_session;
    std::vector<std::string> words_upper;

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
            for (;;) {
                std::ifstream fin_test("spellingbee_filename");
                if (fin_test) break;
                auto r = retry_with_pause("open spellingbee_filename", [&] {
                    std::ifstream fin_try("spellingbee_filename");
                    if (!fin_try) throw std::runtime_error("cannot open spellingbee_filename");
                });
                if (r == StepResult::QUIT) { quit = true; break; }
                if (r == StepResult::SKIP) { break; }
            }
        }

        if (!quit) {
            std::ifstream fin("spellingbee_filename");
            if (fin) {
                std::string line;
                while (std::getline(fin, line)) {
                    if (line.empty()) continue;
                    to_upper_inplace(line);
                    words_upper.push_back(std::move(line));
                }
            }
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
            AttemptResult attempt = run_attempt(wd, have_session, do_full_setup, attempt_index);

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
