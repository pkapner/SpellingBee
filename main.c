#define _POSIX_C_SOURCE 200809L

#include <curl/curl.h>
#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))
#define WD_ELEMENT_KEY "element-6066-11e4-a52e-4f735466cecf"

typedef struct {
    char **items;
    size_t size;
    size_t capacity;
} WordList;

typedef struct {
    WordList short_words;
    WordList medium_words;
    WordList extended_words;
    WordList massive_words;
} WordDictionaries;

typedef struct {
    char *data;
    size_t length;
    size_t capacity;
} StringBuffer;

typedef struct {
    long status;
    char *body;
    size_t body_size;
} HttpResponse;

typedef struct {
    CURL *curl;
    struct curl_slist *headers;
} CurlSession;

typedef struct {
    CurlSession *session;
    char base[512];
    char session_id[256];
} WD;

typedef enum {
    STEP_RESULT_OK = 0,
    STEP_RESULT_SKIP,
    STEP_RESULT_QUIT
} StepResult;

typedef enum {
    STOP_ACTION_PROMPT = 0,
    STOP_ACTION_KEEP,
    STOP_ACTION_RERUN
} StopAction;

typedef struct {
    StopAction stop_action;
    bool has_cli_letters;
    char letters_cli[8];
    char dictionary_dir[PATH_MAX];
} Config;

typedef struct {
    bool session_active;
    bool user_quit;
    bool fatal_error;
    char *fatal_message;
} AttemptResult;

typedef int (*OperationFn)(void *ctx, char **err_out);

// ---------- Utility helpers ----------

static void set_error(char **err_out, const char *fmt, ...) {
    if (!err_out) return;
    va_list ap;
    va_start(ap, fmt);
    va_list ap_copy;
    va_copy(ap_copy, ap);
    int needed = vsnprintf(NULL, 0, fmt, ap_copy);
    va_end(ap_copy);
    if (needed < 0) {
        *err_out = NULL;
        va_end(ap);
        return;
    }
    *err_out = (char *)malloc((size_t)needed + 1);
    if (!*err_out) {
        va_end(ap);
        return;
    }
    vsnprintf(*err_out, (size_t)needed + 1, fmt, ap);
    va_end(ap);
}

static void string_buffer_init(StringBuffer *sb) {
    sb->data = NULL;
    sb->length = 0;
    sb->capacity = 0;
}

static int string_buffer_reserve(StringBuffer *sb, size_t needed) {
    if (needed <= sb->capacity) return 0;
    size_t new_cap = sb->capacity ? sb->capacity : 256;
    while (new_cap < needed) {
        if (new_cap > (SIZE_MAX / 2)) return -1;
        new_cap *= 2;
    }
    char *new_data = (char *)realloc(sb->data, new_cap);
    if (!new_data) return -1;
    sb->data = new_data;
    sb->capacity = new_cap;
    return 0;
}

static int string_buffer_append(StringBuffer *sb, const char *text) {
    size_t add = strlen(text);
    if (sb->length + add + 1 > sb->capacity) {
        if (string_buffer_reserve(sb, sb->length + add + 1) != 0) return -1;
    }
    memcpy(sb->data + sb->length, text, add);
    sb->length += add;
    sb->data[sb->length] = '\0';
    return 0;
}

static int string_buffer_append_char(StringBuffer *sb, char ch) {
    if (sb->length + 2 > sb->capacity) {
        if (string_buffer_reserve(sb, sb->length + 2) != 0) return -1;
    }
    sb->data[sb->length++] = ch;
    sb->data[sb->length] = '\0';
    return 0;
}

static int string_buffer_append_format(StringBuffer *sb, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    va_list ap_copy;
    va_copy(ap_copy, ap);
    int needed = vsnprintf(NULL, 0, fmt, ap_copy);
    va_end(ap_copy);
    if (needed < 0) {
        va_end(ap);
        return -1;
    }
    if (sb->length + (size_t)needed + 1 > sb->capacity) {
        if (string_buffer_reserve(sb, sb->length + (size_t)needed + 1) != 0) {
            va_end(ap);
            return -1;
        }
    }
    vsnprintf(sb->data + sb->length, (size_t)needed + 1, fmt, ap);
    sb->length += (size_t)needed;
    va_end(ap);
    return 0;
}

static char *string_buffer_steal(StringBuffer *sb) {
    char *result = sb->data;
    sb->data = NULL;
    sb->length = 0;
    sb->capacity = 0;
    return result;
}

static void string_buffer_free(StringBuffer *sb) {
    free(sb->data);
    sb->data = NULL;
    sb->length = 0;
    sb->capacity = 0;
}

static void word_list_init(WordList *list) {
    list->items = NULL;
    list->size = 0;
    list->capacity = 0;
}

static int word_list_reserve(WordList *list, size_t required) {
    if (required <= list->capacity) return 0;
    size_t new_cap = list->capacity ? list->capacity : 256;
    while (new_cap < required) {
        if (new_cap > (SIZE_MAX / 2)) return -1;
        new_cap *= 2;
    }
    char **new_items = (char **)realloc(list->items, new_cap * sizeof(char *));
    if (!new_items) return -1;
    list->items = new_items;
    list->capacity = new_cap;
    return 0;
}

static int word_list_append_copy(WordList *list, const char *word) {
    if (list->size + 1 > list->capacity) {
        if (word_list_reserve(list, list->size + 1) != 0) return -1;
    }
    char *copy = strdup(word);
    if (!copy) return -1;
    list->items[list->size++] = copy;
    return 0;
}

static void word_list_free(WordList *list) {
    for (size_t i = 0; i < list->size; ++i) {
        free(list->items[i]);
    }
    free(list->items);
    list->items = NULL;
    list->size = 0;
    list->capacity = 0;
}

static void to_lower_inplace(char *s) {
    if (!s) return;
    for (; *s; ++s) {
        *s = (char)tolower((unsigned char)*s);
    }
}

static void to_upper_inplace(char *s) {
    if (!s) return;
    for (; *s; ++s) {
        *s = (char)toupper((unsigned char)*s);
    }
}

static void trim_inplace(char *s) {
    if (!s) return;
    size_t len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1])) {
        s[--len] = '\0';
    }
    size_t start = 0;
    while (s[start] && isspace((unsigned char)s[start])) {
        start++;
    }
    if (start > 0) {
        memmove(s, s + start, len - start + 1);
    }
}

static bool contains_vowel(const char *word) {
    static const char vowels[] = "aeiouy";
    if (!word) return false;
    for (; *word; ++word) {
        if (strchr(vowels, (char)tolower((unsigned char)*word))) {
            return true;
        }
    }
    return false;
}

static bool letters_contains(const char *letters, char c) {
    for (const char *p = letters; *p; ++p) {
        if (*p == c) return true;
    }
    return false;
}

