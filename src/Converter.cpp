#include "Converter.h"

#include <iostream>
#include <unordered_set>
#include <fstream>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <chrono>
#include <sys/stat.h>
#include <libevdev/libevdev.h>
#include <linux/input-event-codes.h>

#ifdef HAVE_HUNSPELL
#include <hunspell/hunspell.hxx>
#endif

static const int K_UP = 0;
static const int K_DOWN = 1;
static const int K_REPEAT = 2;

static const int ANY_SHIFT = -100; // special placeholder meaning "any shift"

static const std::unordered_set<int> Keys = {
    KEY_1, KEY_2, KEY_3, KEY_4, KEY_5, KEY_6, KEY_7, KEY_8, KEY_9, KEY_0, KEY_MINUS, KEY_EQUAL,
    KEY_Q, KEY_W, KEY_E, KEY_R, KEY_T, KEY_Y, KEY_U, KEY_I, KEY_O, KEY_P, KEY_LEFTBRACE, KEY_RIGHTBRACE,
    KEY_A, KEY_S, KEY_D, KEY_F, KEY_G, KEY_H, KEY_J, KEY_K, KEY_L, KEY_SEMICOLON, KEY_APOSTROPHE, KEY_GRAVE,
    KEY_BACKSLASH, KEY_Z, KEY_X, KEY_C, KEY_V, KEY_B, KEY_N, KEY_M, KEY_COMMA, KEY_DOT, KEY_SLASH, KEY_KPASTERISK,
    KEY_SPACE, KEY_KP7, KEY_KP8, KEY_KP9, KEY_KPMINUS, KEY_KP4, KEY_KP5, KEY_KP6, KEY_KPPLUS, KEY_KP1, KEY_KP2,
    KEY_KP3, KEY_KP0, KEY_KPDOT, KEY_KPSLASH, KEY_ENTER, KEY_KPENTER
};

static const std::unordered_set<int> Shifts = {KEY_LEFTSHIFT, KEY_RIGHTSHIFT};

static const std::unordered_set<int> BufKillers = {
    BTN_LEFT, BTN_RIGHT, BTN_MIDDLE, KEY_TAB, KEY_LEFTCTRL, KEY_LEFTALT, KEY_RIGHTCTRL, KEY_RIGHTALT, KEY_HOME,
    KEY_UP, KEY_PAGEUP, KEY_LEFT, KEY_RIGHT, KEY_END, KEY_DOWN, KEY_PAGEDOWN, KEY_INSERT
};

// Keys that correspond to Cyrillic letters (33 letters) - produce letters in RU layout
static const std::unordered_set<int> AlphaKeys = {
    KEY_Q, KEY_W, KEY_E, KEY_R, KEY_T, KEY_Y, KEY_U, KEY_I, KEY_O, KEY_P,
    KEY_LEFTBRACE, KEY_RIGHTBRACE,
    KEY_A, KEY_S, KEY_D, KEY_F, KEY_G, KEY_H, KEY_J, KEY_K, KEY_L,
    KEY_SEMICOLON, KEY_APOSTROPHE, KEY_GRAVE,
    KEY_Z, KEY_X, KEY_C, KEY_V, KEY_B, KEY_N, KEY_M,
    KEY_COMMA, KEY_DOT
};

static const std::unordered_set<int> DelimiterKeys = {
    KEY_SPACE, KEY_ENTER, KEY_KPENTER
};

Converter::Converter() : conv_key(0), ls_keys{0, 0} {
}

Converter::~Converter() {
    buffer_.clear();
#ifdef HAVE_HUNSPELL
    clearHunspell();
#endif
}

// Write key event to internal buffer
// Returns true only if buffer is changed
bool Converter::push(int code, int value) {
    // clear the buffer if a "killer" key (like Tab, Ctrl, mouse button, etc.) is pressed
    if (is_killer(code) && !is_repeat(value)) {
        clear_buffer();
        return true;
    }

    // if the user-defined convert key is pressed, add it to the buffer without repeats
    if (conv_key != 0 && code == conv_key && !is_repeat(value)) {
        buffer_.push_back({code, value});
        return true;
    }

    // if a shift key is pressed, add it to the buffer without repeats
    if (is_shift(code) && !is_repeat(value)) {
        buffer_.push_back({code, value});
        return true;
    }

    // if backspace is pressed, remove the most recent non-shift key from the buffer.
    // ignore key-up events; backspace repeat is treated as key-down.
    // then remove trailing shift down/up pairs and artifacts.
    if (is_backspace(code) && !is_up(value)) {
        // non-shift key
        for (int i = buffer_.size() - 1; i >= 0; --i) {
            if (!is_shift(buffer_[i].code)) {
                buffer_.erase(buffer_.begin() + i);
                break;
            }
        }

        // shift down/up
        while (buffer_.size() >= 2) {
            if (buffer_matches_pattern({
                    {KEY_LEFTSHIFT, K_DOWN, true},
                    {KEY_LEFTSHIFT, K_UP, true}
                }) || buffer_matches_pattern({
                    {KEY_RIGHTSHIFT, K_DOWN, true},
                    {KEY_RIGHTSHIFT, K_UP, true}
                })
            ) {
                buffer_.erase(buffer_.end() - 2, buffer_.end());
            } else {
                break;
            }
        }

        // double shift down/up artifacts
        while (buffer_.size() >= 4) {
            if (buffer_matches_pattern({
                    {ANY_SHIFT, K_DOWN, true},
                    {ANY_SHIFT, K_DOWN, true},
                    {ANY_SHIFT, K_UP, true},
                    {ANY_SHIFT, K_UP, true}
                })
            ) {
                buffer_.erase(buffer_.end() - 4, buffer_.end());
            } else {
                break;
            }
        }

        return true;
    }


    // if a regular key is pressed, add it to the buffer
    // ignore up, repeat is treated as down
    if (is_key(code) && !is_up(value)) {
        buffer_.push_back({code, K_DOWN});
        return true;
    }

    return false;
}


