#include <stdio.h>
#include <stddef.h>
#include <stdint.h>

#include "plugin_common.h"

attr_public const char *g_pluginName = "re4coop";
attr_public const char *g_pluginDesc = "RE4 coop - plugin de prueba";
attr_public const char *g_pluginAuth = "re4coop";
attr_public uint32_t g_pluginVersion = 0x00000101; // 1.01

// Archivo de log: lo copias de la PS4 a la PC y me lo pasas.
#ifndef RE4COOP_LOG_PATH
#define RE4COOP_LOG_PATH "/data/GoldHEN/re4coop_log.txt"
#endif

// Identifica esta compilacion (fecha y hora en que GitHub la hizo).
#define BUILD_ID __DATE__ " " __TIME__

// fopen() hacia caer el juego al arrancar (error CE-34878-0), asi que el log se
// escribe con las llamadas de bajo nivel de la PS4 (libkernel), que no usan
// memoria dinamica ni stdio. Se declaran aqui con otro nombre de C pero el mismo
// simbolo, para no depender de que un header las declare.
extern int  re4_open(const char *path, int flags, int mode) __asm__("sceKernelOpen");
extern long re4_write(int fd, const void *buf, unsigned long nbyte) __asm__("sceKernelWrite");
extern int  re4_close(int fd) __asm__("sceKernelClose");

#define RE4_O_WRONLY 0x0001
#define RE4_O_CREAT  0x0200
#define RE4_O_TRUNC  0x0400

// Mensajes numerados en pantalla: el ultimo numero que veas es hasta donde llego.
static void notify_step(int step, const char *text) {
    char msg[200] = {0};
    snprintf(msg, sizeof(msg), "re4coop %d: %s", step, text);
    NotifyStatic(TEX_ICON_SYSTEM, msg);
}

int32_t attr_public plugin_load(int32_t argc, const char* argv[]) {
    char text[512] = {0};
    char msg[160] = {0};
    size_t used = 0;
    int n = 0;

    final_printf("[GoldHEN] <%s\\Ver.0x%08x> %s\n", g_pluginName, g_pluginVersion, __func__);

    // 1: lo primero que se ve. Si no sale, el plugin ni cargo.
    snprintf(msg, sizeof(msg), "plugin_load inicio (%s)", BUILD_ID);
    notify_step(1, msg);

    // Armamos el texto del log en memoria (todavia sin tocar archivos).
    n = snprintf(text, sizeof(text), "==== re4coop v1.01 ====\nbuild: %s\nargc=%d\n", BUILD_ID, (int)argc);
    if (n < 0) {
        used = 0;
    } else if ((size_t)n >= sizeof(text)) {
        used = sizeof(text) - 1;
    } else {
        used = (size_t)n;
    }
    for (int32_t i = 0; argv != NULL && i < argc && i < 4 && used < sizeof(text) - 1; i++) {
        n = snprintf(text + used, sizeof(text) - used, "argv[%d]=%s\n", (int)i, argv[i] != NULL ? argv[i] : "(null)");
        if (n < 0) {
            break;
        }
        used += (size_t)n;
        if (used >= sizeof(text)) {
            used = sizeof(text) - 1;
            break;
        }
    }

    // 2: justo antes de tocar el archivo.
    notify_step(2, "antes de abrir el log");

    int fd = re4_open(RE4COOP_LOG_PATH, RE4_O_WRONLY | RE4_O_CREAT | RE4_O_TRUNC, 0777);
    if (fd < 0) {
        snprintf(msg, sizeof(msg), "no pude abrir el log (error 0x%08X)", (unsigned int)fd);
        notify_step(3, msg);
    } else {
        long w = re4_write(fd, text, (unsigned long)used);
        re4_close(fd);
        if (w < 0) {
            snprintf(msg, sizeof(msg), "log abierto pero no pude escribir (error 0x%08X)", (unsigned int)w);
        } else {
            snprintf(msg, sizeof(msg), "log escrito OK (%d bytes)", (int)w);
        }
        notify_step(3, msg);
    }

    // 4: si lo ves, el plugin termino sin problemas.
    notify_step(4, "fin: el plugin termino bien");
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