static void pause_banner(const char *reason) {
    printf("\n=== PAUSED ======================================\n");
    printf("%s\n", reason);
    printf("Fix the browser if needed, then:\n");
    printf("  [Enter] retry  |  'skip' to skip  |  'quit' to stop workflow\n");
    printf("=================================================\n");
    fflush(stdout);
}

static void prompt_line(const char *prompt, char *buffer, size_t buffer_size) {
    if (!buffer || buffer_size == 0) return;
    printf("%s", prompt);
    fflush(stdout);
    if (!fgets(buffer, (int)buffer_size, stdin)) {
        buffer[0] = '\0';
        clearerr(stdin);
        return;
    }
    size_t len = strlen(buffer);
    if (len > 0 && buffer[len - 1] == '\n') {
        buffer[len - 1] = '\0';
    }
}

// ---------- CURL helpers ----------

static size_t http_write_callback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t total = size * nmemb;
    HttpResponse *resp = (HttpResponse *)userp;
    char *ptr = (char *)realloc(resp->body, resp->body_size + total + 1);
    if (!ptr) return 0;
    resp->body = ptr;
    memcpy(resp->body + resp->body_size, contents, total);
    resp->body_size += total;
    resp->body[resp->body_size] = '\0';
    return total;
}

static int http_response_init(HttpResponse *resp) {
    resp->status = 0;
    resp->body_size = 0;
    resp->body = (char *)malloc(1);
    if (!resp->body) return -1;
    resp->body[0] = '\0';
    return 0;
}

static void http_response_cleanup(HttpResponse *resp) {
    free(resp->body);
    resp->body = NULL;
    resp->body_size = 0;
    resp->status = 0;
}

static int curl_session_init(CurlSession *s, char **err_out) {
    s->curl = curl_easy_init();
    if (!s->curl) {
        set_error(err_out, "curl_easy_init failed");
        return -1;
    }
    s->headers = NULL;
    curl_easy_setopt(s->curl, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(s->curl, CURLOPT_TCP_KEEPIDLE, 30L);
    curl_easy_setopt(s->curl, CURLOPT_TCP_KEEPINTVL, 15L);
    curl_easy_setopt(s->curl, CURLOPT_TIMEOUT, 60L);
    curl_easy_setopt(s->curl, CURLOPT_WRITEFUNCTION, http_write_callback);
    s->headers = curl_slist_append(s->headers, "Content-Type: application/json");
    if (!s->headers) {
        set_error(err_out, "failed to append HTTP header");
        return -1;
    }
    curl_easy_setopt(s->curl, CURLOPT_HTTPHEADER, s->headers);
    return 0;
}

static void curl_session_cleanup(CurlSession *s) {
    if (s->headers) {
        curl_slist_free_all(s->headers);
        s->headers = NULL;
    }
    if (s->curl) {
        curl_easy_cleanup(s->curl);
        s->curl = NULL;
    }
}

static int curl_session_request(CurlSession *s,
                                const char *method,
                                const char *url,
                                const char *payload,
                                HttpResponse *out_resp,
                                char **err_out) {
    if (!s->curl) {
        set_error(err_out, "curl session not initialised");
        return -1;
    }
    if (http_response_init(out_resp) != 0) {
        set_error(err_out, "out of memory allocating HTTP response");
        return -1;
    }
    curl_easy_setopt(s->curl, CURLOPT_URL, url);
    curl_easy_setopt(s->curl, CURLOPT_WRITEDATA, out_resp);
    curl_easy_setopt(s->curl, CURLOPT_HTTPGET, 0L);
    curl_easy_setopt(s->curl, CURLOPT_CUSTOMREQUEST, NULL);
    curl_easy_setopt(s->curl, CURLOPT_POSTFIELDS, NULL);
    curl_easy_setopt(s->curl, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)0);

    if (strcmp(method, "GET") == 0) {
        curl_easy_setopt(s->curl, CURLOPT_HTTPGET, 1L);
    } else if (strcmp(method, "POST") == 0) {
        curl_easy_setopt(s->curl, CURLOPT_POSTFIELDS, payload ? payload : "");
        if (payload) {
            curl_easy_setopt(s->curl, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)strlen(payload));
        }
    } else if (strcmp(method, "DELETE") == 0) {
        curl_easy_setopt(s->curl, CURLOPT_CUSTOMREQUEST, "DELETE");
        if (payload && payload[0]) {
            curl_easy_setopt(s->curl, CURLOPT_POSTFIELDS, payload);
            curl_easy_setopt(s->curl, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)strlen(payload));
        } else {
            curl_easy_setopt(s->curl, CURLOPT_POSTFIELDS, "");
        }
    } else {
        curl_easy_setopt(s->curl, CURLOPT_CUSTOMREQUEST, method);
        if (payload && payload[0]) {
            curl_easy_setopt(s->curl, CURLOPT_POSTFIELDS, payload);
            curl_easy_setopt(s->curl, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)strlen(payload));
        }
    }

    CURLcode code = curl_easy_perform(s->curl);
    if (code != CURLE_OK) {
        set_error(err_out, "CURL error: %s", curl_easy_strerror(code));
        http_response_cleanup(out_resp);
        return -1;
    }
    curl_easy_getinfo(s->curl, CURLINFO_RESPONSE_CODE, &out_resp->status);
    return 0;
}

// ---------- JSON helpers ----------

static bool json_extract_string(const char *json,
                                const char *key,
                                char **out_value) {
    char pattern[256];
    snprintf(pattern, sizeof(pattern), "\"%s\":\"", key);
    const char *pos = strstr(json, pattern);
    if (!pos) return false;
    pos += strlen(pattern);
    const char *end = strchr(pos, '"');
    if (!end) return false;
    size_t len = (size_t)(end - pos);
    char *value = (char *)malloc(len + 1);
    if (!value) return false;
    memcpy(value, pos, len);
    value[len] = '\0';
    *out_value = value;
    return true;
}

static bool json_extract_value_string(const char *json, char **out_value) {
    const char *pattern = "\"value\":";
    const char *pos = strstr(json, pattern);
    if (!pos) return false;
    pos += strlen(pattern);
    while (*pos && isspace((unsigned char)*pos)) pos++;
    if (strncmp(pos, "null", 4) == 0) {
        *out_value = NULL;
        return true;
    }
    if (*pos != '"') return false;
    pos++;
    const char *end = strchr(pos, '"');
    if (!end) return false;
    size_t len = (size_t)(end - pos);
    char *value = (char *)malloc(len + 1);
    if (!value) return false;
    memcpy(value, pos, len);
    value[len] = '\0';
    *out_value = value;
    return true;
}

// ---------- WebDriver helpers ----------

static void wd_init(WD *wd, CurlSession *session) {
    wd->session = session;
    strncpy(wd->base, "http://localhost:9515", sizeof(wd->base));
    wd->base[sizeof(wd->base) - 1] = '\0';
    wd->session_id[0] = '\0';
}

static void wd_set_base(WD *wd, const char *base_url) {
    if (!base_url) return;
    strncpy(wd->base, base_url, sizeof(wd->base));
    wd->base[sizeof(wd->base) - 1] = '\0';
}

