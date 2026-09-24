/* A minimal PNG writer for the simulator's screenshots (zlib does the compression). */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

static void be32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }

static void chunk(FILE *f, const char *type, const uint8_t *data, uint32_t len)
{
    uint8_t hdr[8];
    be32(hdr, len);
    memcpy(hdr + 4, type, 4);
    fwrite(hdr, 1, 8, f);
    if (len) fwrite(data, 1, len, f);
    uint32_t crc = crc32(0, (const Bytef *)type, 4);
    if (len) crc = crc32(crc, data, len);
    uint8_t c[4];
    be32(c, crc);
    fwrite(c, 1, 4, f);
}

int png_write_rgb565(const char *path, const uint16_t *px, int w, int h)
{
    size_t raw_len = (size_t)(w * 3 + 1) * h;
    uint8_t *raw = malloc(raw_len);
    if (!raw) return -1;
    for (int y = 0; y < h; y++) {
        uint8_t *row = raw + (size_t)y * (w * 3 + 1);
        row[0] = 0;
        for (int x = 0; x < w; x++) {
            uint16_t p = px[(size_t)y * w + x];
            int r = (p >> 11) & 31, g = (p >> 5) & 63, b = p & 31;
            row[1 + x * 3] = (uint8_t)(r << 3 | r >> 2);
            row[2 + x * 3] = (uint8_t)(g << 2 | g >> 4);
            row[3 + x * 3] = (uint8_t)(b << 3 | b >> 2);
        }
    }
    uLongf zlen = compressBound(raw_len);
    uint8_t *z = malloc(zlen);
    if (!z || compress2(z, &zlen, raw, raw_len, 6) != Z_OK) {
        free(raw);
        free(z);
        return -1;
    }
    FILE *f = fopen(path, "wb");
    if (!f) {
        free(raw);
        free(z);
        return -1;
    }
    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n' };
    fwrite(sig, 1, 8, f);
    uint8_t ihdr[13];
    be32(ihdr, (uint32_t)w);
    be32(ihdr + 4, (uint32_t)h);
    ihdr[8] = 8; ihdr[9] = 2; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
    chunk(f, "IHDR", ihdr, 13);
    chunk(f, "IDAT", z, (uint32_t)zlen);
    chunk(f, "IEND", NULL, 0);
    fclose(f);
    free(raw);
    free(z);
    return 0;
}
