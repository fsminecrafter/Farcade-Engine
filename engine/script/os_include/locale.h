// The C locale only (the runtime has no locale support); Lua asks for the
// decimal point and compares strings with strcoll.
#ifndef FE_SCRIPT_LOCALE_H
#define FE_SCRIPT_LOCALE_H
struct lconv { char* decimal_point; };
static inline struct lconv* localeconv(void) {
    static struct lconv c = { (char*)"." };
    return &c;
}
#endif