// Process the end of the buffer to check if it's time to convert.
// If conversion is needed, also remove the processed tail.
Action Converter::process() {
    if (buffer_.empty()) {
        return None;
    };

    if (conv_key == 0) {
        // converters triggered by double shifts - default

        // 1. double shift without other shift pressed
        if (buffer_matches_pattern({
            {ANY_SHIFT, K_DOWN, false},
            {ANY_SHIFT, K_DOWN, true},
            {ANY_SHIFT, K_UP, true},
            {ANY_SHIFT, K_DOWN, true},
            {ANY_SHIFT, K_UP, true}
        })) {
            trim_buffer();
            return ConvertWord;
        }

        // 2. double shift with other shift pressed
        if (buffer_matches_pattern({
            {ANY_SHIFT, K_DOWN, true},
            {ANY_SHIFT, K_DOWN, true},
            {ANY_SHIFT, K_UP, true},
            {ANY_SHIFT, K_DOWN, true},
            {ANY_SHIFT, K_UP, true},
            {ANY_SHIFT, K_UP, true}
        })) {
            trim_buffer();
            return ConvertAll;
        }

        // 3. just switch layout with shifts, if buffer has no letters
        if (buffer_.size() == 4 &&
            buffer_matches_pattern({
                {ANY_SHIFT, K_DOWN, true},
                {ANY_SHIFT, K_UP, true},
                {ANY_SHIFT, K_DOWN, true},
                {ANY_SHIFT, K_UP, true}
            })) {
            trim_buffer();
            return ConvertAll;
        }
    } else {
        // converters triggered by a user-defined key

        // 1. user-defined key without shift pressed
        if (buffer_matches_pattern({
            {ANY_SHIFT, K_DOWN, false},
            {conv_key, K_DOWN, true},
            {conv_key, K_UP, true}
        })) {
            trim_buffer();
            return ConvertWord;
        }

        // 2. user-defined key with shift pressed
        if (buffer_matches_pattern({
            {ANY_SHIFT, K_DOWN, true},
            {conv_key, K_DOWN, true},
            {conv_key, K_UP, true},
            {ANY_SHIFT, K_UP, true}
        })) {
            trim_buffer();
            return ConvertAll;
        }

        // 3. user-defined key with shift pressed and released before conv_key
        if (buffer_matches_pattern({
            {ANY_SHIFT, K_DOWN, true},
            {conv_key, K_DOWN, true},
            {ANY_SHIFT, K_UP, true},
            {conv_key, K_UP, true}
        })) {
            trim_buffer();
            return ConvertAll;
        }

        // 4. just switch layout with user-defined key, if buffer has no letters
        if (buffer_.size() == 2 &&
            buffer_matches_pattern({
                {conv_key, K_DOWN, true},
                {conv_key, K_UP, true}
            })) {
            trim_buffer();
            return ConvertAll;
        }
    }

    return None;
}

// Returns ready-to-emit buffer.
// Doesn't modify internal buffer.
std::vector<KeyEvent> Converter::convert(Action action) const {
    std::vector<KeyEvent> result;

    // switch layout
    result.push_back({ls_keys[0], K_DOWN});
    if (ls_keys[1] != 0) {
        result.push_back({ls_keys[1], K_DOWN});
        result.push_back({ls_keys[1], K_UP});
    }
    result.push_back({ls_keys[0], K_UP});

    int start_index = 0;

    // find the last word if we are converting only the last word
    if (action == ConvertWord) {
        int i = buffer_.size() - 1;

        // skip trailing SPACE and ENTER keys
        while (i >= 0 && (buffer_[i].code == KEY_SPACE ||
                          buffer_[i].code == KEY_ENTER ||
                          buffer_[i].code == KEY_KPENTER)) {
            --i;
        }

        // move backwards until we hit a SPACE, ENTER, or the beginning
        while (i >= 0 && (buffer_[i].code != KEY_SPACE &&
                          buffer_[i].code != KEY_ENTER &&
                          buffer_[i].code != KEY_KPENTER)) {
            --i;
        }

        // start of the last word
        start_index = i + 1;
    }

    // find the start of the string if we are converting the whole buffer
    if (action == ConvertAll) {
        int i = buffer_.size() - 1;

        // skip trailing ENTER keys
        while (i >= 0 && (buffer_[i].code == KEY_ENTER ||
                          buffer_[i].code == KEY_KPENTER)) {
            --i;
        }

        // move backwards until we hit an ENTER key or reach the beginning
        while (i >= 0 && (buffer_[i].code != KEY_ENTER &&
                          buffer_[i].code != KEY_KPENTER)) {
            --i;
        }

        // start of the string
        start_index = i + 1;
    }


    // Never touch a trailing ENTER/KPENTER: it already reached the app
    // (replaying it would submit twice, backspacing it eats the fresh prompt).
    int emit_end = (int) buffer_.size() - 1;
    while (emit_end >= start_index &&
           (buffer_[emit_end].code == KEY_ENTER || buffer_[emit_end].code == KEY_KPENTER)) {
        --emit_end;
    }

    // send a backspace for each key
    for (int i = start_index; i <= emit_end; ++i) {
        if (!is_shift(buffer_[i].code)) {
            result.push_back({KEY_BACKSPACE, K_DOWN});
            result.push_back({KEY_BACKSPACE, K_UP});
        }
    }

    // replay the buffer
    for (int i = start_index; i <= emit_end; ++i) {
        result.push_back(buffer_[i]);
        if (!is_shift(buffer_[i].code)) {
            result.push_back({buffer_[i].code, K_UP});
        }
    }

    last_converted_ = buffer_;
    return result;
}

