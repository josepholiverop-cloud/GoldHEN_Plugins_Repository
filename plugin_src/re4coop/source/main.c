#include <stdio.h>
#include <stddef.h>
#include <stdint.h>

#include "plugin_common.h"

attr_public const char *g_pluginName = "re4coop";
attr_public const char *g_pluginDesc = "RE4 coop - reconocimiento";
attr_public const char *g_pluginAuth = "re4coop";
attr_public uint32_t g_pluginVersion = 0x0000010d; // 1.13

// Version en texto: sale en CADA mensaje de pantalla y en la cabecera de los logs.
// Ojo: si cambias la version, cambia tambien el numero de arriba. Nada mas.
#define RE4COOP_VER_STR "1.13"

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
#define RE4COOP_FIRST_SCAN_SECS 60                            // espera antes del primer escaneo
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
    snprintf(msg, sizeof(msg), "re4coop v" RE4COOP_VER_STR " #%d: %s", step, text);
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

// Un float se escribe sin usar printf de coma flotante (algunos libc de PS4 no lo traen).
static float re4_bits_to_float(uint32_t b) {
    union { uint32_t u; float f; } x;
    x.u = b;
    return x.f;
}

static void re4_log_float(const char *label, float f) {
    if (!(f > -1000000.0f && f < 1000000.0f)) {
        LOGF(" %s=?", label);
        return;
    }
    int neg = (f < 0.0f);
    float af = neg ? -f : f;
    int v = (int)(af * 100.0f + 0.5f);
    LOGF(" %s=%s%d.%02d", label, neg ? "-" : "", v / 100, v % 100);
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
}

// ---------------------------------------------------------------- escaneo en vivo (ya jugando)

static int g_probe_fd = -1;

// Comprueba sin riesgo de que se caiga el juego si se pueden leer n bytes en esa direccion:
// el sistema intenta copiarlos a un archivo y avisa con un error si no estan mapeados.
static int re4_readable(uint64_t addr, size_t n) {
    if (g_probe_fd < 0) {
        return 0;
    }
    long w = re4_pwrite(g_probe_fd, (const void *)(uintptr_t)addr, (unsigned long)n, 0);
    return w == (long)n;
}

// ---- deteccion por movimiento: quien se mueve cuando tu te mueves, ese es Leon ----
//
// No hace falta saber como se llama la clase. Se guarda una foto de los primeros bytes
// de cada objeto vivo, se te pide que camines, y se compara: los objetos que cambiaron
// son los que se mueven (Leon, su camara, los enemigos cercanos). Con una segunda fase
// (girar la camara sin caminar) se separa la camara del personaje.

#define DET_MAX        640
#define DET_BYTES      0x100
#define DET_WORDS      (DET_BYTES / 8)
#define DET_MAX_PRINT  64      // objetos distintos que se detallan en el archivo
#define DET_WORDS_PRINT 12     // palabras (8 bytes) que se detallan por objeto

typedef struct {
    uintptr_t slot;   // direccion global que guarda el puntero al objeto
    uintptr_t obj;    // el objeto
    uintptr_t vt;     // su vtable
    int name;         // indice en g_names, o -1
    int nf;
} DetEnt;

static DetEnt g_det[DET_MAX];
static int g_ndet;
static unsigned char g_img[DET_MAX * DET_BYTES];

// ---------------------------------------------------------------- Leon, ya localizado
//
// Datos de la prueba de la v1.05: entre las clases SIN nombre del juego hay exactamente
// UNA con 23 funciones virtuales, y 23 es justo lo que tienen cPlAda, cPlAshley,
// cPlKlauser y cPlWesker. Esa es Leon (vtable 0xd28500 en esta version del juego).
// Dos globales apuntan al mismo objeto: 0xd58ee0 y 0xea5b10.

static uint32_t re4_float_to_bits(float f) {
    union { uint32_t u; float f; } x;
    x.f = f;
    return x.u;
}

static int re4_find_leon(void) {
    for (int i = 0; i < g_ndet; i++) {
        const DetEnt *d = &g_det[i];
        if (d->name >= 0 || d->nf != 23) {
            continue;
        }
        if (!re4_readable((uint64_t)d->obj, DET_BYTES)) {
            continue;
        }
        uint64_t own = re4_rd64((const unsigned char *)(uintptr_t)d->obj);
        if (own == (uint64_t)d->vt) {
            return i;
        }
    }
    return -1;
}

// La camara: sin nombre y con 3 funciones (22CameraAttachedToMotion tiene 3 tambien,
// pero esa si trae nombre).
static int re4_find_cam(void) {
    for (int i = 0; i < g_ndet; i++) {
        const DetEnt *d = &g_det[i];
        if (d->name >= 0 || d->nf != 3) {
            continue;
        }
        if (re4_readable((uint64_t)d->obj, DET_BYTES)) {
            return i;
        }
    }
    return -1;
}

// Vuelca los 0x100 bytes del objeto palabra a palabra, para deducir el formato.
static void re4_dump_full(uintptr_t obj, const char *label) {
    LOGF("\n== %s  obj=0x%llx ==\n", label, (unsigned long long)obj);
    for (int w = 0; w < DET_WORDS; w++) {
        uint64_t v = re4_rd64((const unsigned char *)(uintptr_t)(obj + (uintptr_t)w * 8));
        LOGF("   +%03x %016llx", w * 8, (unsigned long long)v);
        re4_log_float("l", re4_bits_to_float((uint32_t)v));
        re4_log_float("h", re4_bits_to_float((uint32_t)(v >> 32)));
        LOGF("\n");
    }
}

// Muestrea el objeto una vez por segundo mientras caminas: la coordenada que avanza
// de forma continua es la posicion; un angulo o una matriz se queda practicamente igual.
static void re4_trace(uintptr_t obj, int seconds) {
    LOGF("\n== traza caminando (1 muestra por segundo, palabras +0x10..+0x48) ==\n");
    for (int s = 0; s < seconds; s++) {
        LOGF("T%02d", s);
        for (int w = 2; w <= 9; w++) {
            uint64_t v = re4_rd64((const unsigned char *)(uintptr_t)(obj + (uintptr_t)w * 8));
            LOGF(" +%02x", w * 8);
            re4_log_float("l", re4_bits_to_float((uint32_t)v));
            re4_log_float("h", re4_bits_to_float((uint32_t)(v >> 32)));
        }
        LOGF("\n");
        log_flush();
        if (s + 1 < seconds) {
            re4_sleep(1);
        }
    }
}

