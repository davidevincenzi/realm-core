#include <realm/object-store/c_api/types.hpp>
#include <realm/object-store/c_api/util.hpp>

RLM_API const char* realm_get_library_version()
{
    return REALM_VERSION_STRING;
}

RLM_API void realm_get_library_version_numbers(int* out_major, int* out_minor, int* out_patch, const char** out_extra)
{
    *out_major = REALM_VERSION_MAJOR;
    *out_minor = REALM_VERSION_MINOR;
    *out_patch = REALM_VERSION_PATCH;
    *out_extra = REALM_VERSION_EXTRA;
}

RLM_API realm_t* realm_open(const realm_config_t* config)
{
    return wrap_err([&]() {
        return new shared_realm{Realm::get_shared_realm(*config)};
    });
}

RLM_API realm_t* _realm_from_native_ptr(const void* pshared_ptr, size_t n)
{
    REALM_ASSERT_RELEASE(n == sizeof(std::shared_ptr<Realm>));
    auto ptr = static_cast<const std::shared_ptr<Realm>*>(pshared_ptr);
    return new shared_realm{*ptr};
}

RLM_API bool realm_close(realm_t* realm)
{
    return wrap_err([&]() {
        (*realm)->close();
        return true;
    });
}

RLM_API bool realm_begin_write(realm_t* realm)
{
    return wrap_err([&]() {
        (*realm)->begin_transaction();
        return true;
    });
}

RLM_API bool realm_commit(realm_t* realm)
{
    return wrap_err([&]() {
        (*realm)->commit_transaction();
        return true;
    });
}

RLM_API bool realm_rollback(realm_t* realm)
{
    return wrap_err([&]() {
        (*realm)->cancel_transaction();
        return true;
    });
}

RLM_API bool realm_refresh(realm_t* realm)
{
    return wrap_err([&]() {
        (*realm)->refresh();
        return true;
    });
}

RLM_API realm_t* realm_freeze(realm_t* realm)
{
    return wrap_err([&]() {
        auto& p = **realm;
        return new realm_t{p.freeze()};
    });
}

RLM_API bool realm_compact(realm_t* realm, bool* did_compact)
{
    return wrap_err([&]() {
        auto& p = **realm;
        *did_compact = p.compact();
        return true;
    });
}
