#pragma once

#include <vector>
#include <string>
#include <unordered_set>
#include <unordered_map>

struct KeyEvent {
    int code;
    int value;
};

struct Pattern {
    KeyEvent ev;
    bool condition;

    Pattern(int code, int value, bool cond) : ev{code, value}, condition(cond) {
    }
};

enum Action {
    None,
    ConvertWord,
    ConvertAll
};

enum class Layout {
    EN,
    RU,
    UNKNOWN
};


class Converter {
public:
    int conv_key;
    int ls_keys[2];

    // auto-correct settings
    bool auto_enabled = false;
    int auto_min_length = 3;
    Layout current_layout = Layout::EN;

    Converter();

    ~Converter();

    bool push(int code, int value);

    Action process();

    std::vector<KeyEvent> convert(Action action) const;

    std::string get_buffer_dump() const;

    void clear_buffer();

    bool is_key(int code) const;

    bool is_shift(int code) const;

    bool is_backspace(int code) const;

    bool is_killer(int code) const;

    bool is_up(int value) const;

    bool is_down(int value) const;

    bool is_repeat(int value) const;

    // auto-correct API
    void setAutoEnabled(bool v) { auto_enabled = v; }
    void setAutoMinLength(int l) { auto_min_length = l; }
    Layout getCurrentLayout() const { return current_layout; }
    void setCurrentLayout(Layout l) { current_layout = l; }
    void toggleLayout();

    void loadDictionaries(const std::string& ruPath = "", const std::string& enPath = "");
    bool isAutoEnabled() const { return auto_enabled; }

    // mapping helpers
    std::string enForKey(int code) const;
    std::string ruForKey(int code) const;
    bool isAlphaKey(int code) const;
    bool isDelimiterKey(int code) const;

    std::string keysToEnWord(int start, int end) const;
    std::string keysToRuWord(int start, int end) const;
    std::string toLowerEn(const std::string& s) const;
    std::string toLowerRu(const std::string& s) const;

    bool isValidEnWord(const std::string& w) const;
    bool isValidRuWord(const std::string& w) const;

    bool getLastWordRange(int& start, int& end) const;
    bool shouldAutoCorrect(Layout layout) const;
    Action checkAutoTrigger(int code, int value) const;

    Layout querySystemLayout() const;

private:
    std::vector<KeyEvent> buffer_;
    std::unordered_set<std::string> en_dict_;
    std::unordered_set<std::string> ru_dict_;
    bool dicts_loaded_ = false;

    bool buffer_matches_pattern(const std::vector<Pattern> &pattern) const;
    void trim_buffer();

    bool loadDictionaryFile(const std::string& path, std::unordered_set<std::string>& dict, bool isRu);
    void initEmbeddedDictionaries();
};