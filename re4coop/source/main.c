#include <stdio.h>
#include <stddef.h>
#include <stdint.h>

#include "plugin_common.h"

attr_public const char *g_pluginName = "re4coop";
attr_public const char *g_pluginDesc = "RE4 coop - reconocimiento";
attr_public const char *g_pluginAuth = "re4coop";
attr_public uint32_t g_pluginVersion = 0x00000103; // 1.03

// Archivos de salida: los copias de la PS4 a la PC y me los pasas.
#ifndef RE4COOP_LOG_PATH
#define RE4COOP_LOG_PATH "/data/GoldHEN/re4coop_log.txt"      // lista de clases (al abrir el juego)
#endif
#ifndef RE4COOP_SCAN_PATH
#define RE4COOP_SCAN_PATH "/data/GoldHEN/re4coop_scan.txt"    // objetos en vivo (ya jugando)
#endif
#ifndef RE4COOP_PROBE_PATH
#define RE4COOP_PROBE_PATH "/data/GoldHEN/re4coop_probe.tmp"  // archivo auxiliar, se puede borrar
#endif
#ifndef RE4COOP_FIRST_SCAN_SECS
#define RE4COOP_FIRST_SCAN_SECS 150                           // espera antes del primer escaneo
#endif

// Identifica esta compilacion (fecha y hora en que GitHub la hizo).
#define BUILD_ID __DATE__ " " __TIME__

// Llamadas de bajo nivel de la PS4 (libkernel). No usan memoria dinamica ni stdio
// (fopen hacia caer el juego al arrancar). Se declaran con otro nombre de C pero
// el mismo simbolo, para no depender de que un header las declare.
extern int  re4_open(const char *path, int flags, int mode) __asm__("sceKernelOpen");
extern long re4_write(int fd, const void *buf, unsigned long nbyte) __asm__("sceKernelWrite");
extern long re4_pwrite(int fd, const void *buf, unsigned long nbyte, long offset) __asm__("sceKernelPwrite");
extern int  re4_close(int fd) __asm__("sceKernelClose");
extern int  re4_module_list(int *array, unsigned long max, unsigned long *count) __asm__("sceKernelGetModuleList");
extern int  re4_module_info(int handle, void *info) __asm__("sceKernelGetModuleInfo");
extern int  re4_thread_create(void **thread, const void *attr, void *(*entry)(void *), void *arg, const char *name) __asm__("scePthreadCreate");
extern unsigned int re4_sleep(unsigned int seconds) __asm__("sceKernelSleep");

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

static void log_open(const char *path) {
    g_len = 0;
    g_total = 0;
    g_fd = re4_open(path, RE4_O_WRONLY | RE4_O_CREAT | RE4_O_TRUNC, 0777);
}