static bool wd_has_session(const WD *wd) {
    return wd->session_id[0] != '\0';
}

static void wd_clear_session(WD *wd) {
    wd->session_id[0] = '\0';
}

static int wd_new_session(WD *wd, char **err_out) {
    StringBuffer url;
    string_buffer_init(&url);
    if (string_buffer_append(&url, wd->base) != 0 ||
        string_buffer_append(&url, "/session") != 0) {
        set_error(err_out, "out of memory building session URL");
        string_buffer_free(&url);
        return -1;
    }
    const char *payload =
        "{\"capabilities\":{\"alwaysMatch\":{\"browserName\":\"chrome\","
        "\"goog:chromeOptions\":{\"args\":[\"--disable-features=PaintHolding\","
        "\"--disable-extensions\",\"--mute-audio\",\"--remote-allow-origins=*\"],"
        "\"detach\":true}}}}";

    HttpResponse resp;
    int rc = curl_session_request(wd->session, "POST", url.data, payload, &resp, err_out);
    string_buffer_free(&url);
    if (rc != 0) return -1;

    if (resp.status < 200 || resp.status >= 300) {
        set_error(err_out, "HTTP %ld from new session: %s", resp.status, resp.body ? resp.body : "");
        http_response_cleanup(&resp);
        return -1;
    }

    char *session_id = NULL;
    if (!json_extract_string(resp.body, "sessionId", &session_id)) {
        set_error(err_out, "failed to parse sessionId from response: %s", resp.body ? resp.body : "(null)");
        http_response_cleanup(&resp);
        return -1;
    }
    strncpy(wd->session_id, session_id, sizeof(wd->session_id));
    wd->session_id[sizeof(wd->session_id) - 1] = '\0';
    free(session_id);
    http_response_cleanup(&resp);
    return 0;
}

static int wd_delete_session(WD *wd, char **err_out) {
    if (!wd_has_session(wd)) return 0;
    StringBuffer url;
    string_buffer_init(&url);
    if (string_buffer_append(&url, wd->base) != 0 ||
        string_buffer_append(&url, "/session/") != 0 ||
        string_buffer_append(&url, wd->session_id) != 0) {
        set_error(err_out, "out of memory building delete URL");
        string_buffer_free(&url);
        return -1;
    }
    HttpResponse resp;
    int rc = curl_session_request(wd->session, "DELETE", url.data, NULL, &resp, err_out);
    string_buffer_free(&url);
    if (rc != 0) return -1;
    http_response_cleanup(&resp);
    wd_clear_session(wd);
    return 0;
}

static int wd_navigate(WD *wd, const char *target, char **err_out) {
    if (!wd_has_session(wd)) {
        set_error(err_out, "cannot navigate without active session");
        return -1;
    }
    StringBuffer url;
    string_buffer_init(&url);
    if (string_buffer_append(&url, wd->base) != 0 ||
        string_buffer_append(&url, "/session/") != 0 ||
        string_buffer_append(&url, wd->session_id) != 0 ||
        string_buffer_append(&url, "/url") != 0) {
        set_error(err_out, "out of memory building navigate URL");
        string_buffer_free(&url);
        return -1;
    }
    StringBuffer payload;
    string_buffer_init(&payload);
    if (string_buffer_append_format(&payload, "{\"url\":\"%s\"}", target) != 0) {
        set_error(err_out, "out of memory building navigate payload");
        string_buffer_free(&url);
        string_buffer_free(&payload);
        return -1;
    }

    HttpResponse resp;
    int rc = curl_session_request(wd->session, "POST", url.data, payload.data, &resp, err_out);
    string_buffer_free(&url);
    string_buffer_free(&payload);
    if (rc != 0) return -1;
    if (resp.status < 200 || resp.status >= 300) {
        set_error(err_out, "HTTP %ld navigating to %s: %s", resp.status, target, resp.body ? resp.body : "");
        http_response_cleanup(&resp);
        return -1;
    }
    http_response_cleanup(&resp);
    return 0;
}

static int wd_set_window_size(WD *wd, int width, int height, char **err_out) {
    if (!wd_has_session(wd)) {
        set_error(err_out, "cannot resize window without active session");
        return -1;
    }
    StringBuffer url;
    string_buffer_init(&url);
    if (string_buffer_append(&url, wd->base) != 0 ||
        string_buffer_append(&url, "/session/") != 0 ||
        string_buffer_append(&url, wd->session_id) != 0 ||
        string_buffer_append(&url, "/window/rect") != 0) {
        set_error(err_out, "out of memory building rect URL");
        string_buffer_free(&url);
        return -1;
    }
    StringBuffer payload;
    string_buffer_init(&payload);
    if (string_buffer_append_format(&payload, "{\"width\":%d,\"height\":%d}", width, height) != 0) {
        set_error(err_out, "out of memory building rect payload");
        string_buffer_free(&url);
        string_buffer_free(&payload);
        return -1;
    }
    HttpResponse resp;
    int rc = curl_session_request(wd->session, "POST", url.data, payload.data, &resp, err_out);
    string_buffer_free(&url);
    string_buffer_free(&payload);
    if (rc != 0) return -1;
    if (resp.status < 200 || resp.status >= 300) {
        set_error(err_out, "HTTP %ld setting window size: %s", resp.status, resp.body ? resp.body : "");
        http_response_cleanup(&resp);
        return -1;
    }
    http_response_cleanup(&resp);
    return 0;
}

static int wd_find_element_id_css(WD *wd, const char *css_selector, char **out_elem_id, char **err_out) {
    if (!wd_has_session(wd)) {
        set_error(err_out, "cannot find element without active session");
        return -1;
    }
    StringBuffer url;
    string_buffer_init(&url);
    if (string_buffer_append(&url, wd->base) != 0 ||
        string_buffer_append(&url, "/session/") != 0 ||
        string_buffer_append(&url, wd->session_id) != 0 ||
        string_buffer_append(&url, "/element") != 0) {
        set_error(err_out, "out of memory building element URL");
        string_buffer_free(&url);
        return -1;
    }
    StringBuffer payload;
    string_buffer_init(&payload);
    if (string_buffer_append_format(&payload,
                                    "{\"using\":\"css selector\",\"value\":\"%s\"}",
                                    css_selector) != 0) {
        set_error(err_out, "out of memory building element payload");
        string_buffer_free(&url);
        string_buffer_free(&payload);
        return -1;
    }
    HttpResponse resp;
    int rc = curl_session_request(wd->session, "POST", url.data, payload.data, &resp, err_out);
    string_buffer_free(&url);
    string_buffer_free(&payload);
    if (rc != 0) return -1;
    if (resp.status < 200 || resp.status >= 300) {
        set_error(err_out, "HTTP %ld finding element (%s): %s", resp.status, css_selector, resp.body ? resp.body : "");
        http_response_cleanup(&resp);
        return -1;
    }
    char pattern[256];
    snprintf(pattern, sizeof(pattern), "\"%s\":\"", WD_ELEMENT_KEY);
    const char *pos = strstr(resp.body, pattern);
    if (!pos) {
        set_error(err_out, "element response missing id: %s", resp.body ? resp.body : "");
        http_response_cleanup(&resp);
        return -1;
    }
    pos += strlen(pattern);
    const char *end = strchr(pos, '"');
    if (!end) {
        set_error(err_out, "element id string not terminated");
        http_response_cleanup(&resp);
        return -1;
    }
    size_t len = (size_t)(end - pos);
    char *value = (char *)malloc(len + 1);
    if (!value) {
        set_error(err_out, "out of memory copying element id");
        http_response_cleanup(&resp);
        return -1;
    }
    memcpy(value, pos, len);
    value[len] = '\0';
    *out_elem_id = value;
    http_response_cleanup(&resp);
    return 0;
}

