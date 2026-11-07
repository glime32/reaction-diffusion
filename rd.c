/*
 * rd — réaction-diffusion de Gray-Scott, en direct dans le terminal.
 *
 * Deux produits chimiques U et V diffusent sur une grille et réagissent :
 * U + 2V -> 3V. On ajoute du U en permanence (taux F) et on retire du V
 * (taux F + k). Selon (F, k), il en sort des coraux, des taches qui se
 * divisent comme des cellules, des labyrinthes, des vers...
 *
 *   dU/dt = Du ΔU − U·V² + F·(1 − U)
 *   dV/dt = Dv ΔV + U·V² − (F + k)·V
 *
 * Affichage : chaque caractère '▀' montre DEUX cases (couleur du texte en
 * haut, couleur du fond en bas), en couleurs 24 bits. La souris dépose du V.
 */
#define _DEFAULT_SOURCE
#include <errno.h>
#include <math.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

typedef struct { const char *nom; float F, k; } Preset;
static Preset PRESETS[] = {
    {"corail", 0.0545f, 0.062f},   {"mitose", 0.0367f, 0.0649f}, {"labyrinthe", 0.029f, 0.057f},
    {"solitons", 0.030f, 0.062f},  {"vers", 0.046f, 0.063f},      {"trous", 0.039f, 0.058f},
    {"pulsations", 0.025f, 0.060f}, {"chaos", 0.022f, 0.051f},    {"u-skate", 0.062f, 0.0609f},
};
#define NB_PRESETS (int)(sizeof PRESETS / sizeof PRESETS[0])

typedef struct { const char *nom; float c[5][3]; } Palette;
static const Palette PALETTES[] = {
    {"violet", {{0, 0, 0}, {0.18f, 0, 0.26f}, {0.38f, 0, 0.54f}, {0.85f, 0.25f, 0.9f}, {1, 0.92f, 1}}},
    {"magma", {{0, 0, 0.02f}, {0.32f, 0.07f, 0.43f}, {0.72f, 0.21f, 0.47f}, {0.99f, 0.53f, 0.38f}, {0.99f, 0.99f, 0.75f}}},
    {"glace", {{0, 0.01f, 0.04f}, {0.02f, 0.12f, 0.3f}, {0.1f, 0.4f, 0.7f}, {0.5f, 0.85f, 0.95f}, {1, 1, 1}}},
    {"mousse", {{0.01f, 0.03f, 0.01f}, {0.05f, 0.2f, 0.08f}, {0.2f, 0.5f, 0.15f}, {0.65f, 0.85f, 0.3f}, {0.97f, 1, 0.85f}}},
};
#define NB_PALETTES (int)(sizeof PALETTES / sizeof PALETTES[0])

/* ------------------------------------------------------------- simulation */

static int W, H;              /* taille de la grille (H = 2 × lignes du terminal) */
static float *U, *V, *U2, *V2; /* état et tampon */
static float *Fc, *kc;        /* F et k de chaque case (différents en mode carte) */
static int carte;             /* 1 : F et k varient dans l'espace (carte de Pearson) */
static int preset;
static const float Du = 1.0f, Dv = 0.5f;

static uint32_t graine = 2463534242u;
static float hasard(void) { /* xorshift32 */
    graine ^= graine << 13; graine ^= graine >> 17; graine ^= graine << 5;
    return (graine >> 8) / 16777216.0f;
}

static void depose(int cx, int cy, int r) {
    for (int y = cy - r; y <= cy + r; y++)
        for (int x = cx - r; x <= cx + r; x++) {
            if ((x - cx) * (x - cx) + (y - cy) * (y - cy) > r * r) continue;
            int xx = (x + W) % W, yy = (y + H) % H;
            U[yy * W + xx] = 0.5f;
            V[yy * W + xx] = 0.25f + 0.02f * hasard();
        }
}

