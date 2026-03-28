#include <stdio.h>
#include <dlfcn.h>
#include <unistd.h>

typedef unsigned int u32;

typedef u32  (*fn_capabilities)();
typedef const char* (*fn_name)();
typedef int  (*fn_init)(u32, u32);
typedef void (*fn_uninit)();

int main() {
    void* lib = dlopen("./r_plugin_frametap.so", RTLD_LAZY);
    if (!lib) { printf("dlopen failed: %s\n", dlerror()); return 1; }

    fn_init    init    = (fn_init)   dlsym(lib, "core_plugin_init");
    fn_uninit  uninit  = (fn_uninit) dlsym(lib, "core_plugin_uninit");
    fn_name    name    = (fn_name)   dlsym(lib, "core_plugin_get_name");

    printf("[test] plugin name: %s\n", name());
    printf("[test] initializing...\n");

    if (init(2, 0xFFFFFFFF) != 0) {
        printf("[test] init failed\n");
        return 1;
    }

    printf("[test] running for 15 seconds...\n");
    sleep(30);

    uninit();
    dlclose(lib);
    printf("[test] done\n");
    return 0;
}