// True when current buffer holds nothing new since the last convert()
// ( trailing delimiters ignored ): already-converted content must not
// be evaluated again on the next delimiter.
bool Converter::isSameAsConverted() const {
    size_t a = buffer_.size();
    while (a > 0) {
        int c = buffer_[a - 1].code;
        if (c != KEY_SPACE && c != KEY_ENTER && c != KEY_KPENTER) break;
        --a;
    }
    size_t b = last_converted_.size();
    while (b > 0) {
        int c = last_converted_[b - 1].code;
        if (c != KEY_SPACE && c != KEY_ENTER && c != KEY_KPENTER) break;
        --b;
    }
    if (a != b) return false;
    for (size_t i = 0; i < a; ++i) {
        if (buffer_[i].code != last_converted_[i].code ||
            buffer_[i].value != last_converted_[i].value) {
            return false;
        }
    }
    return true;
}

// Returns readable buffer.
std::string Converter::get_buffer_dump() const {
    if (buffer_.empty()) return "(empty)";

    std::string out;
    for (const auto &ev: buffer_) {
        std::string state;
        switch (ev.value) {
            case K_DOWN: state = "DOWN";
                break;
            case K_UP: state = "UP";
                break;
            case K_REPEAT: state = "REPEAT";
                break;
            default: state = std::to_string(ev.value);
                break;
        }

        std::string item;
        if (const char *keyname = libevdev_event_code_get_name(EV_KEY, ev.code)) {
            item = keyname;
            if (item.rfind("KEY_", 0) == 0) {
                item = item.substr(4);
            }
        } else {
            item = std::to_string(ev.code);
        }

        if (is_shift(ev.code) || ev.code == conv_key) {
            item += "_" + state;
        }

        out += item + " ";
    }

    return out;
}


void Converter::clear_buffer() {
    buffer_.clear();
}

bool Converter::is_key(int code) const {
    return Keys.count(code) != 0;
}

bool Converter::is_shift(int code) const {
    return Shifts.count(code) != 0;
}

bool Converter::is_backspace(int code) const {
    return code == KEY_BACKSPACE;
}

bool Converter::is_killer(int code) const {
    return BufKillers.count(code) != 0;
}

bool Converter::is_up(int value) const {
    return value == K_UP;
}

bool Converter::is_down(int value) const {
    return value == K_DOWN;
}

bool Converter::is_repeat(int value) const {
    return value == K_REPEAT;
}

// Check if the tail of the buffer matches a given pattern.
// Each Pattern contains:
//   - ev.code  : key code to match (or ANY_SHIFT as a wildcard for any shift key)
//   - ev.value : key state to match (K_DOWN, K_UP, etc.)
//   - condition: expected match result (true if the event should match, false if it should *not* match)
//
// The function compares the last `pattern.size()` events in the buffer with the pattern.
// Returns true only if all events match their corresponding pattern entries according to `condition`.
bool Converter::buffer_matches_pattern(const std::vector<Pattern> &pattern) const {
    if (buffer_.size() < pattern.size()) return false;

    for (size_t i = 0; i < pattern.size(); ++i) {
        const auto &ev = buffer_[buffer_.size() - pattern.size() + i];
        const auto &p = pattern[i];
        if (((p.ev.code == ANY_SHIFT ? is_shift(ev.code) : ev.code == p.ev.code)
             && ev.value == p.ev.value) != p.condition) {
            return false;
        }
    }
    return true;
}

// Removes trailing non-key events from the buffer,
// but preserves a Shift release if it follows a regular key.
void Converter::trim_buffer() {
    while (!buffer_.empty() && !is_key(buffer_.back().code)) {
        if (is_shift(buffer_.back().code) && is_up(buffer_.back().value)) {
            if (buffer_.size() > 1 && is_key(buffer_[buffer_.size() - 2].code)) {
                break;
            }
        }
        buffer_.pop_back();
    }
}

// ================= Auto-correct section =================

void Converter::toggleLayout() {
    if (current_layout == Layout::EN) current_layout = Layout::RU;
    else if (current_layout == Layout::RU) current_layout = Layout::EN;
    // UNKNOWN stays UNKNOWN? default to EN
    else current_layout = Layout::EN;
}

std::string Converter::enForKey(int code) const {
    switch (code) {
        case KEY_Q: return "q";
        case KEY_W: return "w";
        case KEY_E: return "e";
        case KEY_R: return "r";
        case KEY_T: return "t";
        case KEY_Y: return "y";
        case KEY_U: return "u";
        case KEY_I: return "i";
        case KEY_O: return "o";
        case KEY_P: return "p";
        case KEY_LEFTBRACE: return "[";
        case KEY_RIGHTBRACE: return "]";
        case KEY_A: return "a";
        case KEY_S: return "s";
        case KEY_D: return "d";
        case KEY_F: return "f";
        case KEY_G: return "g";
        case KEY_H: return "h";
        case KEY_J: return "j";
        case KEY_K: return "k";
        case KEY_L: return "l";
        case KEY_SEMICOLON: return ";";
        case KEY_APOSTROPHE: return "'";
        case KEY_GRAVE: return "`";
        case KEY_Z: return "z";
        case KEY_X: return "x";
        case KEY_C: return "c";
        case KEY_V: return "v";
        case KEY_B: return "b";
        case KEY_N: return "n";
        case KEY_M: return "m";
        case KEY_COMMA: return ",";
        case KEY_DOT: return ".";
        case KEY_SLASH: return "/";
        case KEY_BACKSLASH: return "\\";
        case KEY_MINUS: return "-";
        case KEY_EQUAL: return "=";
        case KEY_1: return "1";
        case KEY_2: return "2";
        case KEY_3: return "3";
        case KEY_4: return "4";
        case KEY_5: return "5";
        case KEY_6: return "6";
        case KEY_7: return "7";
        case KEY_8: return "8";
        case KEY_9: return "9";
        case KEY_0: return "0";
        default: return "";
    }
}

