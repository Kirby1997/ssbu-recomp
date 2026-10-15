/* test_dlbind.c - nncxxrt.c eh_bind_main on Linux: the module that holds fn_table found with dladdr, its handle with
 * dlopen(RTLD_NOLOAD) (nothing is loaded), game_eh_bind with dlsym; and the fallback when the table is in the executable. */
#include "tp.h"
#include <dlfcn.h>

const int *game_table_p(void);
const int exe_table[2] = {5, 6};
typedef int (*BindFn)(int);
static BindFn find_bind(const void *table_entry, const char **module) {     /* the code of eh_bind_main */
    Dl_info di;
    BindFn bind = NULL;
    if (!dladdr(table_entry, &di) || !di.dli_fname) return NULL;
    *module = di.dli_fname;
    void *dll = dlopen(di.dli_fname, RTLD_LAZY | RTLD_NOLOAD);
    if (!dll) dll = dlopen(NULL, RTLD_LAZY);
    if (!dll) return NULL;
    bind = (BindFn)dlsym(dll, "game_eh_bind");
    dlclose(dll);
    return bind;
}
int main(void) {
    const char *mod = "?";
    SECTION("table in the shared library");
    BindFn b = find_bind(&game_table_p()[0], &mod);
    CHECKF(b != NULL, "module %s: %s", mod, dlerror());
    CHECK(strstr(mod, "libdlbind.so") != NULL);
    CHECK(b && b(1) == 42);
    CHECK(dlopen("libnot_loaded_here.so", RTLD_LAZY | RTLD_NOLOAD) == NULL);
    SECTION("table in the executable (fallback: the global scope still finds the library's export)");
    b = find_bind(&exe_table[0], &mod);
    CHECK(b != NULL && b(0) == 41);
    return tp_done("test_dlbind");
}