// Cual de los campos que cambiaron al caminar tiene mas pinta de coordenada.
typedef struct {
    int off;      // byte dentro del objeto (-1 si no hay candidato)
    int valid;
    float before;
    float after;
    float delta;
} BestMove;

static BestMove re4_leon_moved(uintptr_t obj, const unsigned char *img, BestMove *second) {
    BestMove best;
    best.off = -1;
    best.valid = 0;
    best.before = 0.0f;
    best.after = 0.0f;
    best.delta = 0.0f;
    float bestabs = 0.0f;

    // El segundo mejor, para probar tambien la otra componente horizontal.
    second->off = -1;
    second->valid = 0;
    second->before = 0.0f;
    second->after = 0.0f;
    second->delta = 0.0f;
    float secondabs = 0.0f;

    LOGF("\n== LEON: campos que cambiaron al caminar (mitad por mitad) ==\n");
    for (int w = 0; w < DET_WORDS; w++) {
        uint64_t a = re4_rd64(img + (size_t)w * 8);
        uint64_t c = re4_rd64((const unsigned char *)(uintptr_t)(obj + (uintptr_t)w * 8));
        if (a == c) {
            continue;
        }
        for (int half = 0; half < 2; half++) {
            uint32_t ab = half ? (uint32_t)(a >> 32) : (uint32_t)a;
            uint32_t cb = half ? (uint32_t)(c >> 32) : (uint32_t)c;
            float af = re4_bits_to_float(ab);
            float cf = re4_bits_to_float(cb);
            float d = cf - af;
            LOGF("   +%03x%s %08x -> %08x", w * 8 + half * 4, half ? "H" : "L",
                 (unsigned int)ab, (unsigned int)cb);
            re4_log_float("antes", af);
            re4_log_float("ahora", cf);
            re4_log_float("dif", d);
            LOGF("\n");
            if (!(af > -100000.0f && af < 100000.0f)) {
                continue;   // infinito, NaN o basura
            }
            if (!(cf > -100000.0f && cf < 100000.0f)) {
                continue;
            }
            float ad = d < 0.0f ? -d : d;
            // Una coordenada vale cientos o miles y se movio bastante. Un angulo
            // (seno/coseno) esta entre -7 y 7 y casi no cambia al caminar recto.
            if (!(af > 50.0f || af < -50.0f) || ad < 5.0f) {
                continue;
            }
            // El mismo valor suele estar copiado en varios sitios del objeto: se
            // guarda una sola vez para no probar dos veces la misma coordenada.
            uint32_t cb_bits = re4_float_to_bits(cf);
            if (best.valid && re4_float_to_bits(best.after) == cb_bits) {
                continue;
            }
            if (second->valid && re4_float_to_bits(second->after) == cb_bits) {
                continue;
            }
            BestMove cand;
            cand.off = w * 8 + half * 4;
            cand.valid = 1;
            cand.before = af;
            cand.after = cf;
            cand.delta = d;
            if (!best.valid || ad > bestabs) {
                if (best.valid) {
                    *second = best;
                    secondabs = bestabs;
                }
                best = cand;
                bestabs = ad;
            } else if (!second->valid || ad > secondabs) {
                *second = cand;
                secondabs = ad;
            }
        }
    }
    return best;
}

// Suma delta a todos los campos del objeto que valgan exactamente match_bits (la
// posicion suele estar copiada en varios sitios), y guarda lo original para deshacerlo.
#define PAT_MAX 16
typedef struct { int off; uint32_t bits; } PatEnt;
static PatEnt g_pat[PAT_MAX];
static int g_pat_n;

static int re4_patch_delta(uintptr_t obj, uint32_t match_bits, float delta) {
    int hits = 0;
    g_pat_n = 0;
    for (int w = 0; w < DET_WORDS; w++) {
        uintptr_t a = obj + (uintptr_t)w * 8;
        uint64_t v = re4_rd64((const unsigned char *)(uintptr_t)a);
        int dirty = 0;
        for (int half = 0; half < 2; half++) {
            uint32_t b = half ? (uint32_t)(v >> 32) : (uint32_t)v;
            if (b != match_bits || g_pat_n >= PAT_MAX) {
                continue;
            }
            uint32_t nb = re4_float_to_bits(re4_bits_to_float(b) + delta);
            g_pat[g_pat_n].off = w * 8 + half * 4;
            g_pat[g_pat_n].bits = b;
            g_pat_n++;
            hits++;
            if (half) {
                v = (v & 0x00000000FFFFFFFFULL) | ((uint64_t)nb << 32);
            } else {
                v = (v & 0xFFFFFFFF00000000ULL) | (uint64_t)nb;
            }
            dirty = 1;
        }
        if (dirty) {
            *(volatile uint64_t *)(void *)a = v;
        }
    }
    return hits;
}

// Lee los 4 bytes de un campo del objeto (off dice cual de las dos mitades de la palabra).
static uint32_t re4_field_bits(uintptr_t obj, int off) {
    uintptr_t addr = obj + (uintptr_t)off;
    uintptr_t base = addr & ~(uintptr_t)7;
    uint64_t v = re4_rd64((const unsigned char *)(uintptr_t)base);
    return (addr - base) >= 4 ? (uint32_t)(v >> 32) : (uint32_t)v;
}

