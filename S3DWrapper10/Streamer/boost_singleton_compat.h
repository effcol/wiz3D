#pragma once

// Shim for boost/pool/detail/singleton.hpp, which existed in Boost 1.45 (the old
// bundled in-tree copy) but was removed in the modern vcpkg-managed Boost.
// Restores boost::details::pool::singleton_default<T> for the XML command-flow
// streamer. Scope-limited to this subsystem on purpose -- do not depend on this
// symbol from new code; prefer iz3d::Singleton.

namespace boost { namespace details { namespace pool {

template <typename T>
struct singleton_default
{
    struct object_creator
    {
        object_creator() { singleton_default<T>::instance(); }
        inline void do_nothing() const {}
    };
    static object_creator create_object;

    singleton_default() = delete;

    static T& instance()
    {
        static T obj;
        create_object.do_nothing();
        return obj;
    }
};

template <typename T>
typename singleton_default<T>::object_creator
singleton_default<T>::create_object;

}}} // namespace boost::details::pool
