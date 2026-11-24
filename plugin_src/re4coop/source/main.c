#include <stdio.h>
#include <stddef.h>
#include <stdint.h>

#include "plugin_common.h"

attr_public const char *g_pluginName = "re4coop";
attr_public const char *g_pluginDesc = "RE4 coop - plugin de prueba";
attr_public const char *g_pluginAuth = "re4coop";
attr_public uint32_t g_pluginVersion = 0x00000101; // 1.01

#ifndef RE4COOP_LOG_PATH
#define RE4COOP_LOG_PATH "/data/GoldHEN/re4coop_log.txt"
#endif

#define BUILD_ID __DATE__ " " __TIME__

// Notificacion numerada: la ultima que veas antes de que el juego cierre
// indica hasta donde llego plugin_load.
static void step(int n, const char *what) {
    char msg[96] = {0};
    snprintf(msg, sizeof(msg), "RE4 coop [%d]: %s", n, what);
    NotifyStatic(TEX_ICON_SYSTEM, msg);
}

int32_t attr_public plugin_load(int32_t argc, const char* argv[]) {
    final_printf("[GoldHEN] <%s\\Ver.0x%08x> %s\n", g_pluginName, g_pluginVersion, __func__);

    step(1, "plugin_load inicio (" BUILD_ID ")");
    step(2, "antes de fopen");

    FILE *f = fopen(RE4COOP_LOG_PATH, "w");
    if (f == NULL) {
        step(3, "fopen devolvio NULL");
        return 0;
    }
    step(3, "fopen OK");

    fprintf(f, "build: %s\nargc=%d\nfin: todo bien\n", BUILD_ID, (int)argc);
    fclose(f);

    step(4, "log escrito, fin de plugin_load");
    return 0;
}

int32_t attr_public plugin_unload(int32_t argc, const char* argv[]) {
    final_printf("[GoldHEN] <%s\\Ver.0x%08x> %s\n", g_pluginName, g_pluginVersion, __func__);
    return 0;
}

s32 attr_module_hidden module_start(s64 argc, const void *args)
{
    return 0;
}

s32 attr_module_hidden module_stop(s64 argc, const void *args)
{
    return 0;
}
