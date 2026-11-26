#include <stdio.h>
#include <stddef.h>
#include <stdint.h>

#include "plugin_common.h"

attr_public const char *g_pluginName = "re4coop";
attr_public const char *g_pluginDesc = "RE4 coop - reconocimiento";
attr_public const char *g_pluginAuth = "re4coop";
attr_public uint32_t g_pluginVersion = 0x00000102; // 1.02

// Archivo de log: lo copias de la PS4 a la PC y me lo pasas.
#ifndef RE4COOP_LOG_PATH
#define RE4COOP_LOG_PATH "/data/GoldHEN/re4coop_log.txt"
#endif

// Identifica esta compilacion (fecha y hora en que GitHub la hizo).
#define BUILD_ID __DATE__ " " __TIME__

// Llamadas de bajo nivel de la PS4 (libkernel). No usan memoria dinamica ni stdio
// (fopen hacia caer el juego al arrancar). Se declaran con otro nombre de C pero
// el mismo simbolo, para no depender de que un header las declare.
extern int  re4_open(const char *path, int flags, int mode) __asm__("sceKernelOpen");
extern long re4_write(int fd, const void *buf, unsigned long nbyte) __asm__("sceKernelWrite");
extern int  re4_close(int fd) __asm__("sceKernelClose");
extern int  re4_module_list(int *array, unsigned long max, unsigned long *count) __asm__("sceKernelGetModuleList");
extern int  re4_module_info(int handle, void *info) __asm__("sceKernelGetModuleInfo");

#define RE4_O_WRONLY 0x0001
#define RE4_O_CREAT  0x0200
#define RE4_O_TRUNC  0x0400

// Informacion de un modulo (el juego es el modulo 0, "eboot.bin").
typedef struct {
    void *address;
    uint32_t size;
    int32_t prot;      // 1 = lectura, 2 = escritura, 4 = ejecucion
} Re4Segment;

typedef struct {
    unsigned long size;
    char name[256];
    Re4Segment seg[4];
    uint32_t seg_count;
    uint8_t fingerprint[20];
} Re4ModuleInfo;

// ---------------------------------------------------------------- log en archivo

static int g_fd = -1;
static unsigned long g_total = 0;
static char g_buf[8192];
static size_t g_len = 0;

static void log_flush(void) {
    if (g_fd >= 0 && g_len > 0) {
        long w = re4_write(g_fd, g_buf, (unsigned long)g_len);
        if (w > 0) {
            g_total += (unsigned long)w;
        }
    }
    g_len = 0;
}

static void log_raw(const char *s, size_t n) {
    if (n > sizeof(g_buf)) {
        n = sizeof(g_buf);
    }
    if (g_len + n > sizeof(g_buf)) {
        log_flush();
    }
    for (size_t i = 0; i < n; i++) {
        g_buf[g_len++] = s[i];
    }
}

#define LOGF(...) \
    do { \
        char _t[400]; \
        int _n = snprintf(_t, sizeof(_t), __VA_ARGS__); \
        if (_n > 0) { \
            if ((size_t)_n >= sizeof(_t)) { \
                _n = (int)sizeof(_t) - 1; \
            } \
            log_raw(_t, (size_t)_n); \
        } \
    } while (0)

// Mensajes numerados en pantalla: el ultimo numero que veas es hasta donde llego.
static void notify_step(int step, const char *text) {
    char msg[200] = {0};
    snprintf(msg, sizeof(msg), "re4coop %d: %s", step, text);
    NotifyStatic(TEX_ICON_SYSTEM, msg);
}

// ---------------------------------------------------------------- datos del escaneo

#define MAX_NAMES 4096
#define MAX_VT    8192
#define MAX_INT   64
#define MAX_REFS  6

typedef struct { uintptr_t addr; int len; } NameEnt;           // texto del nombre de clase
typedef struct { uintptr_t addr; int name; } TinfoEnt;         // typeinfo -> indice del nombre
typedef struct { uintptr_t addr; int tinfo; int nfuncs; } VtEnt; // vtable -> indice del typeinfo