static void parametres(void) {
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            int i = y * W + x;
            if (carte) { /* carte de Pearson : k en abscisse, F en ordonnée (F grand en haut) */
                kc[i] = 0.030f + 0.040f * x / (W - 1);
                Fc[i] = 0.080f - 0.080f * y / (H - 1);
            } else {
                kc[i] = PRESETS[preset].k;
                Fc[i] = PRESETS[preset].F;
            }
        }
}

static void reinitialise(void) {
    for (int i = 0; i < W * H; i++) { U[i] = 1; V[i] = 0; }
    if (carte) { /* partout un peu de V, pour que chaque région montre son motif */
        for (int n = 0; n < W * H / 60; n++) depose((int)(hasard() * W), (int)(hasard() * H), 1 + (int)(hasard() * 2));
    } else {
        int n = 6 + (int)(hasard() * 6);
        for (int k = 0; k < n; k++)
            depose(W / 2 + (int)((hasard() - 0.5f) * W * 0.4f), H / 2 + (int)((hasard() - 0.5f) * H * 0.4f), 2 + (int)(hasard() * 3));
    }
    parametres();
}

static void alloue_grille(int w, int h) {
    W = w < 8 ? 8 : w;
    H = h < 8 ? 8 : h;
    size_t n = (size_t)W * H;
    float **tous[] = {&U, &V, &U2, &V2, &Fc, &kc};
    for (int i = 0; i < 6; i++) {
        free(*tous[i]);
        *tous[i] = malloc(n * sizeof(float));
        if (!*tous[i]) { fputs("plus de mémoire\n", stderr); exit(1); }
    }
    reinitialise();
}

/* un pas d'Euler ; laplacien 3×3 (0.05 aux coins, 0.2 sur les côtés, −1 au centre), bords repliés */
static void pas(void) {
    for (int y = 0; y < H; y++) {
        int ym = (y + H - 1) % H * W, y0 = y * W, yp = (y + 1) % H * W;
        for (int x = 0; x < W; x++) {
            int xm = (x + W - 1) % W, xp = (x + 1) % W, i = y0 + x;
            float lu = 0.2f * (U[ym + x] + U[yp + x] + U[y0 + xm] + U[y0 + xp])
                     + 0.05f * (U[ym + xm] + U[ym + xp] + U[yp + xm] + U[yp + xp]) - U[i];
            float lv = 0.2f * (V[ym + x] + V[yp + x] + V[y0 + xm] + V[y0 + xp])
                     + 0.05f * (V[ym + xm] + V[ym + xp] + V[yp + xm] + V[yp + xp]) - V[i];
            float u = U[i], v = V[i], uvv = u * v * v;
            U2[i] = u + Du * lu - uvv + Fc[i] * (1 - u);
            float nv = v + Dv * lv + uvv - (Fc[i] + kc[i]) * v;
            /* V tend vers 0 loin des motifs : sans ce seuil on tombe dans les
             * nombres « sous-normaux », que le processeur calcule ~20× plus lentement */
            V2[i] = nv < 1e-20f ? 0.0f : nv;
        }
    }
    float *t = U; U = U2; U2 = t;
    t = V; V = V2; V2 = t;
}

/* ---------------------------------------------------------------- couleurs */

static int palette;

static void couleur(float v, uint8_t rgb[3]) {
    float t = v * 3.2f; /* V dépasse rarement 0.3 */
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    const Palette *p = &PALETTES[palette];
    float u = t * 4;
    int i = (int)u;
    if (i > 3) i = 3;
    float f = u - i;
    for (int c = 0; c < 3; c++) {
        float x = p->c[i][c] + f * (p->c[i + 1][c] - p->c[i][c]);
        rgb[c] = (uint8_t)(x * 255 + 0.5f);
    }
}