static int wd_get_element_text(WD *wd, const char *elem_id, char **out_text, char **err_out) {
    if (!wd_has_session(wd)) {
        set_error(err_out, "cannot get element text without active session");
        return -1;
    }
    StringBuffer url;
    string_buffer_init(&url);
    if (string_buffer_append(&url, wd->base) != 0 ||
        string_buffer_append(&url, "/session/") != 0 ||
        string_buffer_append(&url, wd->session_id) != 0 ||
        string_buffer_append(&url, "/element/") != 0 ||
        string_buffer_append(&url, elem_id) != 0 ||
        string_buffer_append(&url, "/text") != 0) {
        set_error(err_out, "out of memory building text URL");
        string_buffer_free(&url);
        return -1;
    }
    HttpResponse resp;
    int rc = curl_session_request(wd->session, "GET", url.data, NULL, &resp, err_out);
    string_buffer_free(&url);
    if (rc != 0) return -1;
    if (resp.status < 200 || resp.status >= 300) {
        set_error(err_out, "HTTP %ld getting element text: %s", resp.status, resp.body ? resp.body : "");
        http_response_cleanup(&resp);
        return -1;
    }
    char *value = NULL;
    if (!json_extract_value_string(resp.body, &value)) {
        set_error(err_out, "failed to parse element text: %s", resp.body ? resp.body : "");
        http_response_cleanup(&resp);
        return -1;
    }
    *out_text = value ? value : strdup("");
    if (!*out_text) {
        set_error(err_out, "out of memory duplicating element text");
        http_response_cleanup(&resp);
        return -1;
    }
    http_response_cleanup(&resp);
    return 0;
}

static int wd_get_element_attribute(WD *wd,
                                    const char *elem_id,
                                    const char *attribute,
                                    char **out_value,
                                    char **err_out) {
    if (!wd_has_session(wd)) {
        set_error(err_out, "cannot get element attribute without active session");
        return -1;
    }
    StringBuffer url;
    string_buffer_init(&url);
    if (string_buffer_append(&url, wd->base) != 0 ||
        string_buffer_append(&url, "/session/") != 0 ||
        string_buffer_append(&url, wd->session_id) != 0 ||
        string_buffer_append(&url, "/element/") != 0 ||
        string_buffer_append(&url, elem_id) != 0 ||
        string_buffer_append(&url, "/attribute/") != 0 ||
        string_buffer_append(&url, attribute) != 0) {
        set_error(err_out, "out of memory building attribute URL");
        string_buffer_free(&url);
        return -1;
    }
    HttpResponse resp;
    int rc = curl_session_request(wd->session, "GET", url.data, NULL, &resp, err_out);
    string_buffer_free(&url);
    if (rc != 0) return -1;
    if (resp.status < 200 || resp.status >= 300) {
        set_error(err_out, "HTTP %ld getting attribute %s: %s", resp.status, attribute, resp.body ? resp.body : "");
        http_response_cleanup(&resp);
        return -1;
    }
    char *value = NULL;
    if (!json_extract_value_string(resp.body, &value)) {
        set_error(err_out, "failed to parse attribute %s: %s", attribute, resp.body ? resp.body : "");
        http_response_cleanup(&resp);
        return -1;
    }
    if (value) {
        *out_value = value;
    } else {
        *out_value = strdup("");
        if (!*out_value) {
            set_error(err_out, "out of memory duplicating attribute value");
            http_response_cleanup(&resp);
            return -1;
        }
    }
    http_response_cleanup(&resp);
    return 0;
}

static int wd_click_element(WD *wd, const char *elem_id, char **err_out) {
    if (!wd_has_session(wd)) {
        set_error(err_out, "cannot click without active session");
        return -1;
    }
    StringBuffer url;
    string_buffer_init(&url);
    if (string_buffer_append(&url, wd->base) != 0 ||
        string_buffer_append(&url, "/session/") != 0 ||
        string_buffer_append(&url, wd->session_id) != 0 ||
        string_buffer_append(&url, "/element/") != 0 ||
        string_buffer_append(&url, elem_id) != 0 ||
        string_buffer_append(&url, "/click") != 0) {
        set_error(err_out, "out of memory building click URL");
        string_buffer_free(&url);
        return -1;
    }
    HttpResponse resp;
    int rc = curl_session_request(wd->session, "POST", url.data, "{}", &resp, err_out);
    string_buffer_free(&url);
    if (rc != 0) return -1;
    if (resp.status < 200 || resp.status >= 300) {
        set_error(err_out, "HTTP %ld clicking element: %s", resp.status, resp.body ? resp.body : "");
        http_response_cleanup(&resp);
        return -1;
    }
    http_response_cleanup(&resp);
    return 0;
}