static NameEnt g_names[MAX_NAMES];
static int g_nnames;
static TinfoEnt g_ti[MAX_NAMES];
static int g_nti;
static VtEnt g_vt[MAX_VT];
static int g_nvt;

typedef struct { const unsigned char *p; size_t n; int prot; } Seg;
static Seg g_seg[4];          // segmentos legibles del juego, ordenados por direccion
static int g_nseg;
static uintptr_t g_base;      // direccion del primer segmento (normalmente 0x400000)
static uintptr_t g_code_lo;   // rango de codigo (segmentos ejecutables)
static uintptr_t g_code_hi;

static int g_int_vt[MAX_INT];                 // vtables de clases de interes
static int g_nint;
static uintptr_t g_refs[MAX_INT][MAX_REFS];   // sitios del codigo que usan esa vtable
static int g_nrefs[MAX_INT];

static const char *const g_interest[] = { "Player", "Camera", "Cam", "Ashley", "Pad", "Leon" };
#define N_INTEREST ((int)(sizeof(g_interest) / sizeof(g_interest[0])))

// ---------------------------------------------------------------- utilidades

static int re4_is_digit(unsigned char c) {
    return c >= '0' && c <= '9';
}

static int re4_is_ident(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

static int re4_contains(const char *s, const char *needle) {
    for (; *s != 0; s++) {
        const char *a = s;
        const char *b = needle;
        while (*a != 0 && *b != 0 && *a == *b) {
            a++;
            b++;
        }
        if (*b == 0) {
            return 1;
        }
    }
    return 0;
}

static uint64_t re4_rd64(const unsigned char *p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) {
        v = (v << 8) | p[i];
    }
    return v;
}

static int32_t re4_rd32(const unsigned char *p) {
    uint32_t v = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    return (int32_t)v;
}

// Nombre "mangleado" de clase, como 7cPlayer o N3mtl7CObjectE.
// Devuelve su largo (sin el NUL final) si es valido, o 0 si no lo es.
static size_t re4_parse_mangled(const unsigned char *p, size_t avail) {
    size_t i = 0;
    int nested = 0;
    int segs = 0;

    if (avail < 4) {
        return 0;
    }
    if (p[0] == 'N') {
        nested = 1;
        i = 1;
    }
    for (;;) {
        size_t num = 0;
        size_t digits = 0;
        while (i < avail && re4_is_digit(p[i]) && digits < 4) {
            num = num * 10 + (size_t)(p[i] - '0');
            i++;
            digits++;
        }
        if (digits == 0 || num == 0 || num > 100) {
            return 0;
        }
        if (i + num >= avail) {
            return 0;
        }
        if (re4_is_digit(p[i])) {
            return 0;
        }
        for (size_t k = 0; k < num; k++) {
            if (!re4_is_ident(p[i + k])) {
                return 0;
            }
        }
        i += num;
        segs++;
        if (!nested) {
            break;
        }
        if (p[i] == 'E') {
            i++;
            break;
        }
        if (segs > 8) {
            return 0;
        }
    }
    if (i >= avail || p[i] != 0) {
        return 0;
    }
    return i;
}

