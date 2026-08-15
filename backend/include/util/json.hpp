#pragma once

#include <boost/describe.hpp>
#include <boost/json.hpp>
#include <boost/mp11.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <initializer_list>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace revlm
{

/** boost::json::value with default `{}` and chained operator[]. */
class json : public boost::json::value {
public:
    json()
        : boost::json::value(boost::json::object{})
    {
    }

    using boost::json::value::value;

    // Base copy/move ctors are not inherited by `using`, so a boost::json::value
    // cannot otherwise be turned back into a json. Without these, under GCC +
    // Boost 1.83 the braced `json{ value }` picks the inherited
    // `value(std::initializer_list<value_ref>)` (value converts to value_ref)
    // and silently wraps the object into a one-element array.
    json(const boost::json::value &other)
        : boost::json::value(other)
    {
    }

    json(boost::json::value &&other)
        : boost::json::value(std::move(other))
    {
    }

    json(std::nullptr_t)
        : boost::json::value(nullptr)
    {
    }

    json(std::string_view s)
        : boost::json::value(std::string{ s })
    {
    }

    json(const std::string &s)
        : boost::json::value(s)
    {
    }

    json(const char *s)
        : boost::json::value(s == nullptr ? boost::json::value(nullptr) : boost::json::value(s))
    {
    }

    static json null()
    {
        return json(nullptr);
    }

    static json array(std::initializer_list<json> items = {})
    {
        boost::json::array a;
        a.reserve(items.size());
        for (const auto &item : items) {
            a.push_back(item);
        }
        return json(boost::json::value(std::move(a)));
    }

    static std::optional<json> parse(std::string_view text)
    {
        boost::system::error_code ec;
        boost::json::value value = boost::json::parse(boost::json::string_view{ text.data(), text.size() }, ec);
        if (!ec) {
            return json(std::move(value));
        }
        const std::size_t start = text.find('{');
        const std::size_t end = text.rfind('}');
        if (start == std::string_view::npos || end == std::string_view::npos || end <= start) {
            return std::nullopt;
        }
        value = boost::json::parse(boost::json::string_view{ text.data() + start, end - start + 1 }, ec);
        if (ec) {
            return std::nullopt;
        }
        return json(std::move(value));
    }

    json &operator[](std::string_view key)
    {
        if (!is_object()) {
            *this = boost::json::object{};
        }
        return static_cast<json &>(as_object()[std::string{ key }]);
    }

    json &operator[](std::size_t index)
    {
        if (!is_array()) {
            *this = boost::json::array{};
        }
        auto &a = as_array();
        if (a.size() <= index) {
            a.resize(index + 1);
        }
        return static_cast<json &>(a[index]);
    }

    json operator[](std::string_view key) const
    {
        if (!is_object()) {
            return null();
        }
        const auto *v = as_object().if_contains(key);
        return v == nullptr ? null() : json(*v);
    }

    json operator[](std::size_t index) const
    {
        if (!is_array() || index >= as_array().size()) {
            return null();
        }
        return json(as_array()[index]);
    }

    bool contains(std::string_view key) const
    {
        return is_object() && as_object().if_contains(key) != nullptr;
    }

    std::size_t size() const
    {
        if (is_object()) {
            return as_object().size();
        }
        if (is_array()) {
            return as_array().size();
        }
        if (is_string()) {
            return boost::json::value::as_string().size();
        }
        return 0;
    }

    void erase(std::string_view key)
    {
        if (is_object()) {
            as_object().erase(key);
        }
    }

    void push_back(json v)
    {
        if (!is_array()) {
            *this = boost::json::array{};
        }
        as_array().push_back(std::move(v));
    }

    std::vector<std::string> keys() const
    {
        std::vector<std::string> out;
        if (!is_object()) {
            return out;
        }
        out.reserve(as_object().size());
        for (const auto &field : as_object()) {
            out.emplace_back(field.key());
        }
        return out;
    }

    std::string dump() const
    {
        return boost::json::serialize(static_cast<const boost::json::value &>(*this));
    }

    // Optional scalar extractors (hide throwing base as_* references for call-site convenience).
    std::optional<long long> as_int64() const
    {
        if (is_int64()) {
            return boost::json::value::as_int64();
        }
        if (is_uint64()) {
            const auto u = boost::json::value::as_uint64();
            if (u > static_cast<std::uint64_t>(std::numeric_limits<long long>::max())) {
                return std::nullopt;
            }
            return static_cast<long long>(u);
        }
        return std::nullopt;
    }

    std::optional<std::string> as_string() const
    {
        if (!is_string()) {
            return std::nullopt;
        }
        const auto &s = boost::json::value::as_string();
        return std::string{ s.data(), s.size() };
    }

    std::optional<bool> as_bool() const
    {
        if (!is_bool()) {
            return std::nullopt;
        }
        return boost::json::value::as_bool();
    }

    std::optional<double> as_double() const
    {
        if (is_double()) {
            return boost::json::value::as_double();
        }
        if (const auto i = as_int64()) {
            return static_cast<double>(*i);
        }
        return std::nullopt;
    }
};

inline std::string serialize(const json &v)
{
    return v.dump();
}

// ---------- struct <-> JSON (types described with BOOST_DESCRIBE_STRUCT) ----------

/**
 * Outbound: struct -> JSON. Field names and order come from the describe field
 * list, so they track the struct declaration verbatim; no field is copied by hand.
 */
template <class T> json to_json(const T &v)
{
    return json(boost::json::value_from(v));
}

/**
 * Inbound: JSON -> struct, strictly deconstructed. Every described field must be
 * present; nothing is silently defaulted. An empty string or a zero that slipped
 * in through a defaulted field travels far from where it was introduced before
 * anyone notices, so this refuses instead.
 *
 * Intended for hand-authored data (plugin.json and the like, where a half-finished
 * edit or a typo is the normal case), not for data a plugin emits at runtime --
 * that is the plugin's own contract and the core does not police it.
 *
 * Failure is a return value, never an exception: the caller has to hand the error
 * text back to the user, and an exception would have to cross a C ABI boundary to
 * get there (ADR 0008).
 *
 * Boost's own error is a single "source composite size does not match target size"
 * that names no field. The field list is already available, so compare against it
 * and report every missing key at once.
 *
 * Hidden from the ODB compiler: its C++ frontend predates std::expected, and it
 * only ever needs to see the persistent classes, never this.
 */
#ifndef ODB_COMPILER
template <class T> std::expected<T, std::string> strict_from(const json &v, std::string_view what)
{
    if (auto parsed = boost::json::try_value_to<T>(static_cast<const boost::json::value &>(v))) {
        return std::move(*parsed);
    }

    std::vector<std::string> missing;
    if (v.is_object()) {
        using members = boost::describe::describe_members<T, boost::describe::mod_public>;
        boost::mp11::mp_for_each<members>([&](auto D) {
            if (v.as_object().if_contains(D.name) == nullptr) {
                missing.emplace_back(D.name);
            }
        });
    }

    std::string message{ what };
    if (missing.empty()) {
        return std::unexpected(message + ": field types do not match - " + v.dump());
    }
    message += ": missing required keys [";
    for (std::size_t i = 0; i < missing.size(); ++i) {
        if (i != 0) {
            message += ", ";
        }
        message += missing[i];
    }
    return std::unexpected(message + "]");
}
#endif // ODB_COMPILER

} // namespace revlm