std::string Converter::ruForKey(int code) const {
    switch (code) {
        case KEY_Q: return "й";
        case KEY_W: return "ц";
        case KEY_E: return "у";
        case KEY_R: return "к";
        case KEY_T: return "е";
        case KEY_Y: return "н";
        case KEY_U: return "г";
        case KEY_I: return "ш";
        case KEY_O: return "щ";
        case KEY_P: return "з";
        case KEY_LEFTBRACE: return "х";
        case KEY_RIGHTBRACE: return "ъ";
        case KEY_A: return "ф";
        case KEY_S: return "ы";
        case KEY_D: return "в";
        case KEY_F: return "а";
        case KEY_G: return "п";
        case KEY_H: return "р";
        case KEY_J: return "о";
        case KEY_K: return "л";
        case KEY_L: return "д";
        case KEY_SEMICOLON: return "ж";
        case KEY_APOSTROPHE: return "э";
        case KEY_GRAVE: return "ё";
        case KEY_Z: return "я";
        case KEY_X: return "ч";
        case KEY_C: return "с";
        case KEY_V: return "м";
        case KEY_B: return "и";
        case KEY_N: return "т";
        case KEY_M: return "ь";
        case KEY_COMMA: return "б";
        case KEY_DOT: return "ю";
        case KEY_SLASH: return ".";
        case KEY_BACKSLASH: return "\\";
        case KEY_MINUS: return "-";
        case KEY_EQUAL: return "=";
        case KEY_1: return "1";
        case KEY_2: return "2";
        case KEY_3: return "3";
        case KEY_4: return "4";
        case KEY_5: return "5";
        case KEY_6: return "6";
        case KEY_7: return "7";
        case KEY_8: return "8";
        case KEY_9: return "9";
        case KEY_0: return "0";
        default: return "";
    }
}

bool Converter::isAlphaKey(int code) const {
    return AlphaKeys.count(code) != 0;
}

bool Converter::isDelimiterKey(int code) const {
    return DelimiterKeys.count(code) != 0;
}

std::string Converter::toLowerEn(const std::string& s) const {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c){ return std::tolower(c); });
    return out;
}

std::string Converter::toLowerRu(const std::string& s) const {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        uint32_t cp = 0;
        size_t len = 0;
        if ((c & 0x80) == 0) { cp = c; len = 1; }
        else if ((c & 0xE0) == 0xC0) {
            if (i + 1 >= s.size()) { out.push_back(c); ++i; continue; }
            cp = ((c & 0x1F) << 6) | (static_cast<unsigned char>(s[i+1]) & 0x3F);
            len = 2;
        } else if ((c & 0xF0) == 0xE0) {
            if (i + 2 >= s.size()) { out.push_back(c); ++i; continue; }
            cp = ((c & 0x0F) << 12) | ((static_cast<unsigned char>(s[i+1]) & 0x3F) << 6) | (static_cast<unsigned char>(s[i+2]) & 0x3F);
            len = 3;
        } else if ((c & 0xF8) == 0xF0) {
            if (i + 3 >= s.size()) { out.push_back(c); ++i; continue; }
            cp = ((c & 0x07) << 18) | ((static_cast<unsigned char>(s[i+1]) & 0x3F) << 12) | ((static_cast<unsigned char>(s[i+2]) & 0x3F) << 6) | (static_cast<unsigned char>(s[i+3]) & 0x3F);
            len = 4;
        } else {
            out.push_back(c);
            ++i;
            continue;
        }
        // lower mapping
        if (cp >= 0x0410 && cp <= 0x042F) cp += 0x20;
        else if (cp == 0x0401) cp = 0x0451;
        else if (cp >= 0x0041 && cp <= 0x005A) cp += 0x20;
        // encode back
        if (cp < 0x80) out.push_back(static_cast<char>(cp));
        else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
        i += len;
    }
    return out;
}

std::string Converter::keysToEnWord(int start, int end) const {
    std::string out;
    if (start < 0 || end >= (int)buffer_.size() || start > end) return out;
    for (int i = start; i <= end; ++i) {
        int code = buffer_[i].code;
        if (is_shift(code)) continue;
        if (code == conv_key) continue;
        std::string ch = enForKey(code);
        if (!ch.empty() && isAlphaKey(code)) {
            out += ch;
        } else if (!ch.empty()) {
            // for non-alpha like numbers, include as is (but they are not part of word usually)
            // skip for word building - only alpha
        }
    }
    return out;
}

std::string Converter::keysToRuWord(int start, int end) const {
    std::string out;
    if (start < 0 || end >= (int)buffer_.size() || start > end) return out;
    for (int i = start; i <= end; ++i) {
        int code = buffer_[i].code;
        if (is_shift(code)) continue;
        if (code == conv_key) continue;
        std::string ch = ruForKey(code);
        if (!ch.empty() && isAlphaKey(code)) {
            out += ch;
        }
    }
    return out;
}

bool Converter::isValidEnWord(const std::string& w) const {
    if (w.empty()) return false;
#ifdef HAVE_HUNSPELL
    if (en_hun_) {
        // Hunspell::spell is non-const but logically const for us
        return const_cast<Hunspell*>(en_hun_)->spell(w);
    }
#endif
    return en_dict_.find(w) != en_dict_.end();
}

bool Converter::isValidRuWord(const std::string& w) const {
    if (w.empty()) return false;
#ifdef HAVE_HUNSPELL
    if (ru_hun_) {
        return const_cast<Hunspell*>(ru_hun_)->spell(w);
    }
#endif
    return ru_dict_.find(w) != ru_dict_.end();
}

bool Converter::getLastWordRange(int& start, int& end) const {
    if (buffer_.empty()) return false;
    int e = (int)buffer_.size() - 1;
    while (e >= 0 && isDelimiterKey(buffer_[e].code)) e--;
    if (e < 0) return false;
    int s = e;
    while (s >= 0 && !isDelimiterKey(buffer_[s].code)) s--;
    s++;
    // check has alpha
    bool hasAlpha = false;
    for (int i = s; i <= e; ++i) if (isAlphaKey(buffer_[i].code)) { hasAlpha = true; break; }
    if (!hasAlpha) return false;
    start = s;
    end = e;
    return true;
}