// El nodo que SE DIBUJA es cParts, y guarda su propia copia de la posicion. Si al mover
// a Leon esa copia se queda atras, el juego dibuja desde ahi y habra que escribir ahi:
// por eso la v1.08 anota cParts antes y despues de cada escritura.
static uintptr_t re4_find_parts(uintptr_t leon, int off) {
    uint32_t want = re4_field_bits(leon, off);
    for (int i = 0; i < g_ndet; i++) {
        const DetEnt *d = &g_det[i];
        if (d->name < 0) {
            continue;
        }
        const char *nm = (const char *)g_names[d->name].addr;
        if (!re4_contains(nm, "cParts") || re4_contains(nm, "Mgr")) {
            continue;
        }
        for (int w = 0x8; w <= 0x14; w++) {
            uint64_t v = re4_rd64((const unsigned char *)(uintptr_t)(d->obj + (uintptr_t)w * 8));
            if ((uint32_t)v == want || (uint32_t)(v >> 32) == want) {
                return d->obj;
            }
        }
    }
    return 0;
}

static void re4_log_parts(uintptr_t parts, const char *label) {
    LOGF("   cParts %s:", label);
    for (int w = 0x0f; w <= 0x13; w++) {
        uint64_t v = re4_rd64((const unsigned char *)(uintptr_t)(parts + (uintptr_t)w * 8));
        LOGF(" +%02x", w * 8);
        re4_log_float("l", re4_bits_to_float((uint32_t)v));
        re4_log_float("h", re4_bits_to_float((uint32_t)(v >> 32)));
    }
    LOGF("\n");
}

// ---------------------------------------------------------------- mando 2 (libScePad)
//
// Para leer el mando 2 hay que hablar con libScePad, que el juego ya trae cargado
// (libScePad.sprx). En vez de enlazar la libreria (eso obligaria a tocar el Makefile)
// se pide cada funcion por su nombre al modulo que ya esta en memoria. Si algo de
// esto falla, queda anotado en el log y el plugin no hace nada mas: nada de cierres.

extern int re4_dlsym(int handle, const char *symbol, void **address) __asm__("sceKernelDlsym");
extern int re4_load_module(const char *path, int argc, const void *argv, int flags,
                          const void *opt, int *handle) __asm__("sceKernelLoadStartModule");

// La espera entre pasos va en microsegundos: sceKernelSleep solo cuenta segundos.
extern int re4_usleep(unsigned int useconds) __asm__("sceKernelUsleep");

typedef int (*Re4PadInit)(void);
typedef int (*Re4PadOpen)(int user_id, int type, int index, const void *param);
typedef int (*Re4PadReadState)(int handle, void *data);
typedef int (*Re4PadClose)(int handle);
typedef int (*Re4PadGetHandle)(int user_id, int type, int index);
typedef int (*Re4UserInitial)(int *user_id);

static Re4PadInit      g_pad_init;
static Re4PadOpen      g_pad_open;
static Re4PadReadState g_pad_read;
static Re4PadClose     g_pad_close;
static Re4PadGetHandle g_pad_gethandle;

// Handles de los modulos que el juego ya tiene cargados (los rellena plugin_load).
static int g_mod_pad = -1;
static int g_mod_user = -1;

#define RE4_PAD_MAX 16
static int g_pad_handle[RE4_PAD_MAX];
static int g_pad_which[RE4_PAD_MAX];   // indice del mando dentro de la cuenta
static int g_pad_owned[RE4_PAD_MAX];   // 1 = lo abrio el plugin (hay que cerrarlo)
static int g_pad_user[RE4_PAD_MAX];    // cuenta de la consola (usuario o invitado) del mando
static int g_pad_n;

// El informe del mando se lee crudo y se interpreta por offsets, para no depender de
// acertar con la estructura exacta del SDK.
#define RE4_PAD_BYTES 0x60
#define RE4_PAD_BTN   0x00
#define RE4_PAD_LX    0x04
#define RE4_PAD_LY    0x05
#define RE4_PAD_RX    0x06
#define RE4_PAD_RY    0x07

static unsigned char g_pad_buf[RE4_PAD_MAX][RE4_PAD_BYTES];

static int re4_pad_load_module(void) {
    if (g_mod_pad >= 0) {
        return g_mod_pad;
    }
    int h = -1;
    int r = re4_load_module("libScePad.sprx", 0, NULL, 0, NULL, &h);
    if (r != 0 || h < 0) {
        LOGF("   no pude cargar libScePad (0x%08X)\n", (unsigned int)r);
        return -1;
    }
    g_mod_pad = h;
    return g_mod_pad;
}

static int re4_pad_resolve(void) {
    if (g_pad_read != NULL) {
        return 0;
    }
    int h = re4_pad_load_module();
    if (h < 0) {
        return -1;
    }
    void *fn = NULL;
    int r;

    r = re4_dlsym(h, "scePadInit", &fn);
    if (r == 0 && fn != NULL) {
        g_pad_init = (Re4PadInit)(uintptr_t)fn;
    } else {
        LOGF("   scePadInit: 0x%08X\n", (unsigned int)r);
    }

    fn = NULL;
    r = re4_dlsym(h, "scePadOpen", &fn);
    if (r == 0 && fn != NULL) {
        g_pad_open = (Re4PadOpen)(uintptr_t)fn;
    } else {
        LOGF("   scePadOpen: 0x%08X\n", (unsigned int)r);
    }

    fn = NULL;
    r = re4_dlsym(h, "scePadReadState", &fn);
    if (r == 0 && fn != NULL) {
        g_pad_read = (Re4PadReadState)(uintptr_t)fn;
    } else {
        LOGF("   scePadReadState: 0x%08X\n", (unsigned int)r);
    }

    fn = NULL;
    r = re4_dlsym(h, "scePadClose", &fn);
    if (r == 0 && fn != NULL) {
        g_pad_close = (Re4PadClose)(uintptr_t)fn;
    } else {
        LOGF("   scePadClose: 0x%08X\n", (unsigned int)r);
    }

    fn = NULL;
    r = re4_dlsym(h, "scePadGetHandle", &fn);
    if (r == 0 && fn != NULL) {
        g_pad_gethandle = (Re4PadGetHandle)(uintptr_t)fn;
    } else {
        LOGF("   scePadGetHandle: 0x%08X\n", (unsigned int)r);
    }

    if (g_pad_open == NULL || g_pad_read == NULL) {
        LOGF("   sin funciones del mando: no puedo seguir\n");
        return -1;
    }
    if (g_pad_init != NULL) {
        int ir = g_pad_init();
        LOGF("   scePadInit = 0x%08X\n", (unsigned int)ir);
    }
    return 0;
}

