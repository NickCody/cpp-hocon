#include <hocon/config.hpp>
#include <hocon/config_parse_options.hpp>
#include <hocon/config_list.hpp>
#include <hocon/config_exception.hpp>
#include <internal/default_transformer.hpp>
#include <internal/resolve_context.hpp>
#include <internal/values/config_boolean.hpp>
#include <internal/values/config_null.hpp>
#include <internal/values/config_number.hpp>
#include <internal/values/config_string.hpp>
#include <internal/values/simple_config_object.hpp>
#include <internal/parseable.hpp>
#include <internal/simple_includer.hpp>
#include <leatherman/util/environment.hpp>
#include <leatherman/locale/locale.hpp>

// Mark string for translation (alias for leatherman::locale::format)
using leatherman::locale::_;

using namespace std;

namespace hocon {

    shared_config config::parse_file_any_syntax(std::string file_basename, config_parse_options options) {
        auto source = make_shared<file_name_source>();
        return simple_includer::from_basename(move(source), move(file_basename), move(options))->to_config();
    }

    shared_config config::parse_file_any_syntax(std::string file_basename) {
        return parse_file_any_syntax(move(file_basename), config_parse_options::defaults());
    }

    shared_config config::parse_string(string s, config_parse_options options)
    {
        return parseable::new_string(move(s), move(options)).parse()->to_config();
    }

    shared_config config::parse_string(string s)
    {
        return parse_string(move(s), config_parse_options());
    }

    config::config(shared_object object) : _object(move(object)) { }

    shared_object config::root() const {
        return _object;
    }

    shared_origin config::origin() const {
        return _object->origin();
    }

    shared_config config::resolve() const {
        return resolve(config_resolve_options());
    }

    shared_config config::resolve(config_resolve_options options) const {
        return resolve_with(shared_from_this(), move(options));
    }

    shared_config config::resolve_with(shared_config source) const {
        return resolve_with(source, config_resolve_options());
    }

    shared_config config::resolve_with(shared_config source, config_resolve_options options) const {
        auto resolved = resolve_context::resolve(_object, source->_object, move(options));

        if (resolved == _object) {
            return shared_from_this();
        } else {
            return make_shared<config>(dynamic_pointer_cast<const config_object>(resolved));
        }
    }

    shared_value config::has_path_peek(string const& path_expression) const {
        path raw_path = path::new_path(path_expression);
        shared_value peeked;
        try {
            peeked = _object->peek_path(raw_path);
        } catch (config_exception& ex) {
            if (_object->get_resolve_status() == resolve_status::RESOLVED) {
                throw ex;
            }
            throw config_exception(_("{1} has not been resolved, you need to call config::resolve()", raw_path.render()));
        }
        return peeked;
    }

    bool config::has_path(string const& path_expression) const {
        shared_value peeked = has_path_peek(path_expression);
        return peeked && peeked->value_type() != config_value::type::CONFIG_NULL;
    }

    bool config::has_path_or_null(string const& path) const {
        shared_value peeked = has_path_peek(path);
        return peeked != nullptr;
    }

    bool config::is_empty() const {
        return _object->is_empty();
    }

    void config::find_paths(set<pair<string, shared_ptr<const config_value>>>& entries, path parent,
                                   shared_object obj) {
        for (auto&& entry : obj->entry_set()) {
            string elem = entry.first;
            shared_value v = entry.second;
            path new_path = path::new_key(elem);
            if (!parent.empty()) {
                new_path = new_path.prepend(parent);
            }
            if (auto object = dynamic_pointer_cast<const config_object>(v)) {
                find_paths(entries, new_path, object);
            } else if (auto null_value = dynamic_pointer_cast<const config_null>(v)) {
                // nothing; nulls are conceptually not in a config
            } else {
                entries.insert(make_pair(new_path.render(), v));
            }
        }
    }

    set<pair<string, shared_ptr<const config_value>>> config::entry_set() const {
        set<pair<string, shared_ptr<const config_value>>> entries;
        find_paths(entries, path(), _object);
        return entries;
    }

    shared_value config::throw_if_null(shared_value v, config_value::type expected, path original_path) {
        if (v->value_type() == config_value::type::CONFIG_NULL) {
            // TODO Once we decide on a way of converting the type enum to a string, pass expected type string
            throw null_exception(*(v->origin()), original_path.render());
        } else {
            return v;
        }
    }

