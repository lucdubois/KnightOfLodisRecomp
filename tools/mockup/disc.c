// Mockup: variable-radius disc (bokeh-style) blur of a presented frame.
// disc sharp.bmp vis.bmp out.bmp rmax_px highlight
// vis encodes each pixel's blur as (b, 0, 255-b); anything else is HUD/sky: radius 0.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
typedef struct { int w, h; unsigned char* px; } Img;   // RGB, top-down
static Img load(const char* p) {
    FILE* f = fopen(p, "rb"); fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char* b = malloc(n); fread(b, 1, n, f); fclose(f);
    int off = *(int*)(b + 10), w = *(int*)(b + 18), h = *(int*)(b + 22), bpp = *(short*)(b + 28) / 8;
    int flip = h > 0; h = abs(h); int rs = (w * bpp + 3) / 4 * 4;
    Img im = {w, h, malloc(w * h * 3)};
    for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
        unsigned char* s = b + off + (flip ? h - 1 - y : y) * rs + x * bpp;
        unsigned char* d = im.px + (y * w + x) * 3;
        d[0] = s[2]; d[1] = s[1]; d[2] = s[0];
    }
    free(b); return im;
}
static void save(const char* p, Img im) {
    int rs = (im.w * 3 + 3) / 4 * 4, sz = 54 + rs * im.h;
    unsigned char* b = calloc(1, sz);
    b[0] = 'B'; b[1] = 'M'; *(int*)(b + 2) = sz; *(int*)(b + 10) = 54; *(int*)(b + 14) = 40;
    *(int*)(b + 18) = im.w; *(int*)(b + 22) = im.h; *(short*)(b + 26) = 1; *(short*)(b + 28) = 24;
    for (int y = 0; y < im.h; ++y) for (int x = 0; x < im.w; ++x) {
        unsigned char* s = im.px + (y * im.w + x) * 3; unsigned char* d = b + 54 + (im.h - 1 - y) * rs + x * 3;
        d[0] = s[2]; d[1] = s[1]; d[2] = s[0];
    }
    FILE* f = fopen(p, "wb"); fwrite(b, 1, sz, f); fclose(f); free(b);
}
int main(int argc, char** argv) {
    Img sh = load(argv[1]), vis = load(argv[2]);
    float rmax = atof(argv[4]), hl = atof(argv[5]);
    int w = sh.w, h = sh.h; float* r = malloc(sizeof(float) * w * h);
    for (int i = 0; i < w * h; ++i) {
        unsigned char* v = vis.px + i * 3;
        r[i] = (v[1] <= 2 && abs(v[0] + v[2] - 255) <= 2) ? v[0] / 255.0f * rmax : 0.0f;
    }
    Img out = {w, h, malloc(w * h * 3)};
    int R = (int)ceilf(rmax) + 1;
    for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
        int i = y * w + x; float ri = r[i];
        if (ri < 0.5f) { memcpy(out.px + i * 3, sh.px + i * 3, 3); continue; }
        double acc[3] = {0, 0, 0}, wt = 0;
        for (int dy = -R; dy <= R; ++dy) for (int dx = -R; dx <= R; ++dx) {
            int xx = x + dx, yy = y + dy; if (xx < 0 || yy < 0 || xx >= w || yy >= h) continue;
            int j = yy * w + xx; float d = sqrtf(dx * dx + dy * dy);
            float rr = fminf(ri, r[j]); if (rr < 0.5f) rr = 0.5f;
            float k = rr - d + 0.5f; if (k <= 0) continue; if (k > 1) k = 1;
            // A disc spreads a point's light over its area; bright points
            // (highlights) weigh a little more, as they do through a lens.
            unsigned char* c = sh.px + j * 3;
            float lum = (0.3f * c[0] + 0.59f * c[1] + 0.11f * c[2]) / 255.0f;
            k *= 1.0f + hl * lum * lum * lum * lum;
            acc[0] += k * c[0]; acc[1] += k * c[1]; acc[2] += k * c[2]; wt += k;
        }
        for (int c = 0; c < 3; ++c) out.px[i * 3 + c] = (unsigned char)(acc[c] / wt + 0.5);
    }
    save(argv[3], out); return 0;
}