static int re4_find_name(uintptr_t v) {
    int lo = 0;
    int hi = g_nnames - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (g_names[mid].addr == v) {
            return mid;
        }
        if (g_names[mid].addr < v) {
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return -1;
}

static int re4_find_ti(uintptr_t v) {
    int lo = 0;
    int hi = g_nti - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (g_ti[mid].addr == v) {
            return mid;
        }
        if (g_ti[mid].addr < v) {
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return -1;
}

// ---------------------------------------------------------------- escaneo del juego

// Paso 1: nombres de clase (texto) dentro del juego.
static void re4_collect_names(void) {
    g_nnames = 0;
    for (int s = 0; s < g_nseg; s++) {
        const unsigned char *b = g_seg[s].p;
        size_t n = g_seg[s].n;
        for (size_t off = 1; off + 4 <= n; off++) {
            unsigned char c = b[off];
            if (b[off - 1] != 0) {
                continue;
            }
            if (!(re4_is_digit(c) || c == 'N')) {
                continue;
            }
            size_t avail = n - off;
            if (avail > 140) {
                avail = 140;
            }
            size_t len = re4_parse_mangled(b + off, avail);
            if (len == 0) {
                continue;
            }
            if (g_nnames >= MAX_NAMES) {
                return;
            }
            g_names[g_nnames].addr = (uintptr_t)(b + off);
            g_names[g_nnames].len = (int)len;
            g_nnames++;
            off += len;
        }
    }
}

// Paso 2: "typeinfo" = estructura que apunta al nombre de la clase (puntero en el offset 8).
static void re4_collect_typeinfos(void) {
    g_nti = 0;
    if (g_nnames == 0) {
        return;
    }
    uintptr_t lo = g_names[0].addr;
    uintptr_t hi = g_names[g_nnames - 1].addr;
    for (int s = 0; s < g_nseg; s++) {
        const unsigned char *b = g_seg[s].p;
        size_t n = g_seg[s].n;
        for (size_t off = 8; off + 8 <= n; off += 8) {
            uint64_t v = re4_rd64(b + off);
            if (v < lo || v > hi) {
                continue;
            }
            int idx = re4_find_name((uintptr_t)v);
            if (idx < 0) {
                continue;
            }
            if (re4_rd64(b + off - 8) == 0) {
                continue;
            }
            if (g_nti >= MAX_NAMES) {
                return;
            }
            g_ti[g_nti].addr = (uintptr_t)(b + off - 8);
            g_ti[g_nti].name = idx;
            g_nti++;
        }
    }
}

// Paso 3: "vtable" = tabla de funciones virtuales; justo antes de ella hay un puntero al typeinfo.
static void re4_collect_vtables(void) {
    g_nvt = 0;
    if (g_nti == 0) {
        return;
    }
    uintptr_t lo = g_ti[0].addr;
    uintptr_t hi = g_ti[g_nti - 1].addr;
    for (int s = 0; s < g_nseg; s++) {
        const unsigned char *b = g_seg[s].p;
        size_t n = g_seg[s].n;
        for (size_t off = 8; off + 16 <= n; off += 8) {
            uint64_t v = re4_rd64(b + off);
            if (v < lo || v > hi) {
                continue;
            }
            int idx = re4_find_ti((uintptr_t)v);
            if (idx < 0) {
                continue;
            }
            int nf = 0;
            while (nf < 256 && off + 8 + (size_t)(nf + 1) * 8 <= n) {
                uint64_t f = re4_rd64(b + off + 8 + (size_t)nf * 8);
                if (f < g_code_lo || f >= g_code_hi) {
                    break;
                }
                nf++;
            }
            if (nf == 0) {
                continue;   // no es una vtable (por ejemplo, la referencia de una clase hija a su base)
            }
            if (g_nvt >= MAX_VT) {
                return;
            }
            g_vt[g_nvt].addr = (uintptr_t)(b + off + 8);
            g_vt[g_nvt].tinfo = idx;
            g_vt[g_nvt].nfuncs = nf;
            g_nvt++;
        }
    }
}

static int re4_is_interest(const char *name) {
    for (int i = 0; i < N_INTEREST; i++) {
        if (re4_contains(name, g_interest[i])) {
            return 1;
        }
    }
    return 0;
}

// Paso 4: en el codigo, buscar instrucciones "lea reg, [rip+X]" que apuntan a esas vtables.
// Quien guarda una vtable en un objeto casi siempre es su constructor.
static void re4_find_code_refs(void) {
    g_nint = 0;
    for (int i = 0; i < g_nvt && g_nint < MAX_INT; i++) {
        const char *nm = (const char *)g_names[g_ti[g_vt[i].tinfo].name].addr;
        if (re4_is_interest(nm)) {
            g_int_vt[g_nint] = i;
            g_nrefs[g_nint] = 0;
            g_nint++;
        }
    }
    if (g_nint == 0) {
        return;
    }
    for (int s = 0; s < g_nseg; s++) {
        if (!(g_seg[s].prot & 4)) {
            continue;
        }
        const unsigned char *b = g_seg[s].p;
        size_t n = g_seg[s].n;
        for (size_t off = 0; off + 7 <= n; off++) {
            if ((b[off] != 0x48 && b[off] != 0x4C) || b[off + 1] != 0x8D || (b[off + 2] & 0xC7) != 0x05) {
                continue;
            }
            uintptr_t target = (uintptr_t)(b + off + 7) + (intptr_t)re4_rd32(b + off + 3);
            for (int j = 0; j < g_nint; j++) {
                uintptr_t v = g_vt[g_int_vt[j]].addr;
                if ((target == v || target == v - 16) && g_nrefs[j] < MAX_REFS) {
                    g_refs[j][g_nrefs[j]++] = (uintptr_t)(b + off);
                }
            }
        }
    }
}

// Segmentos legibles del juego, ordenados por direccion.
static int re4_load_segments(const Re4ModuleInfo *mi) {
    g_nseg = 0;
    g_code_lo = (uintptr_t)-1;
    g_code_hi = 0;
    g_base = 0;
    for (uint32_t i = 0; i < mi->seg_count && i < 4; i++) {
        if (mi->seg[i].address == NULL || mi->seg[i].size == 0 || !(mi->seg[i].prot & 1)) {
            continue;
        }
        g_seg[g_nseg].p = (const unsigned char *)mi->seg[i].address;
        g_seg[g_nseg].n = (size_t)mi->seg[i].size;
        g_seg[g_nseg].prot = mi->seg[i].prot;
        g_nseg++;
    }
    for (int a = 0; a < g_nseg; a++) {
        for (int b = a + 1; b < g_nseg; b++) {
            if (g_seg[b].p < g_seg[a].p) {
                Seg t = g_seg[a];
                g_seg[a] = g_seg[b];
                g_seg[b] = t;
            }
        }
    }
    for (int a = 0; a < g_nseg; a++) {
        if (a == 0) {
            g_base = (uintptr_t)g_seg[a].p;
        }
        if (g_seg[a].prot & 4) {
            uintptr_t lo = (uintptr_t)g_seg[a].p;
            uintptr_t hi = lo + g_seg[a].n;
            if (lo < g_code_lo) {
                g_code_lo = lo;
            }
            if (hi > g_code_hi) {
                g_code_hi = hi;
            }
        }
    }
    return g_nseg;
}

// ---------------------------------------------------------------- escribir resultados

static void re4_write_report(void) {
    LOGF("== resumen ==\nnombres de clase=%d typeinfos=%d vtables=%d clases de interes=%d\n",
         g_nnames, g_nti, g_nvt, g_nint);

    LOGF("== clases (nombre | vtable | funciones virtuales) ==\n");
    for (int i = 0; i < g_nvt; i++) {
        const char *nm = (const char *)g_names[g_ti[g_vt[i].tinfo].name].addr;
        LOGF("%s | 0x%llx | %d\n", nm, (unsigned long long)g_vt[i].addr, g_vt[i].nfuncs);
    }

    LOGF("== clases de interes: funciones virtuales (desplazamiento desde la base 0x%llx) y codigo que usa la vtable ==\n",
         (unsigned long long)g_base);
    for (int j = 0; j < g_nint; j++) {
        const VtEnt *v = &g_vt[g_int_vt[j]];
        const char *nm = (const char *)g_names[g_ti[v->tinfo].name].addr;
        LOGF("%s vtable=0x%llx nfuncs=%d\n", nm, (unsigned long long)v->addr, v->nfuncs);
        int shown = v->nfuncs < 48 ? v->nfuncs : 48;
        for (int k = 0; k < shown; k++) {
            uint64_t f = re4_rd64((const unsigned char *)v->addr + (size_t)k * 8);
            LOGF("  f%d=+0x%llx\n", k, (unsigned long long)(f - g_base));
        }
        for (int k = 0; k < g_nrefs[j]; k++) {
            LOGF("  usa_vtable_en=+0x%llx\n", (unsigned long long)(g_refs[j][k] - g_base));
        }
    }
}

// ---------------------------------------------------------------- plugin

int32_t attr_public plugin_load(int32_t argc, const char* argv[]) {
    char msg[200] = {0};

    final_printf("[GoldHEN] <%s\\Ver.0x%08x> %s\n", g_pluginName, g_pluginVersion, __func__);

    // 1: lo primero que se ve. Si no sale, el plugin ni cargo.
    snprintf(msg, sizeof(msg), "plugin_load inicio (%s)", BUILD_ID);
    notify_step(1, msg);

    g_fd = re4_open(RE4COOP_LOG_PATH, RE4_O_WRONLY | RE4_O_CREAT | RE4_O_TRUNC, 0777);
    LOGF("==== re4coop v1.02 reconocimiento ====\nbuild: %s\nargc=%d\n", BUILD_ID, (int)argc);

    // 2: modulos cargados (el juego es el primero de la lista).
    static int handles[256];
    unsigned long count = 0;
    int r = re4_module_list(handles, 256, &count);
    if (r != 0 || count == 0) {
        snprintf(msg, sizeof(msg), "no pude listar los modulos (error 0x%08X)", (unsigned int)r);
        notify_step(2, msg);
        LOGF("error: sceKernelGetModuleList = 0x%08X\n", (unsigned int)r);
        log_flush();
        if (g_fd >= 0) {
            re4_close(g_fd);
        }
        return 0;
    }
    if (count > 256) {
        count = 256;
    }

    static Re4ModuleInfo main_info;
    int main_ok = 0;
    LOGF("== modulos (%lu) ==\n", count);
    for (unsigned long i = 0; i < count; i++) {
        Re4ModuleInfo mi;
        mi.size = sizeof(mi);
        int rr = re4_module_info(handles[i], &mi);
        if (rr != 0) {
            LOGF("[%lu] handle=%d error=0x%08X\n", i, handles[i], (unsigned int)rr);
            continue;
        }
        mi.name[sizeof(mi.name) - 1] = 0;
        LOGF("[%lu] %s", i, mi.name);
        for (uint32_t k = 0; k < mi.seg_count && k < 4; k++) {
            LOGF(" | 0x%llx+0x%x p%d", (unsigned long long)(uintptr_t)mi.seg[k].address, (unsigned int)mi.seg[k].size, (int)mi.seg[k].prot);
        }
        LOGF("\n");
        if (i == 0) {
            main_info = mi;
            main_ok = 1;
        }
    }
    if (!main_ok) {
        notify_step(2, "no pude leer los datos del modulo principal");
        log_flush();
        if (g_fd >= 0) {
            re4_close(g_fd);
        }
        return 0;
    }
    main_info.name[sizeof(main_info.name) - 1] = 0;
    LOGF("== modulo principal ==\nnombre=%s fingerprint=", main_info.name);
    for (int k = 0; k < 20; k++) {
        LOGF("%02X", (unsigned int)main_info.fingerprint[k]);
    }
    LOGF("\n");
    re4_load_segments(&main_info);
    snprintf(msg, sizeof(msg), "modulos: %lu. Principal: %.60s base 0x%llx", count, main_info.name, (unsigned long long)g_base);
    notify_step(2, msg);

    // 3: escaneo (puede tardar un momento).
    notify_step(3, "buscando nombres de clase en el juego...");
    re4_collect_names();
    re4_collect_typeinfos();
    re4_collect_vtables();
    re4_find_code_refs();

    // 4: resultado del escaneo.
    snprintf(msg, sizeof(msg), "nombres de clase: %d, vtables: %d, clases de interes: %d", g_nnames, g_nvt, g_nint);
    notify_step(4, msg);
    re4_write_report();

    // 5: el log.
    log_flush();
    if (g_fd < 0) {
        snprintf(msg, sizeof(msg), "no pude abrir el log (error 0x%08X)", (unsigned int)g_fd);
    } else {
        re4_close(g_fd);
        snprintf(msg, sizeof(msg), "log escrito (%lu bytes)", g_total);
    }
    notify_step(5, msg);

    // 6: si lo ves, el plugin termino sin problemas.
    notify_step(6, "fin: el plugin termino bien");
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
