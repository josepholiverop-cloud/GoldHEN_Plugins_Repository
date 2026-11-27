#include <stdio.h>
#include <stddef.h>
#include <stdint.h>

#include "plugin_common.h"

attr_public const char *g_pluginName = "re4coop";
attr_public const char *g_pluginDesc = "RE4 coop - reconocimiento";
attr_public const char *g_pluginAuth = "re4coop";
attr_public uint32_t g_pluginVersion = 0x00000106; // 1.06

// Version en texto: sale en CADA mensaje de pantalla y en la cabecera de los logs.
// Ojo: si cambias la version, cambia tambien el numero de arriba. Nada mas.
#define RE4COOP_VER_STR "1.06"

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

static BestMove re4_leon_moved(uintptr_t obj, const unsigned char *img) {
    BestMove best;
    best.off = -1;
    best.valid = 0;
    best.before = 0.0f;
    best.after = 0.0f;
    best.delta = 0.0f;
    float bestabs = 0.0f;

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
            if ((af > 50.0f || af < -50.0f) && ad > 100.0f && ad > bestabs) {
                bestabs = ad;
                best.off = w * 8 + half * 4;
                best.before = af;
                best.after = cf;
                best.delta = d;
                best.valid = 1;
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

static void re4_unpatch(uintptr_t obj) {
    for (int i = 0; i < g_pat_n; i++) {
        uintptr_t addr = obj + (uintptr_t)g_pat[i].off;
        uintptr_t base = addr & ~(uintptr_t)7;
        uint64_t v = re4_rd64((const unsigned char *)(uintptr_t)base);
        if ((addr - base) >= 4) {
            v = (v & 0x00000000FFFFFFFFULL) | ((uint64_t)g_pat[i].bits << 32);
        } else {
            v = (v & 0xFFFFFFFF00000000ULL) | (uint64_t)g_pat[i].bits;
        }
        *(volatile uint64_t *)(void *)base = v;
    }
    g_pat_n = 0;
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
    LOGF("==== re4coop v" RE4COOP_VER_STR " deteccion de movimiento ====\nbuild: %s\nprobe_fd=%d scan_fd=%d\n",
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

    notify_step(8, "AHORA camina recto y sin parar 8 s, luego sueltate");
    if (leon >= 0) {
        re4_trace(leon_obj, 8);
    } else {
        re4_sleep(8);
    }

    notify_step(9, "midiendo movimiento...");
    re4_det_report("fase 1: caminaste");
    BestMove best;
    best.off = -1;
    best.valid = 0;
    best.before = 0.0f;
    best.after = 0.0f;
    best.delta = 0.0f;
    if (leon >= 0) {
        best = re4_leon_moved(leon_obj, leon_img);
        re4_dump_full(leon_obj, "LEON despues de caminar");
    }
    log_flush();

    // Prueba de escritura: si el campo elegido es la posicion, Leon se mueve ~10 metros.
    if (leon >= 0 && best.valid) {
        notify_step(10, "voy a mover a Leon por codigo: mira la pantalla");
        LOGF("\n== PRUEBA DE ESCRITURA ==\ncampo elegido: +0x%x\n", best.off);
        re4_log_float("valor", best.before);
        re4_log_float("cambio_al_caminar", best.delta);
        LOGF("\nvoy a sumar 1000.0 (unos 10 m) a ese valor y a sus copias\n");
        log_flush();
        int hits = re4_patch_delta(leon_obj, re4_float_to_bits(best.before), 1000.0f);
        LOGF("campos escritos: %d\n", hits);
        for (int i = 0; i < g_pat_n; i++) {
            LOGF("   escrito +0x%x\n", g_pat[i].off);
        }
        log_flush();
        re4_sleep(4);
        notify_step(11, "lo devuelvo a su sitio...");
        re4_unpatch(leon_obj);
        LOGF("campos restaurados a su valor original\n");
        log_flush();
        re4_sleep(1);
    } else {
        notify_step(10, "sin candidato claro: no escribo nada, solo queda el volcado");
        re4_sleep(2);
    }

    log_close();
    if (g_probe_fd >= 0) {
        re4_close(g_probe_fd);
        g_probe_fd = -1;
    }
    notify_step(12, "escaneo terminado. Copia re4coop_scan.txt");
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
