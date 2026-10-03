/* ui_avatar_def — the avatar's data and geometry, with no LVGL in it (host-tested: test/test_pet.c).
 *
 * An avatar is a body (a primary surface and up to 16 more), two colours, named expressions (each a pose of
 * the head and the two eyes) and named animations (step lists through the expressions). The file format is
 * Bible Strong's `.avatar.json`, schema v1, read from the card (ui_pet.c); the character that ships with the
 * firmware is written here, in the same terms, so there is one renderer for both and an expression the card's
 * file lacks can be borrowed from ours.
 *
 * The geometry is our own: the face is painted on a sphere, the head turns it (about the vertical axis,
 * then the horizontal, then the screen's), the eyes are projected flat and foreshortened by how far the
 * surface leans away. The two constants (how big a sphere, how far apart a unit of the file is on it) were
 * fitted to what the format's own contact sheets show, to within about 2 px.
 *
 * Parsed once into the compact struct below; the file's text is freed straight away. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* a pose: head x y z (degrees), left eye w h x y angle, right eye w h x y angle, spacing; then the body
 * colour and the eye colour as r g b (0..255): 20 numbers that are blended as one */
enum {
    P_HX, P_HY, P_HZ, P_LW, P_LH, P_LX, P_LY, P_LA, P_RW, P_RH, P_RX, P_RY, P_RA, P_SP, P_NV,
    P_BR = P_NV, P_BG, P_BB, P_ER, P_EG, P_EB, AV_NP
};

typedef struct {
    float v[P_NV];
    uint32_t cbody, ceyes; /* a colour override: 0 none, else 0x1000000 | rgb */
    uint8_t meye, mbody;   /* motion: eyes 0 none 1 microSaccades 2 shake; body 0 none 1 slowDrift 2 shake */
} av_expr_t;

typedef struct {
    char name[32];
    av_expr_t e;
} av_xname_t;

enum { AV_SPRING, AV_SMOOTH, AV_SNAPPY };
enum { AV_LOOP, AV_ONCE, AV_PINGPONG };

typedef struct {
    uint16_t expr, hold, trans;
    uint8_t kind;
} av_step_t;

typedef struct {
    char name[32];
    uint8_t mode;
    bool blink;
    uint16_t b_init, b_min, b_max, b_dur; /* ms */
    uint16_t first, n;
} av_anim_t;

enum { AV_SPHERE, AV_MICKEY, AV_CURSOR, AV_CUBE, AV_CAPSULE, AV_CYLINDER, AV_CONE, AV_DIAMOND };

typedef struct {
    uint8_t type;
    float w, h, d, round;
} av_surf_t;

#define AV_MAX_NODES 16
typedef struct {
    av_surf_t s;
    float pos[3];
} av_node_t;

typedef struct av_def {
    char name[40];
    av_surf_t prim;
    int nnodes;
    av_node_t node[AV_MAX_NODES];
    uint32_t body, eyes; /* 0xRRGGBB */
    int nx, na, ns;
    av_xname_t *x;
    av_anim_t *a;
    av_step_t *st;
    bool heap;           /* av_def_free frees it (the built-in one is static) */
    /* the geometry's constants for this body, from av_def_prepare */
    float sc;            /* the file's eye units to the 300-unit box */
    float arc, rad;      /* the sphere the face is painted on, and how fast a unit walks round it */
} av_def_t;

const av_def_t *av_builtin(void);
/* Parses a Bible Strong avatar definition. NULL, and `why` says so in a line, when it isn't one. */
av_def_t *av_def_parse(const char *js, size_t len, char *why, size_t wn);
void av_def_free(av_def_t *d);
int av_def_expr(const av_def_t *d, const char *name); /* index or -1 */
int av_def_anim(const av_def_t *d, const char *name);
/* the pose of expression i, its colours resolved against the definition's own (and `body` replacing the body
 * colour when non-zero: the built-in character takes the accent) */
void av_pose_of(const av_def_t *d, int i, uint32_t body, float P[AV_NP]);

/* ---- geometry: one eye as a stadium (a thick line with round ends), in the 300-unit box centred on 0,0 ---- */
typedef struct {
    float cx, cy;   /* centre */
    float dx, dy;   /* unit vector along its length */
    float len, wid; /* overall length (>= wid) and thickness */
} av_eye_t;

typedef struct {
    float x, y, z;  /* a node's centre after the head turns (z > 0 toward the viewer) */
} av_ear_t;

void av_geom(const av_def_t *d, const float P[AV_NP], av_eye_t eye[2], av_ear_t ear[AV_MAX_NODES]);
/* the mask of a surface: coverage 0..255 at (px, py) (box coordinates, 0,0 the centre) for a surface of
 * half-sizes hw, hh, with `round` its roundness */
float av_sdf(const av_surf_t *s, float hw, float hh, float px, float py);