    shared_value config::find_key(shared_object self, string const& key, config_value::type expected,
                                         path original_path) {
        return throw_if_null(find_key_or_null(self, key, expected, original_path), expected, original_path);
    }

    shared_value config::find_key_or_null(shared_object self, string const& key, config_value::type expected,
                                                 path original_path) {
        shared_value v = self->peek_assuming_resolved(key, original_path);
        if (!v) {
            throw missing_exception(original_path.render());
        }

        if (expected != config_value::type::UNSPECIFIED) {
            v = default_transformer::transform(v, expected);
        }

        if (expected != config_value::type::UNSPECIFIED &&
                v->value_type() != expected &&
                v->value_type() != config_value::type::CONFIG_NULL) {
            throw wrong_type_exception(_("{1} could not be converted to the requested type", original_path.render()));
        } else {
            return v;
        }
    }

    shared_value config::find_or_null(shared_object self, path desired_path,
                                             config_value::type expected, path original_path) {
        try {
            string key = *desired_path.first();
            path next = desired_path.remainder();
            if (next.empty()) {
                return find_key_or_null(self, key, expected, original_path);
            } else {
                shared_object o = dynamic_pointer_cast<const config_object>(
                        find_key(self, key, config_value::type::OBJECT,
                                 original_path.sub_path(0, original_path.length() - next.length())));
                return find_or_null(o, next, expected, original_path);
            }
        } catch (config_exception& ex) {
            if (self->get_resolve_status() == resolve_status::RESOLVED) {
                throw ex;
            }
            throw config_exception(_("{1} has not been resolved, you need to call config::resolve()", desired_path.render()));
        }
    }

    shared_value config::find_or_null(string const& path_expression, config_value::type expected) const {
        path raw_path = path::new_path(path_expression);
        return find_or_null(raw_path, expected, raw_path);
    }

    shared_value config::find(string const& path_expression, config_value::type expected) const {
        path raw_path = path::new_path(path_expression);
        return find(raw_path, expected, raw_path);
    }

    bool config::get_is_null(string const& path_expression) const {
        shared_value v = find_or_null(path_expression, config_value::type::UNSPECIFIED);
        return v->value_type() == config_value::type::CONFIG_NULL;
    }

    shared_ptr<const config_value> config::get_value(string const& path_expression) const {
        return find(path_expression, config_value::type::UNSPECIFIED);
    }

    bool config::get_bool(string const& path_expression) const {
        shared_value v = find(path_expression, config_value::type::BOOLEAN);
        return dynamic_pointer_cast<const config_boolean>(v)->bool_value();
    }

    int config::get_int(string const& path_expression) const {
        shared_value v = find(path_expression, config_value::type::NUMBER);
        return dynamic_pointer_cast<const config_number>(v)->int_value_range_checked(path_expression);
    }

    int64_t config::get_long(string const& path_expression) const {
        shared_value v = find(path_expression, config_value::type::NUMBER);
        return dynamic_pointer_cast<const config_number>(v)->long_value();
    }

    double config::get_double(string const& path_expression) const {
        shared_value v = find(path_expression, config_value::type::NUMBER);
        return dynamic_pointer_cast<const config_number>(v)->double_value();
    }

    string config::get_string(string const& path_expression) const {
        shared_value v = find(path_expression, config_value::type::STRING);
        return dynamic_pointer_cast<const config_string>(v)->transform_to_string();
    }

    shared_ptr<const config_object> config::get_object(string const& path_expression) const {
        return dynamic_pointer_cast<const config_object>(find(path_expression, config_value::type::OBJECT));
    }

    unwrapped_value config::get_any_ref(string const& path_expression) const {
        return find(path_expression, config_value::type::UNSPECIFIED)->unwrapped();
    }

    shared_config config::get_config(string const& path_expression) const {
        return get_object(path_expression)->to_config();
    }

    shared_list config::get_list(string const& path_expression) const {
        return dynamic_pointer_cast<const config_list>(find(path_expression, config_value::type::LIST));
    }

    vector<bool> config::get_bool_list(string const& path) const {
        return get_homogeneous_unwrapped_list<bool>(path);
    }

    std::vector<int> config::get_int_list(std::string const& path) const {
        return get_homogeneous_unwrapped_list<int>(path);
    }

    std::vector<int64_t> config::get_long_list(std::string const& path) const {
        return get_homogeneous_unwrapped_list<int64_t>(path);
    }

    std::vector<double> config::get_double_list(std::string const& path) const {
        return get_homogeneous_unwrapped_list<double>(path);
    }

