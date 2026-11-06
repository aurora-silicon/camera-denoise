// SPDX-License-Identifier: MIT
layout(set=0,binding=0,std430) readonly buffer InputY { uint iy[]; };
layout(set=0,binding=1,std430) readonly buffer InputUV { uint ic[]; };
layout(set=0,binding=2,std430) buffer History { int hist[]; };
layout(set=0,binding=3,std430) buffer Scratch { int tmp[]; };
layout(set=0,binding=4,std430) writeonly buffer Output { uint dst[]; };
layout(set=0,binding=5,std430) buffer Params { int par[]; };
layout(push_constant) uniform Push { int w, h, sy, sc, oy, oc, strength, seed; } pc;
// History: Y, UV (Q4), age. Scratch: diff, horizontal signed sum, weight,
// temporal Y/UV, first spatial UV. All offsets are in 32-bit words.
#define N (pc.w * pc.h)
#define HY 0
#define HC N
#define AGE (N + N/2)
#define DIFF 0
#define HS N
#define KY (2*N)
#define TY (3*N)
#define TC (4*N)
#define TC2 (4*N + N/2)
#define MED 16
int src(int x, int y, bool chroma) {
    int a = (chroma ? pc.oc + y*pc.sc : pc.oy + y*pc.sy) + x;
    uint v = chroma ? ic[a>>2] : iy[a>>2];
    return int((v >> ((a&3)*8)) & 255u);
}
int sat(int x) { return clamp(x, 0, 255); }
int mulq(int a, int b) { return (a*b + 16384) >> 15; }
int blend(int a, int b) { return sat(a + (((b-a)*pc.strength + 128)>>8)); }
int at(int x, int y) { return clamp(y,0,pc.h-1)*pc.w + clamp(x,0,pc.w-1); }
int ramp(int m, int lo, int range, int scale, int shift) {
    return int((uint(clamp(m-lo,0,range))*uint(scale)) >> shift);
}