static int re4_pad_user(void) {
    int user = 0x1000000;
    if (g_mod_user >= 0) {
        void *fn = NULL;
        if (re4_dlsym(g_mod_user, "sceUserServiceGetInitialUser", &fn) == 0 && fn != NULL) {
            Re4UserInitial get = (Re4UserInitial)(uintptr_t)fn;
            int u = 0;
            if (get(&u) == 0 && u != 0) {
                user = u;
            }
        }
    }
    return user;
}

// Lista de cuentas con sesion abierta en la consola. Es lo que hace falta desde la v1.13:
// la PS4 no deja dos mandos en el mismo usuario (apaga el segundo), asi que el mando 2 va
// en otra cuenta o en el invitado, y hay que abrirlo con ESA cuenta, no con la del juego.
// La funcion rellena 4 enteros y -1 significa hueco vacio. Devuelve cuantas cuentas hay
// (-1 si esa funcion no existe en esta consola).
static int re4_pad_login_users(int *out, int max) {
    if (g_mod_user < 0 || out == NULL || max <= 0) {
        return -1;
    }
    void *fn = NULL;
    if (re4_dlsym(g_mod_user, "sceUserServiceGetLoginUserIdList", &fn) != 0 || fn == NULL) {
        LOGF("   lista de cuentas: esa funcion no esta en libSceUserService\n");
        return -1;
    }
    typedef int (*Re4UserList)(void *);
    Re4UserList get = (Re4UserList)(uintptr_t)fn;
    unsigned char raw[0x20];
    for (int k = 0; k < 0x20; k++) {
        raw[k] = 0;
    }
    int r = get(raw);
    if (r != 0) {
        LOGF("   lista de cuentas: 0x%08X (pruebo a inicializar el servicio)\n", (unsigned int)r);
        void *ini = NULL;
        if (re4_dlsym(g_mod_user, "sceUserServiceInitialize", &ini) == 0 && ini != NULL) {
            typedef int (*Re4UserInit)(const void *);
            Re4UserInit init = (Re4UserInit)(uintptr_t)ini;
            LOGF("   sceUserServiceInitialize = 0x%08X\n", (unsigned int)init(NULL));
            r = get(raw);
        }
    }
    if (r != 0) {
        LOGF("   lista de cuentas: 0x%08X (fallo, sigo con lo que haya)\n", (unsigned int)r);
    }
    LOGF("   cuentas con sesion:");
    int n = 0;
    for (int j = 0; j < 4; j++) {
        int v = 0;
        __builtin_memcpy(&v, raw + j * 4, 4);
        LOGF(" 0x%x", v);
        if (v != 0 && v != -1 && n < max) {
            int dup = 0;
            for (int k = 0; k < n; k++) {
                if (out[k] == v) {
                    dup = 1;
                }
            }
            if (!dup) {
                out[n++] = v;
            }
        }
    }
    LOGF("\n");
    return n;
}

static void re4_pad_add(int handle, int which, int owned, int user) {
    if (handle < 0 || g_pad_n >= RE4_PAD_MAX) {
        return;
    }
    g_pad_handle[g_pad_n] = handle;
    g_pad_which[g_pad_n] = which;
    g_pad_owned[g_pad_n] = owned;
    g_pad_user[g_pad_n] = user;
    g_pad_n++;
}

// Un intento con una cuenta y un indice. Primero pregunta si ese mando YA esta abierto
// (el que usa el juego lo esta, y abrirlo otra vez da error) y, si no, lo abre el plugin.
// verbose=0 en los reintentos: solo se anota lo que sale bien, para no llenar el log.
static int re4_pad_try_one(int user, int idx, unsigned char *prm, int verbose) {
    if (g_pad_n >= RE4_PAD_MAX) {
        return 0;
    }
    int h = -1;
    if (g_pad_gethandle != NULL) {
        h = g_pad_gethandle(user, 0, idx);
    }
    if (h >= 0) {
        for (int j = 0; j < g_pad_n; j++) {
            if (g_pad_handle[j] == h) {
                return 0;
            }
        }
        LOGF("   GetHandle(cuenta=0x%x, idx=%d) = %d (ya estaba abierto)\n", user, idx, h);
        re4_pad_add(h, idx, 0, user);
        return 1;
    }
    int r = g_pad_open(user, 0, idx, prm);
    if (r >= 0) {
        for (int j = 0; j < g_pad_n; j++) {
            if (g_pad_handle[j] == r) {
                return 0;
            }
        }
        LOGF("   Open(cuenta=0x%x, idx=%d) = %d (lo abri yo)\n", user, idx, r);
        re4_pad_add(r, idx, 1, user);
        return 1;
    }
    if (verbose) {
        LOGF("   cuenta=0x%x idx=%d: GetHandle=-1, Open=0x%08X\n", user, idx, (unsigned int)r);
    }
    return 0;
}

static void re4_add_user(int *users, int *nu, int max, int v) {
    if (v == 0 || v == -1 || *nu >= max) {
        return;
    }
    for (int j = 0; j < *nu; j++) {
        if (users[j] == v) {
            return;
        }
    }
    users[(*nu)++] = v;
}

// Barre las cuentas y los indices posibles. Devuelve cuantos mandos hay en la lista.
// Primero las cuentas que la consola tiene con sesion ahora mismo (aqui entra el invitado,
// que es donde suele ir el mando 2), despues la cuenta del juego y, por si acaso, unas
// cuentas tipicas. Abrir una cuenta que no existe solo da error: no rompe nada.
static int re4_pad_scan(int verbose) {
    unsigned char prm[16];
    for (int k = 0; k < 16; k++) {
        prm[k] = 0;
    }
    int users[16];
    int nu = 0;
    int logins[4];
    int nl = re4_pad_login_users(logins, 4);
    for (int k = 0; k < nl && k < 4; k++) {
        re4_add_user(users, &nu, 16, logins[k]);
    }
    int base = re4_pad_user();
    re4_add_user(users, &nu, 16, base);
    re4_add_user(users, &nu, 16, base + 1);
    re4_add_user(users, &nu, 16, base + 2);
    re4_add_user(users, &nu, 16, base + 3);
    re4_add_user(users, &nu, 16, 0x1000000);
    re4_add_user(users, &nu, 16, 0x1000001);
    re4_add_user(users, &nu, 16, 0x1000002);
    re4_add_user(users, &nu, 16, 0x1000003);
    if (verbose) {
        LOGF("   cuentas a probar:");
        for (int j = 0; j < nu; j++) {
            LOGF(" 0x%x", users[j]);
        }
        LOGF("\n");
    }
    for (int j = 0; j < nu; j++) {
        for (int k = 0; k < 4; k++) {
            re4_pad_try_one(users[j], k, prm, verbose);
        }
    }
    return g_pad_n;
}