static int wd_send_all_words_as_keys(WD *wd, const WordList *words, char **err_out) {
    if (!wd_has_session(wd)) {
        set_error(err_out, "cannot send keys without active session");
        return -1;
    }
    StringBuffer url;
    string_buffer_init(&url);
    if (string_buffer_append(&url, wd->base) != 0 ||
        string_buffer_append(&url, "/session/") != 0 ||
        string_buffer_append(&url, wd->session_id) != 0 ||
        string_buffer_append(&url, "/actions") != 0) {
        set_error(err_out, "out of memory building actions URL");
        string_buffer_free(&url);
        return -1;
    }
    StringBuffer payload;
    string_buffer_init(&payload);
    if (string_buffer_append(&payload,
                             "{\"actions\":[{\"type\":\"key\",\"id\":\"keyboard\",\"actions\":[") != 0) {
        set_error(err_out, "out of memory starting actions payload");
        string_buffer_free(&url);
        string_buffer_free(&payload);
        return -1;
    }

    bool first = true;
    for (size_t i = 0; i < words->size; ++i) {
        const char *word = words->items[i];
        for (size_t j = 0; word[j]; ++j) {
            if (!first) {
                if (string_buffer_append(&payload, ",") != 0) goto payload_fail;
            }
            first = false;
            if (string_buffer_append_format(&payload,
                                            "{\"type\":\"keyDown\",\"value\":\"%c\"}",
                                            word[j]) != 0) goto payload_fail;
            if (string_buffer_append(&payload, ",") != 0) goto payload_fail;
            if (string_buffer_append_format(&payload,
                                            "{\"type\":\"keyUp\",\"value\":\"%c\"}",
                                            word[j]) != 0) goto payload_fail;
        }
        if (!first) {
            if (string_buffer_append(&payload, ",") != 0) goto payload_fail;
        }
        first = false;
        if (string_buffer_append(&payload,
                                 "{\"type\":\"keyDown\",\"value\":\"\\uE007\"},{\"type\":\"keyUp\",\"value\":\"\\uE007\"}") != 0) {
            goto payload_fail;
        }
    }
    if (string_buffer_append(&payload, "]}]}") != 0) goto payload_fail;

    {
        HttpResponse resp;
        int rc = curl_session_request(wd->session, "POST", url.data, payload.data, &resp, err_out);
        string_buffer_free(&url);
        string_buffer_free(&payload);
        if (rc != 0) return -1;
        if (resp.status < 200 || resp.status >= 300) {
            set_error(err_out, "HTTP %ld sending key actions: %s", resp.status, resp.body ? resp.body : "");
            http_response_cleanup(&resp);
            return -1;
        }
        http_response_cleanup(&resp);
        return 0;
    }

payload_fail:
    set_error(err_out, "out of memory building actions payload");
    string_buffer_free(&url);
    string_buffer_free(&payload);
    return -1;
}

// ---------- Game helpers ----------

typedef struct {
    char letter;
    bool marked_center;
    char *class_attr;
    char *aria_attr;
} HiveCell;

static bool has_class_token(const char *classes, const char *token) {
    if (!classes || !token || !*token) return false;
    const char *ptr = classes;
    size_t token_len = strlen(token);
    while (*ptr) {
        while (*ptr && isspace((unsigned char)*ptr)) ptr++;
        if (!*ptr) break;
        const char *end = ptr;
        while (*end && !isspace((unsigned char)*end)) end++;
        size_t len = (size_t)(end - ptr);
        if (len == token_len && strncmp(ptr, token, token_len) == 0) {
            return true;
        }
        ptr = end;
    }
    return false;
}

static void dump_cell_debug(const HiveCell *cells, size_t count) {
    fprintf(stderr, "[DEBUG] Hive cell attributes:\n");
    for (size_t i = 0; i < count; ++i) {
        fprintf(stderr,
                "  #%zu letter='%c' center=%s classes='%s' aria='%s'\n",
                i + 1,
                cells[i].letter,
                cells[i].marked_center ? "true" : "false",
                cells[i].class_attr ? cells[i].class_attr : "",
                cells[i].aria_attr ? cells[i].aria_attr : "");
    }
}

static int read_letters_from_board(WD *wd, char letters_out[8], char **err_out) {
    HiveCell cells[7];
    memset(cells, 0, sizeof(cells));
    for (int idx = 1; idx <= 7; ++idx) {
        char css[64];
        snprintf(css, sizeof(css), ".hive-cell:nth-child(%d)", idx);
        char *elem_id = NULL;
        if (wd_find_element_id_css(wd, css, &elem_id, err_out) != 0) {
            for (int j = 0; j < idx - 1; ++j) {
                free(cells[j].class_attr);
                free(cells[j].aria_attr);
            }
            return -1;
        }
        char *text = NULL;
        int rc = wd_get_element_text(wd, elem_id, &text, err_out);
        if (rc != 0) {
            free(elem_id);
            for (int j = 0; j < idx - 1; ++j) {
                free(cells[j].class_attr);
                free(cells[j].aria_attr);
            }
            return -1;
        }
        if (!text || text[0] == '\0') {
            set_error(err_out, "no letter found for hive cell %d", idx);
            free(elem_id);
            free(text);
            for (int j = 0; j < idx - 1; ++j) {
                free(cells[j].class_attr);
                free(cells[j].aria_attr);
            }
            return -1;
        }
        char letter = text[0];
        if (!isalpha((unsigned char)letter)) {
            set_error(err_out, "unexpected hive character '%c' at cell %d", letter, idx);
            free(elem_id);
            free(text);
            for (int j = 0; j < idx - 1; ++j) {
                free(cells[j].class_attr);
                free(cells[j].aria_attr);
            }
            return -1;
        }
        char normalized = (char)tolower((unsigned char)letter);

        char *class_attr = NULL;
        rc = wd_get_element_attribute(wd, elem_id, "class", &class_attr, err_out);
        if (rc != 0) {
            free(elem_id);
            free(text);
            for (int j = 0; j < idx - 1; ++j) {
                free(cells[j].class_attr);
                free(cells[j].aria_attr);
            }
            return -1;
        }
        char *aria_attr = NULL;
        rc = wd_get_element_attribute(wd, elem_id, "aria-label", &aria_attr, err_out);
        free(elem_id);
        if (rc != 0) {
            free(text);
            free(class_attr);
            for (int j = 0; j < idx - 1; ++j) {
                free(cells[j].class_attr);
                free(cells[j].aria_attr);
            }
            return -1;
        }

        if (class_attr) to_lower_inplace(class_attr);
        if (aria_attr) to_lower_inplace(aria_attr);

        bool is_center = false;
        if (class_attr && class_attr[0]) {
            if (has_class_token(class_attr, "hive-cell--center") ||
                has_class_token(class_attr, "hive-cell_center") ||
                has_class_token(class_attr, "is-center") ||
                (has_class_token(class_attr, "center") && !has_class_token(class_attr, "outer"))) {
                is_center = true;
            }
        }
        if (!is_center && aria_attr && aria_attr[0]) {
            if (strstr(aria_attr, "center letter") || strcmp(aria_attr, "center") == 0) {
                is_center = true;
            }
        }

        cells[idx - 1].letter = normalized;
        cells[idx - 1].marked_center = is_center;
        cells[idx - 1].class_attr = class_attr;
        cells[idx - 1].aria_attr = aria_attr;
        free(text);
    }

    size_t center_count = 0;
    for (int i = 0; i < 7; ++i) {
        if (cells[i].marked_center) center_count++;
    }
    if (center_count == 0) {
        fprintf(stderr, "[WARN] No center marker found in hive; falling back to nth-child(4)\n");
        dump_cell_debug(cells, 7);
        cells[3].marked_center = true;
        center_count = 1;
    }
    if (center_count > 1) {
        dump_cell_debug(cells, 7);
        for (int i = 0; i < 7; ++i) {
            free(cells[i].class_attr);
            free(cells[i].aria_attr);
        }
        set_error(err_out, "multiple hive cells reported as center");
        return -1;
    }

    char outer[7];
    size_t outer_count = 0;
    char center_letter = '\0';
    for (int i = 0; i < 7; ++i) {
        if (cells[i].marked_center) {
            if (center_letter != '\0' && center_letter != cells[i].letter) {
                dump_cell_debug(cells, 7);
                for (int j = 0; j < 7; ++j) {
                    free(cells[j].class_attr);
                    free(cells[j].aria_attr);
                }
                set_error(err_out, "multiple differing center letters detected");
                return -1;
            }
            center_letter = cells[i].letter;
        } else {
            if (outer_count >= ARRAY_LEN(outer)) {
                for (int j = 0; j < 7; ++j) {
                    free(cells[j].class_attr);
                    free(cells[j].aria_attr);
                }
                set_error(err_out, "too many outer letters collected");
                return -1;
            }
            outer[outer_count++] = cells[i].letter;
        }
    }
    if (center_letter == '\0') {
        dump_cell_debug(cells, 7);
        for (int j = 0; j < 7; ++j) {
            free(cells[j].class_attr);
            free(cells[j].aria_attr);
        }
        set_error(err_out, "could not determine center hive letter after processing");
        return -1;
    }
    if (outer_count != 6) {
        dump_cell_debug(cells, 7);
        for (int j = 0; j < 7; ++j) {
            free(cells[j].class_attr);
            free(cells[j].aria_attr);
        }
        set_error(err_out, "expected 6 outer hive letters but collected %zu", outer_count);
        return -1;
    }
    for (int j = 0; j < 7; ++j) {
        free(cells[j].class_attr);
        free(cells[j].aria_attr);
    }
    memcpy(letters_out, outer, 6);
    letters_out[6] = center_letter;
    letters_out[7] = '\0';
    return 0;
}