/* image PNG non compressée (pour --export) */
static uint32_t crc_t[256];
static void be32(FILE *f, uint32_t v) { uint8_t b[4] = {v >> 24, v >> 16, v >> 8, v}; fwrite(b, 1, 4, f); }
static uint32_t crc(uint32_t c, const uint8_t *b, size_t n) { while (n--) c = crc_t[(c ^ *b++) & 0xFF] ^ (c >> 8); return c; }
static void chunk(FILE *f, const char *t, const uint8_t *d, size_t n) {
    be32(f, (uint32_t)n);
    fwrite(t, 1, 4, f);
    fwrite(d, 1, n, f);
    be32(f, ~crc(crc(~0u, (const uint8_t *)t, 4), d, n));
}
static int exporte_png(const char *chemin, int echelle) {
    for (uint32_t n = 0; n < 256; n++) {
        uint32_t c = n;
        for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_t[n] = c;
    }
    int w = W * echelle, h = H * echelle;
    size_t ligne = (size_t)w * 3 + 1, n = ligne * h;
    uint8_t *brut = malloc(n), *z = malloc(n + (n / 65535 + 1) * 5 + 6), *p = z;
    if (!brut || !z) return -1;
    for (int y = 0; y < h; y++) {
        brut[y * ligne] = 0;
        for (int x = 0; x < w; x++) couleur(V[(y / echelle) * W + x / echelle], brut + y * ligne + 1 + x * 3);
    }
    *p++ = 0x78; *p++ = 1;
    uint32_t a = 1, b = 0;
    for (size_t pos = 0; pos < n;) {
        size_t k = n - pos > 65535 ? 65535 : n - pos;
        *p++ = pos + k == n; *p++ = k; *p++ = k >> 8; *p++ = ~k; *p++ = ~k >> 8;
        memcpy(p, brut + pos, k);
        for (size_t i = 0; i < k; i++) { a = (a + brut[pos + i]) % 65521; b = (b + a) % 65521; }
        p += k; pos += k;
    }
    uint32_t ad = b << 16 | a;
    *p++ = ad >> 24; *p++ = ad >> 16; *p++ = ad >> 8; *p++ = ad;
    FILE *f = fopen(chemin, "wb");
    if (!f) return -1;
    fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
    uint8_t ihdr[13] = {w >> 24, w >> 16, w >> 8, w, h >> 24, h >> 16, h >> 8, h, 8, 2, 0, 0, 0};
    chunk(f, "IHDR", ihdr, 13);
    chunk(f, "IDAT", z, (size_t)(p - z));
    chunk(f, "IEND", NULL, 0);
    fclose(f);
    free(brut); free(z);
    return 0;
}

/* ---------------------------------------------------------------- terminal */

static struct termios ancien;
static char *sortie;
static size_t sortie_cap;
static volatile sig_atomic_t redimensionne = 1, quitter;

static void restaure(void) {
    /* souris off, curseur visible, retour à l'écran normal */
    fputs("\033[?1002l\033[?1006l\033[0m\033[?25h\033[?1049l", stdout);
    fflush(stdout);
    tcsetattr(0, TCSANOW, &ancien);
}
static void sur_signal(int s) {
    if (s == SIGWINCH) redimensionne = 1;
    else quitter = 1;
}

static void dessine(int cols, int lignes, double fps, int pas_par_image, int en_pause) {
    size_t besoin = (size_t)cols * (lignes + 1) * 48 + 1024;
    if (besoin > sortie_cap) { sortie_cap = besoin; sortie = realloc(sortie, sortie_cap); }
    char *p = sortie;
    p += sprintf(p, "\033[H");
    int dernier_h = -1, dernier_b = -1;
    for (int l = 0; l < lignes - 1; l++) {
        if (l) { *p++ = '\r'; *p++ = '\n'; }
        dernier_h = dernier_b = -1;
        for (int x = 0; x < cols && x < W; x++) {
            uint8_t a[3], b[3];
            couleur(V[(2 * l) * W + x], a);
            couleur(V[(2 * l + 1) * W + x], b);
            int ha = a[0] << 16 | a[1] << 8 | a[2], hb = b[0] << 16 | b[1] << 8 | b[2];
            if (ha != dernier_h) { p += sprintf(p, "\033[38;2;%d;%d;%dm", a[0], a[1], a[2]); dernier_h = ha; }
            if (hb != dernier_b) { p += sprintf(p, "\033[48;2;%d;%d;%dm", b[0], b[1], b[2]); dernier_b = hb; }
            memcpy(p, "\xe2\x96\x80", 3); /* ▀ */
            p += 3;
        }
    }
    char etat[256];
    if (carte)
        snprintf(etat, sizeof etat, " carte de Pearson : k %.3f→%.3f (→)  F %.3f→%.3f (↑) │ %s%d pas/image │ %.0f img/s │ m retour  q quitter",
                 0.030, 0.070, 0.0, 0.080, en_pause ? "PAUSE │ " : "", pas_par_image, fps);
    else
        snprintf(etat, sizeof etat, " %d %s  F=%.4f k=%.4f │ %s%d pas/image │ %.0f img/s │ 1-9 motifs  clic dépose  espace  r  c couleurs  +/-  m carte  q",
                 preset + 1, PRESETS[preset].nom, PRESETS[preset].F, PRESETS[preset].k, en_pause ? "PAUSE │ " : "", pas_par_image, fps);
    p += sprintf(p, "\r\n\033[0m\033[38;2;200;170;230m\033[48;2;20;8;30m%.*s\033[K\033[0m", cols, etat);
    fwrite(sortie, 1, (size_t)(p - sortie), stdout);
    fflush(stdout);
}