static int re4_pad_open_all(void) {
    g_pad_n = 0;
    LOGF("\n== mando 2: preparando libScePad (modulo=0x%x) ==\n", g_mod_pad);
    if (re4_pad_resolve() != 0) {
        return -1;
    }
    LOGF("   userId=0x%x\n", re4_pad_user());
    int n = re4_pad_scan(1);
    LOGF("   mandos en la lista: %d\n", n);
    log_flush();
    return n;
}

static int re4_pad_read_one(int slot) {
    if (slot < 0 || slot >= g_pad_n) {
        return -1;
    }
    unsigned char *d = g_pad_buf[slot];
    for (int k = 0; k < RE4_PAD_BYTES; k++) {
        d[k] = 0;
    }
    return g_pad_read(g_pad_handle[slot], d);
}

static int re4_pad_stick(int v) {
    int d = v - 128;
    if (d > 127) {
        d = 127;
    }
    if (d < -128) {
        d = -128;
    }
    return d;
}

// Fase de sondeo: 8 s mirando todos los mandos de la lista mientras tu mueves SOLO el
// mando 2. El que se mueva, ese es. La sonda corre SIEMPRE (aunque no haya ningun mando
// abierto) y cada segundo vuelve a intentar abrir uno, por si enciendes el mando 2 ahora.
static int re4_pad_probe(int step_no, int seconds) {
    notify_step(step_no, "SONDA: suelta el mando 1 y mueve SOLO el mando 2 (stick izq. en circulos)");
    re4_sleep(3);

    int us = re4_usleep(1000);
    int fast = (us == 0) ? 1 : 0;

    LOGF("\n== SONDEO DE MANDOS (%d s): mueve solo el mando 2 ==\n", seconds);
    LOGF("   espera corta sceKernelUsleep(1000) = 0x%08X (%s)\n",
         (unsigned int)us, fast ? "bien" : "falla: usare segundos");
    LOGF("   mandos en la lista: %d\n", g_pad_n);
    log_flush();

    int activ[RE4_PAD_MAX];
    uint32_t prev_btn[RE4_PAD_MAX];
    int fails[RE4_PAD_MAX];
    int known = 0;
    for (int j = 0; j < RE4_PAD_MAX; j++) {
        activ[j] = 0;
        fails[j] = 0;
        prev_btn[j] = 0;
    }

    int ticks = fast ? seconds * 4 : seconds;
    for (int t = 0; t < ticks; t++) {
        int retry = fast ? ((t % 4) == 0) : 1;
        if (retry && g_pad_n < RE4_PAD_MAX) {
            int before = g_pad_n;
            if (re4_pad_scan(0) != before) {
                LOGF("   ahora hay %d mandos en la lista\n", g_pad_n);
                log_flush();
            }
        }
        if (g_pad_n > known) {
            for (int j = known; j < g_pad_n; j++) {
                activ[j] = 0;
                fails[j] = 0;
                prev_btn[j] = 0;
                if (re4_pad_read_one(j) == 0) {
                    prev_btn[j] = (uint32_t)re4_rd32(g_pad_buf[j] + RE4_PAD_BTN);
                }
            }
            known = g_pad_n;
        }

        for (int j = 0; j < g_pad_n; j++) {
            if (re4_pad_read_one(j) != 0) {
                fails[j]++;
                continue;
            }
            const unsigned char *d = g_pad_buf[j];
            int a = re4_pad_stick((int)d[RE4_PAD_LX]);
            int b = re4_pad_stick((int)d[RE4_PAD_LY]);
            int c = re4_pad_stick((int)d[RE4_PAD_RX]);
            int e = re4_pad_stick((int)d[RE4_PAD_RY]);
            int m = (a < 0 ? -a : a) + (b < 0 ? -b : b) + (c < 0 ? -c : c) + (e < 0 ? -e : e);
            uint32_t btn = (uint32_t)re4_rd32(d + RE4_PAD_BTN);
            if (btn != prev_btn[j]) {
                m += 300;
                prev_btn[j] = btn;
            }
            activ[j] += m;
        }

        if ((t % 4) == 3 || !fast) {
            int sec = fast ? ((t + 1) / 4) : (t + 1);
            LOGF("   t=%ds", sec);
            if (g_pad_n == 0) {
                LOGF("  (ningun mando en la lista)");
            }
            for (int j = 0; j < g_pad_n; j++) {
                const unsigned char *d = g_pad_buf[j];
                LOGF("  [u=0x%x h=%d idx=%d act=%d fallos=%d lx=%d ly=%d btn=%08x]",
                     g_pad_user[j], g_pad_handle[j], g_pad_which[j], activ[j], fails[j],
                     (int)d[RE4_PAD_LX], (int)d[RE4_PAD_LY],
                     (unsigned int)re4_rd32(d + RE4_PAD_BTN));
            }
            LOGF("\n");
            log_flush();
        }

        if (fast) {
            re4_usleep(250000);
        } else {
            re4_sleep(1);
        }
    }

    int best = -1;
    int bestv = 0;
    for (int j = 0; j < g_pad_n; j++) {
        if (activ[j] > bestv) {
            bestv = activ[j];
            best = j;
        }
    }
    if (best < 0 || bestv < 400) {
        LOGF("   elegido: ninguno (actividad maxima %d, mandos en la lista %d)\n", bestv, g_pad_n);
        if (g_pad_n == 0) {
            LOGF("   no hay ningun mando abierto. El mando 2 tiene que estar ENCENDIDO y con\n"
                 "   sesion en OTRA cuenta o en el INVITADO (la PS4 apaga el segundo mando\n"
                 "   si eliges el mismo usuario que el mando 1). Pulsa su boton PS antes de\n"
                 "   la sonda y elige \"Jugador invitado\".\n");
        }
        log_flush();
        return -1;
    }
    LOGF("   elegido: cuenta 0x%x, handle %d, indice %d, actividad %d\n",
         g_pad_user[best], g_pad_handle[best], g_pad_which[best], bestv);
    log_flush();
    return best;
}