static void log_close(void) {
    log_flush();
    if (g_fd >= 0) {
        re4_close(g_fd);
    }
    g_fd = -1;
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
#define MAX_CEN   3072
#define MAX_REFS  3

typedef struct { uintptr_t addr; int len; } NameEnt;      // texto del nombre de clase
typedef struct { uintptr_t addr; int name; } TinfoEnt;    // typeinfo -> indice del nombre

// Una "vtable" = tabla de funciones virtuales de una clase. Cada objeto de esa clase
// guarda en su primer campo la direccion de su vtable.
typedef struct {
    uintptr_t addr;           // direccion de la vtable (la que guardan los objetos)
    int nf;                   // cuantas funciones seguidas apuntan al codigo del juego
    int name;                 // indice en g_names, o -1 si el juego no trae el nombre
    uint64_t f[4];            // primeras funciones
    int nrefs;
    uintptr_t refs[MAX_REFS]; // sitios del codigo que la usan (constructores probables)
} CenEnt;

static NameEnt g_names[MAX_NAMES];
static int g_nnames;
static TinfoEnt g_ti[MAX_NAMES];
static int g_nti;
static CenEnt g_cen[MAX_CEN];
static int g_ncen;

typedef struct { const unsigned char *p; size_t n; int prot; } Seg;
static Seg g_seg[4];          // segmentos legibles del juego, ordenados por direccion
static int g_nseg;
static uintptr_t g_base;      // direccion del primer segmento (normalmente 0x400000)
static uintptr_t g_code_lo;   // rango de codigo (segmentos ejecutables)
static uintptr_t g_code_hi;

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

static int re4_in_code(uint64_t v) {
    return v >= g_code_lo && v < g_code_hi;
}

static int re4_in_module(uint64_t v) {
    for (int s = 0; s < g_nseg; s++) {
        uint64_t lo = (uint64_t)(uintptr_t)g_seg[s].p;
        if (v >= lo && v < lo + g_seg[s].n) {
            return 1;
        }
    }
    return 0;
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

static int re4_find_cen(uint64_t v) {
    int lo = 0;
    int hi = g_ncen - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (g_cen[mid].addr == v) {
            return mid;
        }
        if (g_cen[mid].addr < v) {
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return -1;
}

static const char *re4_cen_name(int ci) {
    if (g_cen[ci].name >= 0) {
        return (const char *)g_names[g_cen[ci].name].addr;
    }
    return "?";
}

// ---------------------------------------------------------------- escaneo estatico (al abrir el juego)

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

// Paso 3: censo de TODAS las vtables, tengan nombre o no. Una vtable es una tira de
// punteros al codigo del juego, precedida de [offset_to_top = 0] y [typeinfo o 0].
static void re4_collect_census(void) {
    g_ncen = 0;
    for (int s = 0; s < g_nseg; s++) {
        if (g_seg[s].prot & 4) {
            continue;   // el codigo no guarda vtables
        }
        const unsigned char *b = g_seg[s].p;
        size_t n = g_seg[s].n;
        for (size_t off = 16; off + 16 <= n; off += 8) {
            uint64_t f0 = re4_rd64(b + off);
            if (!re4_in_code(f0)) {
                continue;
            }
            uint64_t f1 = re4_rd64(b + off + 8);
            if (!re4_in_code(f1)) {
                continue;
            }
            uint64_t prev = re4_rd64(b + off - 8);
            if (re4_in_code(prev)) {
                continue;   // no es el inicio de la tira
            }
            uint64_t ott = re4_rd64(b + off - 16);
            if (!(ott == 0 || ((int64_t)ott < 0 && (int64_t)ott > -0x10000))) {
                continue;
            }
            if (prev != 0 && !re4_in_module(prev)) {
                continue;
            }
            int nf = 0;
            while (nf < 256 && off + (size_t)(nf + 1) * 8 <= n && re4_in_code(re4_rd64(b + off + (size_t)nf * 8))) {
                nf++;
            }
            if (g_ncen >= MAX_CEN) {
                return;
            }
            CenEnt *c = &g_cen[g_ncen++];
            c->addr = (uintptr_t)(b + off);
            c->nf = nf;
            c->name = -1;
            int ti = re4_find_ti((uintptr_t)prev);
            if (ti >= 0) {
                c->name = g_ti[ti].name;
            }
            for (int k = 0; k < 4; k++) {
                c->f[k] = (k < nf) ? re4_rd64(b + off + (size_t)k * 8) : 0;
            }
            c->nrefs = 0;
        }
    }
}

// Paso 4: en el codigo, buscar instrucciones "lea reg, [rip+X]" que apuntan a una vtable.
// Quien guarda una vtable en un objeto casi siempre es su constructor.
static void re4_find_code_refs(void) {
    if (g_ncen == 0) {
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
            int ci = re4_find_cen(target);
            if (ci < 0) {
                ci = re4_find_cen(target + 16);   // a veces apuntan al inicio del bloque (vtable - 16)
            }
            if (ci >= 0 && g_cen[ci].nrefs < MAX_REFS) {
                g_cen[ci].refs[g_cen[ci].nrefs++] = (uintptr_t)(b + off);
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

static void re4_write_static_report(void) {
    int named = 0;
    for (int i = 0; i < g_ncen; i++) {
        if (g_cen[i].name >= 0) {
            named++;
        }
    }
    LOGF("== resumen ==\nnombres=%d typeinfos=%d vtables=%d (con nombre=%d, sin nombre=%d)\n",
         g_nnames, g_nti, g_ncen, named, g_ncen - named);
    LOGF("(funciones y codigo = desplazamiento desde la base 0x%llx)\n", (unsigned long long)g_base);

    LOGF("== vtables SIN nombre: vtable | n | f0 f1 f2 f3 | codigo que la usa ==\n");
    for (int i = 0; i < g_ncen; i++) {
        const CenEnt *c = &g_cen[i];
        if (c->name >= 0) {
            continue;
        }
        LOGF("0x%llx | %d |", (unsigned long long)c->addr, c->nf);
        for (int k = 0; k < 4 && k < c->nf; k++) {
            LOGF(" +0x%llx", (unsigned long long)(c->f[k] - g_base));
        }
        LOGF(" |");
        for (int k = 0; k < c->nrefs; k++) {
            LOGF(" +0x%llx", (unsigned long long)(c->refs[k] - g_base));
        }
        LOGF("\n");
    }

    LOGF("== vtables CON nombre: nombre | vtable | n | codigo que la usa ==\n");
    for (int i = 0; i < g_ncen; i++) {
        const CenEnt *c = &g_cen[i];
        if (c->name < 0) {
            continue;
        }
        LOGF("%s | 0x%llx | %d |", re4_cen_name(i), (unsigned long long)c->addr, c->nf);
        for (int k = 0; k < c->nrefs; k++) {
            LOGF(" +0x%llx", (unsigned long long)(c->refs[k] - g_base));
        }
        LOGF("\n");
    }

    LOGF("== familia de 23 funciones virtuales (todas las funciones) ==\n");
    for (int i = 0; i < g_ncen; i++) {
        const CenEnt *c = &g_cen[i];
        if (c->nf != 23) {
            continue;
        }
        LOGF("%s 0x%llx:", re4_cen_name(i), (unsigned long long)c->addr);
        const unsigned char *vt = (const unsigned char *)c->addr;
        for (int k = 0; k < c->nf; k++) {
            LOGF(" +0x%llx", (unsigned long long)(re4_rd64(vt + (size_t)k * 8) - g_base));
        }
        LOGF("\n");
    }
}

// ---------------------------------------------------------------- escaneo en vivo (ya jugando)

static int g_probe_fd = -1;
static int g_inst[MAX_CEN];

// Comprueba sin riesgo de que se caiga el juego si se pueden leer n bytes en esa direccion:
// el sistema intenta copiarlos a un archivo y avisa con un error si no estan mapeados.
static int re4_readable(uint64_t addr, size_t n) {
    if (g_probe_fd < 0) {
        return 0;
    }
    long w = re4_pwrite(g_probe_fd, (const void *)(uintptr_t)addr, (unsigned long)n, 0);
    return w == (long)n;
}

static void re4_dump_obj(uint64_t obj, size_t len) {
    if (!re4_readable(obj, len)) {
        LOGF("  (no se puede leer)\n");
        return;
    }
    const unsigned char *p = (const unsigned char *)(uintptr_t)obj;
    for (size_t o = 0; o < len; o += 32) {
        LOGF("  +%03x:", (unsigned int)o);
        for (size_t k = 0; k < 32; k += 8) {
            LOGF(" %016llx", (unsigned long long)re4_rd64(p + o + k));
        }
        LOGF("\n");
    }
}

static int re4_dump_worthy(int ci) {
    const CenEnt *c = &g_cen[ci];
    if (c->nf == 23) {
        return 1;
    }
    if (c->name >= 0) {
        const char *nm = re4_cen_name(ci);
        if (re4_contains(nm, "cPl") || re4_contains(nm, "cSub") || re4_contains(nm, "Camera") || re4_contains(nm, "cCtrl")) {
            return 1;
        }
    }
    return 0;
}

#define SCAN_MAX_LINES 2500
#define SCAN_MAX_DUMPS 48
#define SCAN_PER_CLASS 6
#define DUMP_BYTES     0x200

// Busca en la memoria de datos del juego objetos vivos: o bien objetos guardados ahi mismo
// (S: el campo es una vtable), o bien punteros globales a objetos (P: apuntan a algo cuyo
// primer campo es una vtable). Asi se encuentran a Leon, a Ashley, la camara, etc.
static void re4_runtime_scan(int number) {
    int lines = 0;
    int dumps = 0;
    int cand = 0;
    int checked = 0;

    for (int i = 0; i < g_ncen; i++) {
        g_inst[i] = 0;
    }
    LOGF("== escaneo %d ==\n", number);
    for (int s = 0; s < g_nseg; s++) {
        if (!(g_seg[s].prot & 2) || (g_seg[s].prot & 4)) {
            continue;   // solo memoria de datos escribible
        }
        const unsigned char *b = g_seg[s].p;
        size_t n = g_seg[s].n;
        for (size_t off = 0; off + 8 <= n; off += 8) {
            uint64_t v = re4_rd64(b + off);
            if (v == 0) {
                continue;
            }
            uint64_t slot = (uint64_t)(uintptr_t)(b + off);
            uint64_t obj = 0;
            uint64_t vt = 0;
            char kind = 0;
            int ci = re4_find_cen(v);
            if (ci >= 0) {
                kind = 'S';
                obj = slot;
                vt = v;
            } else {
                if ((v & 7) != 0 || v >= 0x0000800000000000ULL || re4_in_code(v)) {
                    continue;
                }
                if (!re4_in_module(v) && v < 0x100000000ULL) {
                    continue;
                }
                cand++;
                if (!re4_in_module(v)) {
                    checked++;
                    if (!re4_readable(v, 8)) {
                        continue;
                    }
                }
                uint64_t q = re4_rd64((const unsigned char *)(uintptr_t)v);
                ci = re4_find_cen(q);
                if (ci < 0) {
                    continue;
                }
                kind = 'P';
                obj = v;
                vt = q;
            }
            g_inst[ci]++;
            if (g_inst[ci] > SCAN_PER_CLASS || lines >= SCAN_MAX_LINES) {
                continue;
            }
            lines++;
            if (kind == 'S') {
                LOGF("S obj=0x%llx vt=0x%llx n=%d %s\n", (unsigned long long)obj, (unsigned long long)vt, g_cen[ci].nf, re4_cen_name(ci));
            } else {
                LOGF("P slot=0x%llx obj=0x%llx vt=0x%llx n=%d %s\n", (unsigned long long)slot, (unsigned long long)obj,
                     (unsigned long long)vt, g_cen[ci].nf, re4_cen_name(ci));
            }
            if (re4_dump_worthy(ci) && g_inst[ci] <= 3 && dumps < SCAN_MAX_DUMPS) {
                dumps++;
                re4_dump_obj(obj, DUMP_BYTES);
            }
        }
    }
    LOGF("-- instancias por clase (solo clases con mas de %d) --\n", SCAN_PER_CLASS);
    for (int i = 0; i < g_ncen; i++) {
        if (g_inst[i] > SCAN_PER_CLASS) {
            LOGF("%s 0x%llx: %d\n", re4_cen_name(i), (unsigned long long)g_cen[i].addr, g_inst[i]);
        }
    }
    LOGF("-- fin escaneo %d: lineas=%d dumps=%d candidatos=%d comprobados=%d --\n", number, lines, dumps, cand, checked);
}

// Hilo aparte: espera a que estes jugando y hace tres escaneos guiados por mensajes.
static void *re4_scan_thread(void *arg) {
    char msg[160];
    (void)arg;

    g_probe_fd = re4_open(RE4COOP_PROBE_PATH, RE4_O_WRONLY | RE4_O_CREAT | RE4_O_TRUNC, 0777);
    log_open(RE4COOP_SCAN_PATH);
    LOGF("==== re4coop escaneo de objetos (v1.03) ====\nbuild: %s\nprobe_fd=%d scan_fd=%d\n", BUILD_ID, g_probe_fd, g_fd);
    log_flush();

    re4_sleep(RE4COOP_FIRST_SCAN_SECS);
    notify_step(7, "escaneo 1/3 AHORA. Deja a Leon quieto");
    re4_runtime_scan(1);
    log_flush();
    notify_step(8, "escaneo 1 listo. Sigue sin moverte 30 s");

    re4_sleep(30);
    notify_step(9, "escaneo 2/3 AHORA. Sigue quieto");
    re4_runtime_scan(2);
    log_flush();
    notify_step(10, "escaneo 2 listo. Ahora camina y espera 20 s");

    re4_sleep(20);
    notify_step(11, "escaneo 3/3 AHORA. Puedes seguir caminando");
    re4_runtime_scan(3);
    log_flush();
    snprintf(msg, sizeof(msg), "escaneo 3 listo (%lu bytes). Ya puedes jugar normal", g_total);
    notify_step(12, msg);

    log_close();
    if (g_probe_fd >= 0) {
        re4_close(g_probe_fd);
        g_probe_fd = -1;
    }
    return NULL;
}

// ---------------------------------------------------------------- plugin

int32_t attr_public plugin_load(int32_t argc, const char* argv[]) {
    char msg[200] = {0};

    final_printf("[GoldHEN] <%s\\Ver.0x%08x> %s\n", g_pluginName, g_pluginVersion, __func__);

    // 1: lo primero que se ve. Si no sale, el plugin ni cargo.
    snprintf(msg, sizeof(msg), "plugin_load inicio (%s)", BUILD_ID);
    notify_step(1, msg);

    log_open(RE4COOP_LOG_PATH);
    LOGF("==== re4coop v1.03 reconocimiento ====\nbuild: %s\n", BUILD_ID);

    // 2: modulos cargados (el juego es el primero de la lista).
    static int handles[256];
    unsigned long count = 0;
    int r = re4_module_list(handles, 256, &count);
    if (r != 0 || count == 0) {
        snprintf(msg, sizeof(msg), "no pude listar los modulos (error 0x%08X)", (unsigned int)r);
        notify_step(2, msg);
        LOGF("error: sceKernelGetModuleList = 0x%08X\n", (unsigned int)r);
        log_close();
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
        log_close();
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

    // 3: escaneo estatico (puede tardar un momento).
    notify_step(3, "buscando clases y vtables en el juego...");
    re4_collect_names();
    re4_collect_typeinfos();
    re4_collect_census();
    re4_find_code_refs();

    // 4: resultado del escaneo.
    snprintf(msg, sizeof(msg), "nombres de clase: %d, vtables: %d", g_nnames, g_ncen);
    notify_step(4, msg);
    re4_write_static_report();

    // 5: el log.
    log_flush();
    if (g_fd < 0) {
        snprintf(msg, sizeof(msg), "no pude abrir el log (error 0x%08X)", (unsigned int)g_fd);
    } else {
        snprintf(msg, sizeof(msg), "log escrito (%lu bytes)", g_total);
    }
    log_close();
    notify_step(5, msg);

    // 6: hilo que hace el escaneo en vivo cuando ya estes jugando.
    void *thread = NULL;
    int tr = re4_thread_create(&thread, NULL, re4_scan_thread, NULL, "re4coop_scan");
    if (tr != 0) {
        snprintf(msg, sizeof(msg), "no pude crear el hilo de escaneo (error 0x%08X)", (unsigned int)tr);
    } else {
        snprintf(msg, sizeof(msg), "listo. Entra a tu partida YA: el escaneo en vivo empieza en %d s", RE4COOP_FIRST_SCAN_SECS);
    }
    notify_step(6, msg);
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
