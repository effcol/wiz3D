
#ifndef SQUISH_EXPORT_H
#define SQUISH_EXPORT_H

#ifdef SQUISH_STATIC_DEFINE
#  define SQUISH_EXPORT
#  define SQUISH_NO_EXPORT
#else
#  ifndef SQUISH_EXPORT
#    ifdef squish_EXPORTS
        /* We are building this library */
#      define SQUISH_EXPORT 
#    else
        /* We are using this library */
#      define SQUISH_EXPORT 
#    endif
#  endif

#  ifndef SQUISH_NO_EXPORT
#    define SQUISH_NO_EXPORT 
#  endif
#endif

#ifndef SQUISH_DEPRECATED
#  define SQUISH_DEPRECATED __declspec(deprecated)
#endif

#ifndef SQUISH_DEPRECATED_EXPORT
#  define SQUISH_DEPRECATED_EXPORT SQUISH_EXPORT SQUISH_DEPRECATED
#endif

#ifndef SQUISH_DEPRECATED_NO_EXPORT
#  define SQUISH_DEPRECATED_NO_EXPORT SQUISH_NO_EXPORT SQUISH_DEPRECATED
#endif

/* NOLINTNEXTLINE(readability-avoid-unconditional-preprocessor-if) */
#if 0 /* DEFINE_NO_DEPRECATED */
#  ifndef SQUISH_NO_DEPRECATED
#    define SQUISH_NO_DEPRECATED
#  endif
#endif

#endif /* SQUISH_EXPORT_H */