#define RE4COOP_PAD_HZ    120
#define RE4COOP_PAD_SPEED 1500.0f
#define RE4COOP_PAD_SECS  15
#define RE4COOP_PAD_DEAD  30

// Mover a Leon con el stick izquierdo del mando 2, en empujones pequenos y continuos
// (los mismos 120 pasos por segundo de la v1.10). El stick arriba mueve en un eje del
// mapa y el stick a los lados en el otro: todavia no giramos segun la camara.
static void re4_pad_drive(uintptr_t obj, int offx, int offz, uintptr_t parts, int slot, int step_no) {
    char msg[200];
    snprintf(msg, sizeof(msg), "AHORA el stick IZQUIERDO del mando 2: %d s", RE4COOP_PAD_SECS);
    notify_step(step_no, msg);
    re4_sleep(3);

    LOGF("\n== MANDO 2: %d s con el stick izquierdo (%.0f u/s a %d Hz) ==\n",
         RE4COOP_PAD_SECS, RE4COOP_PAD_SPEED, RE4COOP_PAD_HZ);
    LOGF("   campo x=+0x%x campo z=+0x%x\n", offx, offz);
    re4_log_float("x_inicio", re4_bits_to_float(re4_field_bits(obj, offx)));
    if (offz >= 0) {
        re4_log_float("z_inicio", re4_bits_to_float(re4_field_bits(obj, offz)));
    }
    LOGF("\n");
    log_flush();

    float start_x = re4_bits_to_float(re4_field_bits(obj, offx));
    float start_z = (offz >= 0) ? re4_bits_to_float(re4_field_bits(obj, offz)) : 0.0f;
    float want_x = 0.0f;
    float want_z = 0.0f;
    int misses = 0;
    int fails = 0;
    int steps = RE4COOP_PAD_HZ * RE4COOP_PAD_SECS;
    float per_step = RE4COOP_PAD_SPEED / (float)RE4COOP_PAD_HZ;
    int fast = (re4_usleep(1000) == 0) ? 1 : 0;
    if (!fast) {
        LOGF("   aviso: la espera corta no funciona; ire en golpes de 1 s\n");
        log_flush();
    }

    for (int i = 0; i < steps; i++) {
        int lx = 128;
        int ly = 128;
        if (re4_pad_read_one(slot) == 0) {
            lx = (int)g_pad_buf[slot][RE4_PAD_LX];
            ly = (int)g_pad_buf[slot][RE4_PAD_LY];
        } else {
            fails++;
            ly = 128;   // si no hay lectura, se queda quieto
            lx = 128;
        }
        int ax = re4_pad_stick(lx);
        int ay = re4_pad_stick(ly);
        float dx = 0.0f;
        float dz = 0.0f;
        if (ax > RE4COOP_PAD_DEAD || ax < -RE4COOP_PAD_DEAD) {
            dx = ((float)ax / 127.0f) * per_step;
        }
        if (ay > RE4COOP_PAD_DEAD || ay < -RE4COOP_PAD_DEAD) {
            dz = -((float)ay / 127.0f) * per_step;   // arriba = adelante
        }
        if (dx != 0.0f) {
            uint32_t cur = re4_field_bits(obj, offx);
            if (re4_patch_delta(obj, cur, dx) == 0) {
                misses++;
            }
            g_pat_n = 0;
        }
        if (offz >= 0 && dz != 0.0f) {
            uint32_t cur = re4_field_bits(obj, offz);
            if (re4_patch_delta(obj, cur, dz) == 0) {
                misses++;
            }
            g_pat_n = 0;
        }
        want_x += dx;
        want_z += dz;

        if ((i + 1) % RE4COOP_PAD_HZ == 0) {
            float nowx = re4_bits_to_float(re4_field_bits(obj, offx));
            LOGF("   t=%ds stick lx=%d ly=%d real_x=%.1f queria_x=%.1f desvio_x=%.1f",
                 (i + 1) / RE4COOP_PAD_HZ, lx, ly, nowx - start_x, want_x,
                 (nowx - start_x) - want_x);
            if (offz >= 0) {
                float nowz = re4_bits_to_float(re4_field_bits(obj, offz));
                LOGF(" real_z=%.1f queria_z=%.1f desvio_z=%.1f",
                     nowz - start_z, want_z, (nowz - start_z) - want_z);
            }
            LOGF(" sin_copias=%d sin_lectura=%d", misses, fails);
            re4_log_float(" giro(+0xb0)", re4_bits_to_float(re4_field_bits(obj, 0xb0)));
            LOGF("\n");
            log_flush();
        }
        if (fast) {
            re4_usleep(1000000u / (unsigned int)RE4COOP_PAD_HZ);
        } else if (((i + 1) % RE4COOP_PAD_HZ) == 0) {
            re4_sleep(1);
        }
    }

    re4_log_float("x_final", re4_bits_to_float(re4_field_bits(obj, offx)));
    if (offz >= 0) {
        re4_log_float("z_final", re4_bits_to_float(re4_field_bits(obj, offz)));
    }
    if (parts) {
        re4_log_parts(parts, "final");
    }
    LOGF("\n");
    log_flush();
}

   // "foto" de los objetos

static int re4_det_want(const char *nm) {
    return re4_contains(nm, "cPl") || re4_contains(nm, "Player") ||
           re4_contains(nm, "Camera") || re4_contains(nm, "cSub") ||
           re4_contains(nm, "cCtrl") || re4_contains(nm, "Enemy") ||
           re4_contains(nm, "Chara") || re4_contains(nm, "Pl0");
}