bool Converter::shouldAutoCorrect(Layout layout) const {
    if (!auto_enabled) return false;
    if (buffer_.empty()) return false;
    if (dicts_loaded_ && en_dict_.empty() && ru_dict_.empty()) return false;
    int start, end;
    if (!getLastWordRange(start, end)) return false;
    int letterCount = 0;
    for (int i = start; i <= end; ++i) if (isAlphaKey(buffer_[i].code)) letterCount++;
    if (letterCount < auto_min_length) return false;

    std::string enWord = keysToEnWord(start, end);
    std::string ruWord = keysToRuWord(start, end);
    if (enWord.empty() || ruWord.empty()) return false;

    std::string enLower = toLowerEn(enWord);
    std::string ruLower = toLowerRu(ruWord);

    bool enValid = isValidEnWord(enLower);
    bool ruValid = isValidRuWord(ruLower);

    if (enValid == ruValid) {
        // both valid or both invalid -> don't auto-correct to avoid false positives.
        // Mandatory singles policy: the 8 ambiguous EN/RU single pairs always
        // convert when typed in EN (no memory/TTL/context required).
        if (enValid && layout == Layout::EN && letterCount == 1 &&
            (enLower == "e" || enLower == "r" || enLower == "d" ||
             enLower == "f" || enLower == "j" || enLower == "z" ||
             enLower == "c" || enLower == "b")) {
            return true;
        }
        return false;
    }
    bool displayedValid, otherValid;
    if (layout == Layout::EN) {
        displayedValid = enValid;
        otherValid = ruValid;
    } else if (layout == Layout::RU) {
        displayedValid = ruValid;
        otherValid = enValid;
    } else {
        return false;
    }
    return (!displayedValid && otherValid);
}

Action Converter::checkAutoTrigger(int code, int value) const {
    if (!auto_enabled) return None;
    if (is_repeat(value) || is_up(value)) return None;
    if (!isDelimiterKey(code)) return None;
    // Already converted on a previous delimiter: do not evaluate again.
    if (isSameAsConverted()) return None;
    // Fresh system layout once per delimiter; tracked layout only as fallback.
    Layout detected = querySystemLayout();
    Layout use = (detected == Layout::UNKNOWN) ? current_layout : detected;
    if (shouldAutoCorrect(use)) {
        lastAutoDir_ = (use == Layout::EN) ? AutoDir::EN_TO_RU : AutoDir::RU_TO_EN;
        lastAutoTime_ = std::chrono::steady_clock::now();
        return ConvertWord;
    }
    return None;
}

bool Converter::autoDirExpired() const {
    auto age = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - lastAutoTime_).count();
    return age > AUTO_DIR_TTL_SEC;
}

// Count RU-only words (valid RU, invalid EN) among up to 3 complete words
// located strictly before buffer position wordStart. UNKNOWN words are
// skipped: they neither count nor stop the backward scan. BufKiller/mouse
// naturally bound the history (they clear the buffer).
int Converter::precedingRuCount(int wordStart) const {
    int count = 0;
    int examined = 0;
    int e = wordStart - 1;
    while (examined < 3 && e >= 0) {
        while (e >= 0 && isDelimiterKey(buffer_[e].code)) e--;
        if (e < 0) break;
        int we = e;
        while (e >= 0 && !isDelimiterKey(buffer_[e].code)) e--;
        int ws = e + 1;
        bool hasAlpha = false;
        for (int i = ws; i <= we; ++i) {
            if (isAlphaKey(buffer_[i].code)) { hasAlpha = true; break; }
        }
        if (!hasAlpha) continue;
        examined++;
        std::string enW = toLowerEn(keysToEnWord(ws, we));
        std::string ruW = toLowerRu(keysToRuWord(ws, we));
        if (!enW.empty() && !ruW.empty() && isValidRuWord(ruW) && !isValidEnWord(enW)) {
            count++;
        }
    }
    return count;
}

