#include <stdio.h>
#include <stddef.h>
#include <stdint.h>

#include "plugin_common.h"

attr_public const char *g_pluginName = "re4coop";
attr_public const char *g_pluginDesc = "RE4 coop - plugin de prueba";
attr_public const char *g_pluginAuth = "re4coop";
attr_public uint32_t g_pluginVersion = 0x00000100; // 1.00

// Archivo de log: lo copias de la PS4 a la PC y me lo pasas.
#ifndef RE4COOP_LOG_PATH
#define RE4COOP_LOG_PATH "/data/GoldHEN/re4coop_log.txt"
#endif

// Identifica esta compilacion (fecha y hora en que GitHub la hizo).
#define BUILD_ID __DATE__ " " __TIME__

static int g_log_started = 0;

// La primera linea de cada partida borra el log anterior; las demas se agregan.
static void log_line(const char *line) {
    FILE *f = fopen(RE4COOP_LOG_PATH, g_log_started ? "a" : "w");
    if (f == NULL) {
        return;
    }
    g_log_started = 1;
    fprintf(f, "%s\n", line);
    fclose(f);
}

int32_t attr_public plugin_load(int32_t argc, const char* argv[]) {
    char msg[160] = {0};

    final_printf("[GoldHEN] <%s\\Ver.0x%08x> %s\n", g_pluginName, g_pluginVersion, __func__);

    // Primero la notificacion: si algo fallara despues, al menos se ve.
    snprintf(msg, sizeof(msg), "RE4 coop: plugin cargado (%s)", BUILD_ID);
    NotifyStatic(TEX_ICON_SYSTEM, msg);

    log_line("==== re4coop: plugin_load ====");
    log_line("build: " BUILD_ID);

    snprintf(msg, sizeof(msg), "argc=%d", (int)argc);
    log_line(msg);
    if (argv != NULL) {
        for (int32_t i = 0; i < argc && i < 8; i++) {
            snprintf(msg, sizeof(msg), "argv[%d]=%s", (int)i, argv[i] != NULL ? argv[i] : "(null)");
            log_line(msg);
        }
    }

    log_line("fin: todo bien");
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