// Recorre la memoria de datos del juego y guarda los objetos vivos (primero los de
// clases que suenan interesantes, para que no se llenen los huecos con basura).
static void re4_det_collect(void) {
    g_ndet = 0;
    for (int pass = 0; pass < 2; pass++) {
        for (int s = 0; s < g_nseg; s++) {
            if (!(g_seg[s].prot & 2) || (g_seg[s].prot & 4)) {
                continue;   // solo memoria de datos escribible
            }
            const unsigned char *b = g_seg[s].p;
            size_t n = g_seg[s].n;
            for (size_t off = 0; off + 8 <= n; off += 8) {
                if (g_ndet >= DET_MAX) {
                    return;
                }
                uint64_t v = re4_rd64(b + off);
                if (v == 0 || re4_in_code(v)) {
                    continue;
                }
                int ci = re4_find_cen(v);
                uint64_t obj = 0;
                uint64_t vt = 0;
                if (ci >= 0) {
                    // el campo es directamente una vtable: objeto incrustado en los datos
                    obj = (uint64_t)(uintptr_t)(b + off);
                    vt = v;
                } else {
                    if ((v & 7) != 0 || v >= 0x0000800000000000ULL) {
                        continue;
                    }
                    if (!re4_in_module(v) && v < 0x100000000ULL) {
                        continue;
                    }
                    if (!re4_in_module(v) && !re4_readable(v, 8)) {
                        continue;
                    }
                    uint64_t q = re4_rd64((const unsigned char *)(uintptr_t)v);
                    ci = re4_find_cen(q);
                    if (ci < 0) {
                        continue;
                    }
                    obj = v;
                    vt = q;
                }
                int want = (g_cen[ci].name >= 0) && re4_det_want(re4_cen_name(ci));
                if (pass == 0 && !want) {
                    continue;
                }
                if (pass == 1 && want) {
                    continue;   // ya se guardo en la primera pasada
                }
                if (!re4_readable(obj, DET_BYTES)) {
                    continue;
                }
                DetEnt *d = &g_det[g_ndet];
                d->slot = (uintptr_t)(b + off);
                d->obj = (uintptr_t)obj;
                d->vt = (uintptr_t)vt;
                d->name = g_cen[ci].name;
                d->nf = g_cen[ci].nf;
                const unsigned char *p = (const unsigned char *)(uintptr_t)obj;
                unsigned char *img = &g_img[(size_t)g_ndet * DET_BYTES];
                for (int k = 0; k < DET_BYTES; k++) {
                    img[k] = p[k];
                }
                g_ndet++;
            }
        }
    }
}

static void re4_det_list(void) {
    LOGF("== candidatos guardados: %d ==\n", g_ndet);
    for (int i = 0; i < g_ndet; i++) {
        const DetEnt *d = &g_det[i];
        LOGF("C%03d slot=0x%llx obj=0x%llx vt=0x%llx n=%d %s\n",
             i, (unsigned long long)d->slot, (unsigned long long)d->obj,
             (unsigned long long)d->vt, d->nf,
             d->name >= 0 ? (const char *)g_names[d->name].addr : "?");
    }
}

// Compara la foto guardada con la memoria actual y escribe que objetos cambiaron.
static int re4_det_report(const char *label) {
    unsigned char now[DET_BYTES];
    int moved = 0;
    int printed = 0;
    int truncated = 0;

    LOGF("\n== %s ==\n", label);
    for (int i = 0; i < g_ndet; i++) {
        DetEnt *d = &g_det[i];
        unsigned char *img = &g_img[(size_t)i * DET_BYTES];

        if (!re4_readable((uint64_t)d->obj, DET_BYTES)) {
            LOGF("GONE obj=0x%llx (ya no se puede leer)\n", (unsigned long long)d->obj);
            continue;
        }
        const unsigned char *p = (const unsigned char *)(uintptr_t)d->obj;
        for (int k = 0; k < DET_BYTES; k++) {
            now[k] = p[k];
        }

        int ch = 0;
        int first = -1;
        int last = -1;
        for (int w = 0; w < DET_WORDS; w++) {
            uint64_t a = re4_rd64(img + (size_t)w * 8);
            uint64_t c = re4_rd64(now + (size_t)w * 8);
            if (a != c) {
                ch++;
                if (first < 0) {
                    first = w;
                }
                last = w;
            }
        }
        if (ch == 0) {
            continue;
        }
        moved++;

        if (printed >= DET_MAX_PRINT) {
            truncated = 1;
        } else {
            printed++;
            LOGF("M slot=0x%llx obj=0x%llx vt=0x%llx n=%d %s ch=%d first=+0x%x last=+0x%x\n",
                 (unsigned long long)d->slot, (unsigned long long)d->obj,
                 (unsigned long long)d->vt, d->nf,
                 d->name >= 0 ? (const char *)g_names[d->name].addr : "?",
                 ch, first * 8, last * 8);
            int shown = 0;
            for (int w = 0; w < DET_WORDS && shown < DET_WORDS_PRINT; w++) {
                uint64_t a = re4_rd64(img + (size_t)w * 8);
                uint64_t c = re4_rd64(now + (size_t)w * 8);
                if (a == c) {
                    continue;
                }
                shown++;
                LOGF("   +%03x %016llx -> %016llx",
                     w * 8, (unsigned long long)a, (unsigned long long)c);
                re4_log_float("l", re4_bits_to_float((uint32_t)c));
                re4_log_float("h", re4_bits_to_float((uint32_t)(c >> 32)));
                LOGF("\n");
            }
            if (ch > DET_WORDS_PRINT) {
                LOGF("   ... (%d palabras mas)\n", ch - DET_WORDS_PRINT);
            }
        }

        // La foto se actualiza al estado actual: la siguiente fase compara contra esto.
        for (int k = 0; k < DET_BYTES; k++) {
            img[k] = now[k];
        }
    }
    if (truncated) {
        LOGF("(solo se detallaron %d objetos; los demas solo cuentan)\n", DET_MAX_PRINT);
    }
    LOGF("-- %s: objetos que cambiaron = %d de %d candidatos --\n", label, moved, g_ndet);
    return moved;
}