static int find_valid_words(const WordList *dictionary,
                            const char letters[8],
                            WordList *results) {
    const char required = letters[6];
    for (size_t i = 0; i < dictionary->size; ++i) {
        const char *word = dictionary->items[i];
        size_t len = strlen(word);
        if (len < 4) continue;
        bool missing = false;
        for (size_t j = 0; j < len; ++j) {
            char c = word[j];
            if (!letters_contains(letters, c)) {
                missing = true;
                break;
            }
        }
        if (missing) continue;
        if (!strchr(word, required)) continue;
        if (!contains_vowel(word)) continue;
        char *copy = strdup(word);
        if (!copy) return -1;
        to_upper_inplace(copy);
        if (word_list_append_copy(results, copy) != 0) {
            free(copy);
            return -1;
        }
        free(copy);
    }
    return 0;
}

// ---------- Pause/Retry ----------

static StepResult retry_with_pause(const char *what, OperationFn fn, void *ctx) {
    for (;;) {
        char *err = NULL;
        int rc = fn(ctx, &err);
        if (rc == 0) {
            free(err);
            return STEP_RESULT_OK;
        }
        fprintf(stderr, "[WARN] %s failed: %s\n", what, err ? err : "unknown error");
        free(err);
        pause_banner(what);
        char answer[32];
        prompt_line("> ", answer, sizeof(answer));
        if (strcmp(answer, "quit") == 0 || strcmp(answer, "q") == 0) {
            return STEP_RESULT_QUIT;
        }
        if (strcmp(answer, "skip") == 0 || strcmp(answer, "s") == 0) {
            return STEP_RESULT_SKIP;
        }
    }
}

// ---------- Argument parsing ----------

static bool path_is_directory(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) return false;
    return S_ISDIR(st.st_mode);
}

static bool normalize_letters_input(const char *raw, char out_letters[8], char **err_out) {
    size_t len = 0;
    for (const char *p = raw; *p; ++p) {
        if (isspace((unsigned char)*p)) continue;
        if (!isalpha((unsigned char)*p)) {
            set_error(err_out, "letters may only contain alphabetic characters (got '%c')", *p);
            return false;
        }
        if (len >= 7) {
            set_error(err_out, "letters must contain exactly 7 alphabetic characters");
            return false;
        }
        out_letters[len++] = (char)tolower((unsigned char)*p);
    }
    if (len != 7) {
        set_error(err_out, "letters must contain exactly 7 alphabetic characters");
        return false;
    }
    out_letters[7] = '\0';
    return true;
}

static bool find_default_dictionary_dir(char *out_dir, size_t out_size) {
    const char *candidates[] = {
        "WordListerApp/target/classes/com/uestechnology",
        "WordListerApp/src/main/resources/com/uestechnology",
        "WordListerApp/extracted/com/uestechnology"
    };
    for (size_t i = 0; i < ARRAY_LEN(candidates); ++i) {
        if (path_is_directory(candidates[i])) {
            strncpy(out_dir, candidates[i], out_size);
            out_dir[out_size - 1] = '\0';
            return true;
        }
    }
    return false;
}

static bool parse_args(int argc, char **argv, Config *config) {
    config->stop_action = STOP_ACTION_RERUN;
    config->has_cli_letters = false;
    config->letters_cli[0] = '\0';
    config->dictionary_dir[0] = '\0';
    find_default_dictionary_dir(config->dictionary_dir, sizeof(config->dictionary_dir));

    for (int i = 1; i < argc; ++i) {
        const char *arg = argv[i];
        if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
            printf("Usage: %s [options]\n", argv[0]);
            printf("Options:\n");
            printf("  --stop-action=prompt|keep|rerun  Control what happens after the run stops.\n");
            printf("  --keep-open-on-stop              Shortcut for --stop-action=keep.\n");
            printf("  --rerun-on-stop                  Shortcut for --stop-action=rerun.\n");
            printf("  --letters=ABCDEFg                Supply hive letters (center letter last).\n");
            printf("  --dictionary-dir=PATH            Override word list directory.\n");
            return false;
        }
        if (strncmp(arg, "--stop-action=", 14) == 0) {
            const char *value = arg + 14;
            if (strcmp(value, "prompt") == 0) config->stop_action = STOP_ACTION_PROMPT;
            else if (strcmp(value, "keep") == 0) config->stop_action = STOP_ACTION_KEEP;
            else if (strcmp(value, "rerun") == 0) config->stop_action = STOP_ACTION_RERUN;
            else {
                fprintf(stderr, "Unknown stop action: %s\n", value);
                return false;
            }
            continue;
        }
        if (strcmp(arg, "--keep-open-on-stop") == 0) {
            config->stop_action = STOP_ACTION_KEEP;
            continue;
        }
        if (strcmp(arg, "--rerun-on-stop") == 0) {
            config->stop_action = STOP_ACTION_RERUN;
            continue;
        }
        if (strcmp(arg, "--letters") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "--letters requires a value\n");
                return false;
            }
            char *err = NULL;
            if (!normalize_letters_input(argv[++i], config->letters_cli, &err)) {
                fprintf(stderr, "%s\n", err ? err : "invalid letters");
                free(err);
                return false;
            }
            free(err);
            config->has_cli_letters = true;
            continue;
        }
        if (strncmp(arg, "--letters=", 10) == 0) {
            char *err = NULL;
            if (!normalize_letters_input(arg + 10, config->letters_cli, &err)) {
                fprintf(stderr, "%s\n", err ? err : "invalid letters");
                free(err);
                return false;
            }
            free(err);
            config->has_cli_letters = true;
            continue;
        }
        if (strcmp(arg, "--dictionary-dir") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "--dictionary-dir requires a value\n");
                return false;
            }
            strncpy(config->dictionary_dir, argv[++i], sizeof(config->dictionary_dir));
            config->dictionary_dir[sizeof(config->dictionary_dir) - 1] = '\0';
            continue;
        }
        if (strncmp(arg, "--dictionary-dir=", 17) == 0) {
            strncpy(config->dictionary_dir, arg + 17, sizeof(config->dictionary_dir));
            config->dictionary_dir[sizeof(config->dictionary_dir) - 1] = '\0';
            continue;
        }
        fprintf(stderr, "Unknown argument: %s\n", arg);
        return false;
    }
    return true;
}

