#pragma once

#include <cassert>
#include <map>
#include <variant>

class Options {
public:
    using OptionType = std::variant<int, float, bool>;

    template <typename T>
    void Add(const char* name, T defaultValue = {}) {
        m_options[name] = defaultValue;
    }

    void LoadOptionsFile(const char* file);
    void SaveOptionsFiles(const char* file);

    template <typename T>
    T Get(const char* name) {
        if (auto iter = m_options.find(name); iter != m_options.end()) {
            return std::get<T>(iter->second);
        }
        assert(false);
        return {};
    }

    template <typename T>
    void Set(const char* name, T value) {
        auto iter = m_options.find(name);
        assert(iter != m_options.end());
        iter->second = value;
    }

private:
    std::map<std::string, OptionType> m_options;
};