// Hilo aparte: espera a que estes jugando y hace tres fases guiadas por mensajes.
static void *re4_scan_thread(void *arg) {
    (void)arg;

    g_probe_fd = re4_open(RE4COOP_PROBE_PATH, RE4_O_WRONLY | RE4_O_CREAT | RE4_O_TRUNC, 0777);
    log_open(RE4COOP_SCAN_PATH);
    LOGF("==== re4coop v" RE4COOP_VER_STR " movimiento + mando 2 ====\nbuild: %s\nprobe_fd=%d scan_fd=%d\n",
         BUILD_ID, g_probe_fd, g_fd);
    log_flush();

    re4_sleep(RE4COOP_FIRST_SCAN_SECS);

    // Fase 1: foto de todo, y retrato a fondo de Leon (que ya sabemos quien es).
    notify_step(7, "QUIETO AHORA: no muevas a Leon. Sacando la foto...");
    re4_det_collect();
    re4_det_list();
    log_flush();

    int leon = re4_find_leon();
    uintptr_t leon_obj = 0;
    unsigned char leon_img[DET_BYTES];
    for (int k = 0; k < DET_BYTES; k++) {
        leon_img[k] = 0;
    }
    if (leon >= 0) {
        leon_obj = (uintptr_t)g_det[leon].obj;
        LOGF("\n== LEON: candidato C%03d slot=0x%llx obj=0x%llx vt=0x%llx (sin nombre, 23 funciones)\n",
             leon, (unsigned long long)g_det[leon].slot, (unsigned long long)leon_obj,
             (unsigned long long)g_det[leon].vt);
        const unsigned char *lp = (const unsigned char *)(uintptr_t)leon_obj;
        for (int k = 0; k < DET_BYTES; k++) {
            leon_img[k] = lp[k];
        }
        re4_dump_full(leon_obj, "LEON quieto");
    } else {
        LOGF("\n== LEON: no encontrado (ninguna clase sin nombre con 23 funciones) ==\n");
    }

    int cam = re4_find_cam();
    if (cam >= 0) {
        LOGF("\n== CAMARA: candidato C%03d slot=0x%llx obj=0x%llx vt=0x%llx (sin nombre, 3 funciones)\n",
             cam, (unsigned long long)g_det[cam].slot, (unsigned long long)g_det[cam].obj,
             (unsigned long long)g_det[cam].vt);
        re4_dump_full((uintptr_t)g_det[cam].obj, "CAMARA quieta");
    }
    log_flush();

    notify_step(8, "AHORA camina recto y sin parar: cuento 12 s desde YA");
    // Los avisos de pantalla se muestran con retraso (en la v1.06 el #9 salio a los
    // pocos segundos y solo se midieron los 2 ultimos). Se espera un poco antes de
    // medir para que los 12 segundos de caminata cuenten enteros.
    re4_sleep(3);
    if (leon >= 0) {
        re4_trace(leon_obj, 12);
    } else {
        re4_sleep(12);
    }

    notify_step(9, "midiendo movimiento...");
    re4_det_report("fase 1: caminaste");
    BestMove best;
    BestMove second;
    best.off = -1;
    best.valid = 0;
    best.before = 0.0f;
    best.after = 0.0f;
    best.delta = 0.0f;
    second = best;
    if (leon >= 0) {
        best = re4_leon_moved(leon_obj, leon_img, &second);
        re4_dump_full(leon_obj, "LEON despues de caminar");
    }
    log_flush();

    // Las escrituras: un salto grande en cada eje horizontal y luego un deslizar.
    uintptr_t parts = 0;
    if (leon >= 0 && best.valid) {
        parts = re4_find_parts(leon_obj, best.off);
        LOGF("\n== cParts (el nodo que se dibuja): 0x%llx ==\n", (unsigned long long)parts);
        log_flush();
        LOGF("   campo x=+0x%x campo z=+0x%x\n", best.off, second.valid ? second.off : -1);
        log_flush();

        int npads = re4_pad_open_all();
        LOGF("\n== mandos abiertos: %d ==\n", npads);
        log_flush();
        int slot = re4_pad_probe(10, 8);   // corre siempre, aunque no haya mandos
        if (slot >= 0) {
            char m2[120];
            snprintf(m2, sizeof(m2), "mando 2 en handle %d. Ahora lo mueves tu", g_pad_handle[slot]);
            notify_step(11, m2);
            re4_pad_drive(leon_obj, best.off, second.valid ? second.off : -1, parts, slot, 12);
            if (g_pad_close != NULL) {
                for (int k = 0; k < g_pad_n; k++) {
                    if (g_pad_owned[k]) {
                        g_pad_close(g_pad_handle[k]);
                    }
                }
            }
            notify_step(13, "fin: mira donde quedo Leon y copia re4coop_scan.txt");
        } else {
            notify_step(11, "no vi el mando 2: queda anotado en el log");
            re4_sleep(2);
        }
    } else {
        notify_step(10, "sin candidato claro: no escribo nada, solo queda el volcado");
        re4_sleep(2);
    }

    log_close();
    if (g_probe_fd >= 0) {
        re4_close(g_probe_fd);
        g_probe_fd = -1;
    }
    notify_step(14, "escaneo terminado. Copia re4coop_scan.txt");
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
    LOGF("==== re4coop v" RE4COOP_VER_STR " reconocimiento ====\nbuild: %s\n", BUILD_ID);

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
        if (re4_contains(mi.name, "libScePad")) {
            g_mod_pad = handles[i];
        }
        if (re4_contains(mi.name, "libSceUserService")) {
            g_mod_user = handles[i];
        }
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

    // 6: hilo que mide en vivo cuando ya estes jugando.
    void *thread = NULL;
    int tr = re4_thread_create(&thread, NULL, re4_scan_thread, NULL, "re4coop_scan");
    if (tr != 0) {
        snprintf(msg, sizeof(msg), "no pude crear el hilo de escaneo (error 0x%08X)", (unsigned int)tr);
    } else {
        snprintf(msg, sizeof(msg), "listo. Entra a tu partida YA: la medicion empieza en %d s", RE4COOP_FIRST_SCAN_SECS);
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
