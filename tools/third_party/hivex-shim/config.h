/* Hand-written config for building hivex's lib/ without its autotools and gnulib tree. */
#define HAVE_MMAP 1
#define HAVE_VARIABLE_SIZE_ARRAYS 1
#ifdef __linux__
#define HAVE_ENDIAN_H 1
#endif
#define PACKAGE_VERSION "1.3.24"
#define ICONV_CONST