Layout Converter::querySystemLayout() const {
    FILE* fp = nullptr;
    char buf[512];

    auto trimLower = [](std::string s) {
        s.erase(0, s.find_first_not_of(" \n\r\t\"'"));
        if (!s.empty()) s.erase(s.find_last_not_of(" \n\r\t\"'") + 1);
        std::transform(s.begin(), s.end(), s.begin(), ::tolower);
        return s;
    };

    // 0. KDE Plasma: authoritative session layout via D-Bus (read-only, no side effects).
    // Runs as the desktop user to reach the session bus. Any error -> fall through.
    {
        const char* kdeCmd = "sudo -u v bash -c 'DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/$(id -u)/bus qdbus org.kde.keyboard /Layouts org.kde.KeyboardLayouts.getLayout 2>/dev/null' 2>/dev/null";
        fp = popen(kdeCmd, "r");
        if (fp) {
            if (fgets(buf, sizeof(buf), fp)) {
                std::string s = trimLower(buf);
                pclose(fp);
                try {
                    int idx = std::stoi(s);
                    if (idx == 0) return Layout::EN;
                    if (idx == 1) return Layout::RU;
                    // 2+ layouts: cannot map generically, try next backend
                } catch (...) {
                    // non-numeric output: try next backend
                }
            } else pclose(fp);
        }
    }

    // 1. Try gsettings as user v (GNOME Wayland/X11) - most reliable for GNOME
    // current holds index, sources holds list
    const char* user = "v";
    std::string cmd = std::string("sudo -u ") + user + " bash -c 'gsettings get org.gnome.desktop.input-sources current 2>/dev/null'";
    fp = popen(cmd.c_str(), "r");
    if (fp) {
        if (fgets(buf, sizeof(buf), fp)) {
            std::string curStr = trimLower(buf);
            pclose(fp);
            // also get sources to map index to layout
            std::string cmd2 = std::string("sudo -u ") + user + " bash -c 'gsettings get org.gnome.desktop.input-sources sources 2>/dev/null'";
            FILE* fp2 = popen(cmd2.c_str(), "r");
            std::string sourcesStr;
            if (fp2) {
                if (fgets(buf, sizeof(buf), fp2)) sourcesStr = trimLower(buf);
                pclose(fp2);
            }
            // parse current index - gsettings outputs "uint32 0" or "0"
            try {
                int idx = -1;
                // gsettings current is "uint32 0" -> we need last number
                std::string ns;
                size_t lastSpace = curStr.rfind(' ');
                if(lastSpace != std::string::npos){
                    ns = curStr.substr(lastSpace+1);
                    // trim
                    ns.erase(0, ns.find_first_not_of(" \n\r\t\"'"));
                    ns.erase(ns.find_last_not_of(" \n\r\t\"'")+1);
                } else {
                    // find last digit sequence
                    size_t p=curStr.size();
                    while(p>0 && !isdigit((unsigned char)curStr[p-1])) --p;
                    size_t q=p;
                    while(q>0 && isdigit((unsigned char)curStr[q-1])) --q;
                    ns = curStr.substr(q, p-q);
                }
                if(!ns.empty()) idx=std::stoi(ns);
                if(idx==-1) throw std::invalid_argument("no idx");
                // sourcesStr like "[('xkb', 'ru'), ('xkb', 'us')]" -> find nth 'xkb'
                // simple: find occurrences of "'ru'" "'us'" "'en'"
                std::vector<std::string> layouts;
                size_t pos=0;
                while ((pos=sourcesStr.find("'xkb'",pos))!=std::string::npos) {
                    size_t q1=sourcesStr.find("'",pos+5);
                    if(q1==std::string::npos) break;
                    size_t q2=sourcesStr.find("'",q1+1);
                    if(q2==std::string::npos) break;
                    // Actually format is ('xkb', 'ru') -> we need second quoted value
                    size_t comma=sourcesStr.find(",",pos);
                    size_t firstQuote=sourcesStr.find("'",comma);
                    size_t secondQuote=sourcesStr.find("'",firstQuote+1);
                    if(firstQuote!=std::string::npos && secondQuote!=std::string::npos){
                        std::string lay=sourcesStr.substr(firstQuote+1, secondQuote-firstQuote-1);
                        layouts.push_back(trimLower(lay));
                    }
                    pos=secondQuote+1;
                    if(layouts.size()>5) break;
                }
                if(idx>=0 && idx < (int)layouts.size()){
                    std::string lay=layouts[idx];
                    if(lay.find("ru")!=std::string::npos) return Layout::RU;
                    if(lay.find("us")!=std::string::npos || lay.find("en")!=std::string::npos) return Layout::EN;
                }
            } catch(...){
                // fallback: if curStr contains ru/en directly (older gsettings)
                if(curStr.find("ru")!=std::string::npos) return Layout::RU;
                if(curStr.find("us")!=std::string::npos || curStr.find("en")!=std::string::npos) return Layout::EN;
            }
        } else pclose(fp);
    }

    // 2. Try mru-sources as user (GNOME 40+)
    cmd = std::string("sudo -u ") + user + " bash -c 'gsettings get org.gnome.desktop.input-sources mru-sources 2>/dev/null | head -n1'";
    fp = popen(cmd.c_str(), "r");
    if (fp) {
        if (fgets(buf, sizeof(buf), fp)) {
            std::string s = trimLower(buf);
            pclose(fp);
            // mru first element is current? e.g., "[('xkb', 'us'), ('xkb', 'ru')]" -> first is us
            if(s.find("'ru'")!=std::string::npos){
                size_t posRu=s.find("'ru'");
                size_t posUs=s.find("'us'");
                if(posRu!=std::string::npos && (posUs==std::string::npos || posRu < posUs)) return Layout::RU;
                if(posUs!=std::string::npos && (posRu==std::string::npos || posUs < posRu)) return Layout::EN;
            }
        } else pclose(fp);
    }

    // 3. Try setxkbmap as user with DISPLAY
    fp = popen("sudo -u v bash -c 'DISPLAY=:0 setxkbmap -query 2>/dev/null | grep -E \"layout|variant\"' 2>/dev/null", "r");
    if (fp) {
        std::string out;
        while (fgets(buf, sizeof(buf), fp)) out+=buf;
        pclose(fp);
        out=trimLower(out);
        // Not reliable for current, but check if layout list order matches? ignore
    }

    // 4. Try xkblayout-state as user
    fp = popen("sudo -u v bash -c 'DISPLAY=:0 xkblayout-state print %s 2>/dev/null' 2>/dev/null", "r");
    if (fp) {
        if (fgets(buf, sizeof(buf), fp)) {
            std::string s = trimLower(buf);
            pclose(fp);
            if(s.find("ru")!=std::string::npos) return Layout::RU;
            if(s.find("us")!=std::string::npos || s.find("en")!=std::string::npos) return Layout::EN;
        } else pclose(fp);
    }

    // 5. Try xkb-switch as user
    fp = popen("sudo -u v bash -c 'DISPLAY=:0 xkb-switch 2>/dev/null' 2>/dev/null", "r");
    if (fp) {
        if (fgets(buf, sizeof(buf), fp)) {
            std::string s = trimLower(buf);
            pclose(fp);
            if(s.find("ru")!=std::string::npos) return Layout::RU;
            if(s.find("us")!=std::string::npos || s.find("en")!=std::string::npos) return Layout::EN;
        } else pclose(fp);
    }

    // 6. Fallback to root gsettings (if running as user session)
    fp = popen("gsettings get org.gnome.desktop.input-sources current 2>/dev/null", "r");
    if (fp) {
        if (fgets(buf, sizeof(buf), fp)) {
            std::string s = trimLower(buf);
            pclose(fp);
            // try to interpret as index or layout
            if(s=="0") {
                // need sources, but assume 0 is ru if sources[0] is ru (common)
                // check sources
                FILE* fp2=popen("gsettings get org.gnome.desktop.input-sources sources 2>/dev/null", "r");
                if(fp2){
                    char buf2[512];
                    if(fgets(buf2,sizeof(buf2),fp2)){
                        std::string src=trimLower(buf2);
                        pclose(fp2);
                        // if sources starts with ru, idx0=ru
                        if(src.find("'ru'")!=std::string::npos){
                            size_t posRu=src.find("'ru'");
                            size_t posUs=src.find("'us'");
                            if(posRu < posUs) return Layout::RU;
                            else return Layout::EN;
                        }
                    } else pclose(fp2);
                }
            }
            if(s=="1") return Layout::EN; // guess
            if(s.find("ru")!=std::string::npos) return Layout::RU;
            if(s.find("us")!=std::string::npos || s.find("en")!=std::string::npos) return Layout::EN;
        } else pclose(fp);
    }

    // 7. Try xkblayout-state as root (X11)
    fp = popen("xkblayout-state print %s 2>/dev/null", "r");
    if (fp) {
        if (fgets(buf, sizeof(buf), fp)) {
            std::string s = trimLower(buf);
            pclose(fp);
            if(s.find("ru")!=std::string::npos) return Layout::RU;
            if(s.find("us")!=std::string::npos || s.find("en")!=std::string::npos) return Layout::EN;
        } else pclose(fp);
    }
    fp = popen("xkb-switch 2>/dev/null", "r");
    if (fp) {
        if (fgets(buf, sizeof(buf), fp)) {
            std::string s = trimLower(buf);
            pclose(fp);
            if(s.find("ru")!=std::string::npos) return Layout::RU;
            if(s.find("us")!=std::string::npos || s.find("en")!=std::string::npos) return Layout::EN;
        } else pclose(fp);
    }
    return Layout::UNKNOWN;
}