static double maintenant(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

/* lit les touches et la souris (mode SGR : ESC [ < bouton ; x ; y M) */
static void lit_entrees(int *pas_par_image, int *en_pause, int cols, int lignes) {
    char b[256];
    ssize_t n = read(0, b, sizeof b - 1);
    if (n <= 0) return;
    b[n] = 0;
    for (ssize_t i = 0; i < n; i++) {
        if (b[i] == '\033' && i + 2 < n && b[i + 1] == '[' && b[i + 2] == '<') {
            int bouton, x, y;
            char fin;
            if (sscanf(b + i + 3, "%d;%d;%d%c", &bouton, &x, &y, &fin) == 4) {
                if ((fin == 'M') && (bouton & 3) == 0 && y < lignes) depose(x - 1, 2 * (y - 1), 3);
                if ((fin == 'M') && (bouton & 3) == 2) { /* clic droit : efface */
                    for (int dy = -6; dy <= 6; dy++)
                        for (int dx = -3; dx <= 3; dx++) {
                            int xx = (x - 1 + dx + W) % W, yy = (2 * (y - 1) + dy + H) % H;
                            U[yy * W + xx] = 1; V[yy * W + xx] = 0;
                        }
                }
            }
            while (i < n && b[i] != 'M' && b[i] != 'm') i++;
            continue;
        }
        if (b[i] == '\033' && i + 1 < n && b[i + 1] == '[') { /* autre séquence (flèches...) : ignorée */
            i += 2;
            while (i < n && !(b[i] >= 0x40 && b[i] <= 0x7E)) i++;
            continue;
        }
        switch (b[i]) {
        case 'q': case 'Q': quitter = 1; break;
        case ' ': *en_pause = !*en_pause; break;
        case 'r': reinitialise(); break;
        case 'c': palette = (palette + 1) % NB_PALETTES; break;
        case 'm': carte = !carte; reinitialise(); break;
        case '+': case '=': if (*pas_par_image < 200) *pas_par_image += *pas_par_image < 10 ? 1 : 5; break;
        case '-': if (*pas_par_image > 1) *pas_par_image -= *pas_par_image <= 10 ? 1 : 5; break;
        default:
            if (b[i] >= '1' && b[i] <= '0' + NB_PRESETS) { preset = b[i] - '1'; carte = 0; reinitialise(); }
        }
    }
    (void)cols;
}

static void aide(void) {
    puts("rd — réaction-diffusion de Gray-Scott dans le terminal\n\n"
         "  rd [motif]                      animation (motifs : corail mitose labyrinthe solitons vers\n"
         "                                  trous pulsations chaos u-skate, ou carte)\n"
         "  rd [motif] --export f.png [--etapes N] [--taille LxH] [--palette nom] [--echelle E]\n\n"
         "  touches : 1-9 motif, clic gauche dépose du V, clic droit efface, espace pause,\n"
         "            r recommencer, c couleurs, +/- vitesse, m carte des paramètres, q quitter");
}

int main(int argc, char **argv) {
    const char *export = NULL;
    int etapes = 10000, ew = 320, eh = 200, echelle = 2;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--export") && i + 1 < argc) export = argv[++i];
        else if (!strcmp(argv[i], "--etapes") && i + 1 < argc) etapes = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--taille") && i + 1 < argc) { if (sscanf(argv[++i], "%dx%d", &ew, &eh) != 2) { aide(); return 1; } }
        else if (!strcmp(argv[i], "--echelle") && i + 1 < argc) echelle = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--palette") && i + 1 < argc) {
            const char *nom = argv[++i];
            for (int k = 0; k < NB_PALETTES; k++) if (!strcmp(nom, PALETTES[k].nom)) palette = k;
        } else if (!strcmp(argv[i], "--graine") && i + 1 < argc) graine = (uint32_t)strtoul(argv[++i], NULL, 10) | 1;
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--aide")) { aide(); return 0; }
        else if (!strcmp(argv[i], "carte")) carte = 1;
        else if (!strcmp(argv[i], "--F") && i + 1 < argc) PRESETS[preset].F = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--k") && i + 1 < argc) PRESETS[preset].k = (float)atof(argv[++i]);
        else {
            int ok = 0;
            for (int k = 0; k < NB_PRESETS; k++) if (!strcmp(argv[i], PRESETS[k].nom)) { preset = k; ok = 1; }
            if (!ok) { fprintf(stderr, "argument inconnu : %s\n", argv[i]); aide(); return 1; }
        }
    }

    if (export) { /* sans terminal : simuler puis écrire une image */
        alloue_grille(ew, eh);
        double t0 = maintenant();
        for (int s = 0; s < etapes; s++) pas();
        double dt = maintenant() - t0;
        if (exporte_png(export, echelle < 1 ? 1 : echelle)) { perror(export); return 1; }
        fprintf(stderr, "%s : %d pas sur %d×%d en %.2f s (%.0f M cases/s) -> %s\n", carte ? "carte" : PRESETS[preset].nom,
                etapes, W, H, dt, (double)etapes * W * H / dt / 1e6, export);
        return 0;
    }

    if (!isatty(0) || !isatty(1)) { fputs("rd a besoin d'un terminal (ou utilise --export)\n", stderr); return 1; }
    tcgetattr(0, &ancien);
    struct termios brut = ancien;
    brut.c_lflag &= ~(ICANON | ECHO);
    brut.c_cc[VMIN] = 0;
    brut.c_cc[VTIME] = 0;
    tcsetattr(0, TCSANOW, &brut);
    atexit(restaure);
    signal(SIGWINCH, sur_signal);
    signal(SIGINT, sur_signal);
    signal(SIGTERM, sur_signal);
    /* écran alternatif, curseur caché, souris (glisser) en mode SGR */
    fputs("\033[?1049h\033[?25l\033[?1002h\033[?1006h\033[2J", stdout);

    int cols = 80, lignes = 24, pas_par_image = 12, en_pause = 0;
    double fps = 0, derniere = maintenant();
    while (!quitter) {
        if (redimensionne) {
            struct winsize ws;
            if (ioctl(1, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) { cols = ws.ws_col; lignes = ws.ws_row; }
            alloue_grille(cols, 2 * (lignes - 1));
            fputs("\033[2J", stdout);
            redimensionne = 0;
        }
        lit_entrees(&pas_par_image, &en_pause, cols, lignes);
        if (!en_pause)
            for (int s = 0; s < pas_par_image; s++) pas();
        dessine(cols, lignes, fps, pas_par_image, en_pause);
        double t = maintenant(), duree = t - derniere;
        if (duree < 1.0 / 30) { /* au plus 30 images par seconde */
            struct timespec d = {0, (long)((1.0 / 30 - duree) * 1e9)};
            nanosleep(&d, NULL);
            t = maintenant();
            duree = t - derniere;
        }
        fps = 0.9 * fps + 0.1 / duree;
        derniere = t;
    }
    return 0;
}