// ---------- Dictionary loading ----------

static int load_word_file(const char *path, WordList *out_list) {
    FILE *fp = fopen(path, "r");
    if (!fp) {
        fprintf(stderr, "failed to open dictionary file: %s (%s)\n", path, strerror(errno));
        return -1;
    }
    char *line = NULL;
    size_t cap = 0;
    ssize_t len;
    while ((len = getline(&line, &cap, fp)) != -1) {
        (void)len;
        trim_inplace(line);
        if (line[0] == '\0') continue;
        to_lower_inplace(line);
        if (word_list_append_copy(out_list, line) != 0) {
            fprintf(stderr, "out of memory loading dictionary words\n");
            free(line);
            fclose(fp);
            return -1;
        }
    }
    free(line);
    fclose(fp);
    return 0;
}

static int load_word_dictionaries(const Config *config, WordDictionaries *dicts) {
    word_list_init(&dicts->short_words);
    word_list_init(&dicts->medium_words);
    word_list_init(&dicts->extended_words);
    word_list_init(&dicts->massive_words);

    StringBuffer path;
    string_buffer_init(&path);

    if (string_buffer_append_format(&path, "%s/%s", config->dictionary_dir, "wordlist.txt") != 0 ||
        load_word_file(path.data, &dicts->short_words) != 0) {
        string_buffer_free(&path);
        return -1;
    }
    path.length = 0; path.data[0] = '\0';
    if (string_buffer_append_format(&path, "%s/%s", config->dictionary_dir, "wiki-100k.txt") != 0 ||
        load_word_file(path.data, &dicts->medium_words) != 0) {
        string_buffer_free(&path);
        return -1;
    }
    path.length = 0; path.data[0] = '\0';
    if (string_buffer_append_format(&path, "%s/%s", config->dictionary_dir, "words.txt") != 0 ||
        load_word_file(path.data, &dicts->extended_words) != 0) {
        string_buffer_free(&path);
        return -1;
    }
    path.length = 0; path.data[0] = '\0';
    if (string_buffer_append_format(&path, "%s/%s", config->dictionary_dir, "words400k.txt") != 0 ||
        load_word_file(path.data, &dicts->extended_words) != 0) {
        string_buffer_free(&path);
        return -1;
    }
    path.length = 0; path.data[0] = '\0';
    if (string_buffer_append_format(&path, "%s/%s", config->dictionary_dir, "wlist_match1.txt") != 0 ||
        load_word_file(path.data, &dicts->massive_words) != 0) {
        string_buffer_free(&path);
        return -1;
    }

    string_buffer_free(&path);
    return 0;
}

static void free_word_dictionaries(WordDictionaries *dicts) {
    word_list_free(&dicts->short_words);
    word_list_free(&dicts->medium_words);
    word_list_free(&dicts->extended_words);
    word_list_free(&dicts->massive_words);
}

// ---------- Operation wrappers ----------

static int op_new_session(void *ctx, char **err_out) {
    return wd_new_session((WD *)ctx, err_out);
}

typedef struct {
    WD *wd;
    const char *url;
} NavigateCtx;

static int op_navigate(void *ctx, char **err_out) {
    NavigateCtx *nav = (NavigateCtx *)ctx;
    return wd_navigate(nav->wd, nav->url, err_out);
}

typedef struct {
    WD *wd;
    int width;
    int height;
} ResizeCtx;

static int op_resize(void *ctx, char **err_out) {
    ResizeCtx *rs = (ResizeCtx *)ctx;
    return wd_set_window_size(rs->wd, rs->width, rs->height, err_out);
}

typedef struct {
    WD *wd;
    char letters[8];
} LettersCtx;

static int op_read_letters(void *ctx, char **err_out) {
    LettersCtx *lc = (LettersCtx *)ctx;
    return read_letters_from_board(lc->wd, lc->letters, err_out);
}

typedef struct {
    WD *wd;
} FocusCtx;

static int op_focus_hive(void *ctx, char **err_out) {
    FocusCtx *fc = (FocusCtx *)ctx;
    char *elem_id = NULL;
    int rc = wd_find_element_id_css(fc->wd, ".hive-cell:nth-child(4)", &elem_id, err_out);
    if (rc != 0) return -1;
    rc = wd_click_element(fc->wd, elem_id, err_out);
    free(elem_id);
    return rc;
}

typedef struct {
    WD *wd;
    const WordList *words;
} SendWordsCtx;

static int op_send_words(void *ctx, char **err_out) {
    SendWordsCtx *sw = (SendWordsCtx *)ctx;
    return wd_send_all_words_as_keys(sw->wd, sw->words, err_out);
}

// ---------- Attempt runner ----------