bool Converter::loadDictionaryFile(const std::string& path, std::unordered_set<std::string>& dict, bool isRu) {
    std::ifstream file(path);
    if (!file.is_open()) return false;
    std::string line;
    bool firstLine = true;
    size_t inserted = 0;
    while (std::getline(file, line)) {
        if (line.empty()) continue;
        if (firstLine) {
            firstLine = false;
            // check if numeric count
            bool isNum = true;
            for (char c: line) if (!std::isdigit(c) && !std::isspace(c)) { isNum = false; break; }
            std::string trimmed = line;
            trimmed.erase(0, trimmed.find_first_not_of(" \t\r\n"));
            trimmed.erase(trimmed.find_last_not_of(" \t\r\n")+1);
            if (isNum && !trimmed.empty()) {
                try {
                    std::stoi(trimmed);
                    continue;
                } catch(...) {}
            }
            // fall through to process this line as word
        }
        size_t slash = line.find('/');
        std::string word = (slash == std::string::npos) ? line : line.substr(0, slash);
        // trim
        size_t a = word.find_first_not_of(" \t\r\n");
        if (a == std::string::npos) continue;
        size_t b = word.find_last_not_of(" \t\r\n");
        word = word.substr(a, b - a + 1);
        if (word.empty()) continue;
        if (word.size() < 2) continue;
        // filter words containing digits
        bool hasDigit = false;
        for (unsigned char c: word) if (std::isdigit(c)) { hasDigit = true; break; }
        if (hasDigit) continue;
        std::string lower = isRu ? toLowerRu(word) : toLowerEn(word);
        // for ru, check length in characters (approx)
        if (lower.empty()) continue;
        dict.insert(lower);
        inserted++;
        if (inserted > 250000) break; // safety cap
    }
    return inserted > 0;
}

void Converter::initEmbeddedDictionaries() {
    static const char* en_words[] = {
        "hello","world","test","keyboard","layout","language","computer","program","system",
        "time","people","water","food","house","family","work","school","university","book",
        "today","tomorrow","yesterday","now","here","there","very","little","big","small",
        "good","bad","new","old","first","second","third","red","blue","green","black","white",
        "speak","write","read","do","go","know","understand","want","can","need","love","life",
        "hand","head","city","country","street","shop","money","friend","dog","cat","car","window",
        "door","table","chair","phone","internet","message","word","text","letter","easy","switcher",
        "correct","convert","auto","manual","press","key","type","write","read","input","output",
        "the","and","for","you","with","this","that","have","from","they","will","one","all","would",
        "there","their","what","about","which","when","make","like","time","just","know","take","people",
        "into","year","your","good","some","could","them","see","other","than","then","now","look","only",
        "come","its","over","think","also","back","after","use","two","how","our","work","first","well",
        "way","even","new","want","because","any","these","give","day","most","us","is","are","was","were",
        "be","been","has","had","will","would","can","could","should","may","might","must","shall",
        "privet","ghost","hello","example","sample","demo","quick","brown","fox","jumps","over","lazy","dog",
        nullptr
    };
    static const char* ru_words[] = {
        "привет","мир","спасибо","пожалуйста","здравствуйте","до","свидания","да","нет","хорошо","плохо",
        "большой","маленький","человек","люди","время","день","ночь","утро","вечер","работа","дом","квартира",
        "семья","мама","папа","брат","сестра","любовь","жизнь","рука","нога","голова","глаз","город","страна",
        "язык","слово","текст","письмо","книга","школа","университет","вода","еда","стол","стул","окно","дверь",
        "машина","улица","магазин","деньги","друг","подруга","собака","кошка","клавиатура","раскладка","программа",
        "компьютер","интернет","телефон","сообщение","сегодня","завтра","вчера","сейчас","потом","здесь","там",
        "очень","немного","быстро","медленно","легко","трудно","новый","старый","молодой","красный","синий","зеленый",
        "черный","белый","первый","второй","третий","говорить","писать","читать","делать","идти","ехать","знать",
        "понимать","хотеть","мочь","нужно","можно","нельзя","почему","когда","где","кто","что","как","который",
        "этот","тот","мой","твой","его","ее","наш","ваш","их","я","ты","он","она","мы","вы","они","меня","тебя",
        "будет","был","была","были","есть","стать","иметь","делать","время","человек","год","рука","дело","жизнь",
        "ребенок","голова","дом","друг","слово","место","лицо","дверь","образ","господин","земля","час","стол","вода",
        "отец","работа","случай","нога","система","вид","город","вопрос","конец","машина","история","вечер","книга",
        "здравствуй","пока","утро","вечер","ночь","утром","вечером","спасибо","пожалуйста","извините","подскажите",
        nullptr
    };
    for (int i=0; en_words[i]; ++i) {
        std::string w = toLowerEn(en_words[i]);
        en_dict_.insert(w);
    }
    for (int i=0; ru_words[i]; ++i) {
        std::string w = toLowerRu(ru_words[i]);
        ru_dict_.insert(w);
    }
}