    std::vector<std::string> config::get_string_list(std::string const& path) const {
        return get_homogeneous_unwrapped_list<string>(path);
    }

    std::vector<shared_object> config::get_object_list(std::string const& path) const {
        auto list = get_list(path);
        vector<shared_object> object_list;
        for (auto item : *list) {
            shared_object obj = dynamic_pointer_cast<const config_object>(item);
            if (obj == nullptr) {
                throw new config_exception(_("List does not contain only config_objects."));
            }
            object_list.push_back(obj);
        }
        return object_list;
    }

    std::vector<shared_config> config::get_config_list(std::string const& path) const {
        auto list = get_list(path);
        vector<shared_config> object_list;
        for (auto item : *list) {
            shared_config obj = dynamic_pointer_cast<const config>(item);
            if (obj == nullptr) {
                throw config_exception(_("List does not contain only configs."));
            }
            object_list.push_back(obj);
        }
        return object_list;
    }

    template<>
    std::vector<int64_t> config::get_homogeneous_unwrapped_list(std::string const& path) const {
        auto list = boost::get<std::vector<unwrapped_value>>(get_list(path)->unwrapped());
        std::vector<int64_t> long_list;
        for (auto item : list) {
            // Even if the parser stored the number as an int, we want to treat it as a long.
            try {
                long_list.push_back(boost::get<int64_t>(item));
            } catch (std::exception& ex) {
                try {
                    long_list.push_back(boost::get<int>(item));
                } catch (boost::bad_get &ex) {
                    throw config_exception(_("The list did not contain only the desired type."));
                }
            }
        }
        return long_list;
    }

    shared_value config::to_fallback_value() const {
        return _object;
    }

    shared_ptr<const config_mergeable> config::with_fallback(shared_ptr<const config_mergeable> other) const {
        if (auto newobj = dynamic_pointer_cast<const config_object>(_object->with_fallback(other))) {
            return newobj->to_config();
        } else {
            throw bug_or_broken_exception(_("Creating new object from config_object did not return a config_object"));
        }
    }

    bool config::operator==(config const &other) const {
        return _object == other._object;
    }

    shared_config config::with_value(string const& path_expression, shared_ptr<const config_value> value) const {
        path raw_path = path::new_path(path_expression);
        return make_shared<config>(root()->with_value(raw_path, value));
    }

    bool config::is_resolved() const {
        return root()->get_resolve_status() == resolve_status::RESOLVED;
    }

    shared_config config::without_path(string const& path_expression) const {
        path raw_path = path::new_path(path_expression);
        return make_shared<config>(root()->without_path(raw_path));
    }

    shared_config config::with_only_path(string const& path_expression) const {
        path raw_path = path::new_path(path_expression);
        return make_shared<config>(root()->with_only_path(raw_path));
    }

    shared_config config::at_key(shared_origin origin, string const& key) const {
        return root()->at_key(origin, key);
    }

    shared_config config::at_key(std::string const& key) const {
        return root()->at_key(key);
    }

    shared_config config::at_path(std::string const& path) const {
        return root()->at_path(path);
    }

    shared_includer config::default_includer() {
        static auto _default_includer = make_shared<simple_includer>(nullptr);
        return _default_includer;
    }

    void config::check_valid(shared_config reference, std::vector<std::string> restrict_to_paths) const {
        // TODO: implement this once resolve functionality is working
        throw runtime_error(_("Method not implemented"));
    }

    shared_value config::peek_path(path desired_path) const {
        return root()->peek_path(desired_path);
    }

    shared_value config::find_or_null(path path_expression, config_value::type expected,
            path original_path) const {
        return find_or_null(_object, path_expression, expected, original_path);
    }

    shared_value config::find(path path_expression, config_value::type expected, path original_path) const {
        return throw_if_null(find_or_null(_object, path_expression, expected, original_path), expected, original_path);
    }

    shared_object config::env_variables_as_config_object() {
        unordered_map<string, shared_value> values;
        leatherman::util::environment::each([&](string& k, string& v) {
            auto origin = make_shared<simple_config_origin>("env var " + k);
            values.emplace(k, make_shared<config_string>(origin, v, config_string_type::QUOTED));
            return true;
        });
        auto origin = make_shared<simple_config_origin>("env variables");
        return make_shared<simple_config_object>(origin, move(values), resolve_status::RESOLVED, false);
    }


}  // namespace hocon