static AttemptResult run_attempt(WD *wd,
                                 bool have_session,
                                 bool do_full_setup,
                                 int attempt_index,
                                 const Config *config,
                                 const WordDictionaries *dicts) {
    AttemptResult result = {0};
    WordList words_upper;
    word_list_init(&words_upper);
    char letters[8] = {0};

    bool quit = false;
    bool session_active = have_session;
    do_full_setup = do_full_setup || !have_session;

    if (!session_active) {
        StepResult sr = retry_with_pause("start session", op_new_session, wd);
        if (sr == STEP_RESULT_QUIT) {
            quit = true;
        } else if (sr == STEP_RESULT_OK) {
            session_active = true;
            do_full_setup = true;
        } else {
            quit = true;
        }
    }

    if (!session_active) {
        result.session_active = false;
        result.user_quit = quit;
        word_list_free(&words_upper);
        return result;
    }

    if (!quit && do_full_setup) {
        NavigateCtx nav = {.wd = wd, .url = "https://www.nytimes.com/puzzles/spelling-bee"};
        StepResult sr = retry_with_pause("navigate", op_navigate, &nav);
        if (sr == STEP_RESULT_QUIT || sr == STEP_RESULT_SKIP) quit = true;
    }

    if (!quit && do_full_setup) {
        ResizeCtx rs = {.wd = wd, .width = 1680, .height = 939};
        StepResult sr = retry_with_pause("resize window", op_resize, &rs);
        if (sr == STEP_RESULT_QUIT || sr == STEP_RESULT_SKIP) quit = true;
    }

    if (!quit) {
        if (do_full_setup) {
            pause_banner("Browser ready? Clear modals/login, then press Enter to begin.");
            char dummy[8];
            prompt_line("> ", dummy, sizeof(dummy));
        } else {
            printf("\n--- Restarting word list (attempt %d) ---\n", attempt_index);
            fflush(stdout);
        }
    }

    if (!quit) {
        FocusCtx fc = {.wd = wd};
        StepResult sr = retry_with_pause("focus hive", op_focus_hive, &fc);
        if (sr == STEP_RESULT_QUIT || sr == STEP_RESULT_SKIP) quit = true;
    }

    if (!quit) {
        if (config->has_cli_letters) {
            memcpy(letters, config->letters_cli, sizeof(letters));
        } else {
            LettersCtx lc = {.wd = wd};
            StepResult sr = retry_with_pause("read hive letters", op_read_letters, &lc);
            if (sr == STEP_RESULT_QUIT || sr == STEP_RESULT_SKIP) {
                quit = true;
            } else if (sr == STEP_RESULT_OK) {
                memcpy(letters, lc.letters, sizeof(letters));
            }
        }
    }

    if (!quit && letters[0] == '\0') {
        result.fatal_error = true;
        result.fatal_message = strdup("no hive letters available");
        quit = true;
    }

    if (!quit) {
        if (find_valid_words(&dicts->massive_words, letters, &words_upper) != 0) {
            result.fatal_error = true;
            result.fatal_message = strdup("failed to compute valid words");
            quit = true;
        }
    }

    if (!quit) {
        char outer_display[7];
        for (int i = 0; i < 6; ++i) {
            outer_display[i] = (char)toupper((unsigned char)letters[i]);
        }
        outer_display[6] = '\0';
        char center_display = (char)toupper((unsigned char)letters[6]);
        printf("Letters: %s (center %c)\n", outer_display, center_display);
        printf("Generated %zu candidate words.\n", words_upper.size);
        fflush(stdout);
    }

    if (!quit) {
        SendWordsCtx sw = {.wd = wd, .words = &words_upper};
        StepResult sr = retry_with_pause("send words", op_send_words, &sw);
        if (sr == STEP_RESULT_QUIT) quit = true;
    }

    result.session_active = wd_has_session(wd);
    result.user_quit = quit;

    word_list_free(&words_upper);
    return result;
}

// ---------- Main ----------

int main(int argc, char **argv) {
    Config config;
    if (!parse_args(argc, argv, &config)) {
        return 1;
    }

    if (config.dictionary_dir[0] == '\0') {
        fprintf(stderr, "[FATAL] Could not locate word list directory. Specify --dictionary-dir=PATH.\n");
        return 1;
    }
    if (!path_is_directory(config.dictionary_dir)) {
        fprintf(stderr, "[FATAL] Dictionary directory not found: %s\n", config.dictionary_dir);
        return 1;
    }

    WordDictionaries dicts;
    if (load_word_dictionaries(&config, &dicts) != 0) {
        free_word_dictionaries(&dicts);
        return 1;
    }

    if (dicts.massive_words.size == 0) {
        fprintf(stderr, "[FATAL] word list 'wlist_match1.txt' appears to be empty in %s\n", config.dictionary_dir);
        free_word_dictionaries(&dicts);
        return 1;
    }

    printf("Loaded word lists from %s (massive set size: %zu)\n",
           config.dictionary_dir,
           dicts.massive_words.size);
    fflush(stdout);

    curl_global_init(CURL_GLOBAL_DEFAULT);
    CurlSession session;
    char *err = NULL;
    if (curl_session_init(&session, &err) != 0) {
        fprintf(stderr, "[FATAL] %s\n", err ? err : "failed to initialise curl session");
        free(err);
        free_word_dictionaries(&dicts);
        curl_global_cleanup();
        return 1;
    }
    free(err);

    WD wd;
    wd_init(&wd, &session);
    const char *override_url = getenv("WEBDRIVER_URL");
    if (override_url && *override_url) {
        wd_set_base(&wd, override_url);
    }

    bool want_close = true;
    bool exit_program = false;
    bool need_full_setup = true;
    int attempt_index = 0;

    while (!exit_program) {
        ++attempt_index;
        bool have_session = wd_has_session(&wd);
        bool do_full_setup = need_full_setup || !have_session;
        AttemptResult attempt = run_attempt(&wd,
                                            have_session,
                                            do_full_setup,
                                            attempt_index,
                                            &config,
                                            &dicts);

        if (attempt.fatal_error) {
            fprintf(stderr, "\n[FATAL] %s\n", attempt.fatal_message ? attempt.fatal_message : "unknown error");
        }

        if (attempt.user_quit) {
            exit_program = true;
            want_close = true;
        } else {
            switch (config.stop_action) {
                case STOP_ACTION_PROMPT: {
                    char answer[32];
                    prompt_line("\nClose the browser now?  (press Enter = close)  Type 'keep' to leave it open: ",
                                answer,
                                sizeof(answer));
                    if (strcmp(answer, "keep") == 0 || strcmp(answer, "k") == 0) {
                        want_close = false;
                    } else {
                        want_close = true;
                    }
                    exit_program = true;
                    break;
                }
                case STOP_ACTION_KEEP:
                    want_close = false;
                    exit_program = true;
                    break;
                case STOP_ACTION_RERUN: {
                    want_close = false;
                    char answer[32];
                    if (attempt.fatal_error) {
                        prompt_line("Fix any issues, then press Enter to rerun, 'quit' to exit, or 'close' to close the browser and exit: ",
                                    answer,
                                    sizeof(answer));
                    } else {
                        prompt_line("Press Enter to rerun from the beginning, or type 'quit' to exit, 'close' to close the browser and exit: ",
                                    answer,
                                    sizeof(answer));
                    }
                    if (strcmp(answer, "quit") == 0 || strcmp(answer, "q") == 0) {
                        exit_program = true;
                        want_close = true;
                    } else if (strcmp(answer, "close") == 0 || strcmp(answer, "c") == 0) {
                        exit_program = true;
                        want_close = true;
                    } else {
                        exit_program = false;
                    }
                    break;
                }
            }
        }

        if (attempt.fatal_error && config.stop_action != STOP_ACTION_RERUN) {
            exit_program = true;
        }
        need_full_setup = attempt.fatal_error || !attempt.session_active;

        free(attempt.fatal_message);
    }

    if (wd_has_session(&wd) && want_close) {
        char *cleanup_err = NULL;
        wd_delete_session(&wd, &cleanup_err);
        free(cleanup_err);
    }

    curl_session_cleanup(&session);
    curl_global_cleanup();
    free_word_dictionaries(&dicts);
    return 0;
}