void Converter::loadDictionaries(const std::string& ruPath, const std::string& enPath) {
#ifdef HAVE_HUNSPELL
    clearHunspell();
    en_dict_.clear();
    ru_dict_.clear();

    bool ruHun = false, enHun = false;
    auto tryHunspell = [&](const std::string& dicPath, Hunspell* &hun) -> bool {
        if (dicPath.empty()) return false;
        std::string affPath = dicPath;
        size_t pos = affPath.rfind(".dic");
        if (pos != std::string::npos) affPath.replace(pos, 4, ".aff");
        else affPath += ".aff";
        struct stat st;
        if (stat(affPath.c_str(), &st) != 0) return false;
        if (stat(dicPath.c_str(), &st) != 0) return false;
        try {
            hun = new Hunspell(affPath.c_str(), dicPath.c_str());
            return true;
        } catch (...) {
            if (hun) { delete hun; hun = nullptr; }
            return false;
        }
    };

    if (!ruPath.empty()) ruHun = tryHunspell(ruPath, ru_hun_);
    if (!enPath.empty()) enHun = tryHunspell(enPath, en_hun_);

    if (!ruHun) {
        if (tryHunspell("/usr/share/hunspell/ru_RU.dic", ru_hun_)) ruHun = true;
        else if (tryHunspell("/usr/share/hunspell/ru.dic", ru_hun_)) ruHun = true;
    }
    if (!enHun) {
        if (tryHunspell("/usr/share/hunspell/en_US.dic", en_hun_)) enHun = true;
        else if (tryHunspell("/usr/share/hunspell/en_GB.dic", en_hun_)) enHun = true;
    }

    // flat fallback if hunspell not available for a language
    bool ruLoaded = ruHun;
    bool enLoaded = enHun;
    if (!ruLoaded) {
        if (!ruPath.empty()) ruLoaded = loadDictionaryFile(ruPath, ru_dict_, true);
        if (!ruLoaded) {
            if (loadDictionaryFile("/usr/share/hunspell/ru_RU.dic", ru_dict_, true)) ruLoaded = true;
            else if (loadDictionaryFile("/usr/share/hunspell/ru.dic", ru_dict_, true)) ruLoaded = true;
        }
    }
    if (!enLoaded) {
        if (!enPath.empty()) enLoaded = loadDictionaryFile(enPath, en_dict_, false);
        if (!enLoaded) {
            if (loadDictionaryFile("/usr/share/hunspell/en_US.dic", en_dict_, false)) enLoaded = true;
            else if (loadDictionaryFile("/usr/share/hunspell/en_GB.dic", en_dict_, false)) enLoaded = true;
            else if (loadDictionaryFile("/usr/share/dict/words", en_dict_, false)) enLoaded = true;
            else if (loadDictionaryFile("/usr/share/dict/american-english", en_dict_, false)) enLoaded = true;
        }
    }
    // always ensure embedded fallback for common words (checked second after hunspell)
    initEmbeddedDictionaries();
    dicts_loaded_ = true;
#else
    bool ruLoaded = false, enLoaded = false;
    if (!ruPath.empty()) ruLoaded = loadDictionaryFile(ruPath, ru_dict_, true);
    if (!enPath.empty()) enLoaded = loadDictionaryFile(enPath, en_dict_, false);

    if (!ruLoaded) {
        if (loadDictionaryFile("/usr/share/hunspell/ru_RU.dic", ru_dict_, true)) ruLoaded = true;
        else if (loadDictionaryFile("/usr/share/hunspell/ru.dic", ru_dict_, true)) ruLoaded = true;
    }
    if (!enLoaded) {
        if (loadDictionaryFile("/usr/share/hunspell/en_US.dic", en_dict_, false)) enLoaded = true;
        else if (loadDictionaryFile("/usr/share/hunspell/en_GB.dic", en_dict_, false)) enLoaded = true;
        else if (loadDictionaryFile("/usr/share/dict/words", en_dict_, false)) enLoaded = true;
        else if (loadDictionaryFile("/usr/share/dict/american-english", en_dict_, false)) enLoaded = true;
    }
    if (ru_dict_.empty() || en_dict_.empty()) {
        initEmbeddedDictionaries();
    } else {
        initEmbeddedDictionaries();
    }
    dicts_loaded_ = true;
    if (ru_dict_.size() < 100) initEmbeddedDictionaries();
#endif
}

#ifdef HAVE_HUNSPELL
void Converter::clearHunspell() {
    if (en_hun_) { delete en_hun_; en_hun_ = nullptr; }
    if (ru_hun_) { delete ru_hun_; ru_hun_ = nullptr; }
}
#endif
