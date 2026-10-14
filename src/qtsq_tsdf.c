/*
 * QTSQ TSDF — CPU volumetric fusion + marching-cubes mesh extraction
 * (implementation)
 *
 * Phase F of the live-surfel capture roadmap: posed depth maps rendered from
 * the trained 2DGS model are fused into a truncated signed-distance field —
 * two floats per voxel (averaged SDF + weight), so a 128^3 grid costs 16 MB —
 * and extracted as a triangle mesh with marching cubes.
 *
 * Marching cubes is generated, not table-driven: the set of crossed edges of
 * a cell follows from the per-corner sign bitmask (the classic 12-edge "edge
 * table"), and the triangulation is derived per cell from the 2D cut pattern
 * of each of the six cube faces: classic 2D marching-squares segments
 * (ambiguous alternating faces resolved with the bilinear asymptotic
 * decider), stitched into closed loops and fanned. No static tables, no
 * mutable statics — thread-safe, deterministic.
 *
 * SDF sign convention (Phase F spec): SDF = depth(voxel) − measured_depth
 * along the camera forward axis, clamped to ±trunc. A voxel on the far side
 * of a measured surface (inside the solid) is therefore positive; marching
 * cubes treats sdf > 0 as inside, and per-vertex normals are the NEGATED SDF
 * gradient so they point away from the solid, matching the winding.
 *
 * Author: Haruhito
 */

#include "qtsq_tsdf.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <float.h>

/* ══════════════════════════════════════════════════════════════
 *  Cube topology — canonical marching-cubes layout
 * ══════════════════════════════════════════════════════════════ */

/* corner offsets in (x, y, z) grid steps */
static const int8_t TSDF_CORNER_DXYZ[8][3] = {
    {0,0,0},{1,0,0},{1,1,0},{0,1,0},
    {0,0,1},{1,0,1},{1,1,1},{0,1,1}
};

/* canonical 12 edges: e0..e3 bottom ring, e4..e7 top ring, e8..e11 uprights */
static const uint8_t TSDF_EDGE_CORNERS[12][2] = {
    {0,1},{1,2},{2,3},{3,0},
    {4,5},{5,6},{6,7},{7,4},
    {0,4},{1,5},{2,6},{3,7}
};

/* edge index for an unordered corner pair (0xFF = not a cube edge) */
static const uint8_t TSDF_EDGE_OF[8][8] = {
    {0xFF,   0,0xFF,   3,   8,0xFF,0xFF,0xFF},
    {   0,0xFF,   1,0xFF,0xFF,   9,0xFF,0xFF},
    {0xFF,   1,0xFF,   2,0xFF,0xFF,  10,0xFF},
    {   3,0xFF,   2,0xFF,0xFF,0xFF,0xFF,  11},
    {   8,0xFF,0xFF,0xFF,0xFF,   4,0xFF,   7},
    {0xFF,   9,0xFF,0xFF,   4,0xFF,   5,0xFF},
    {0xFF,0xFF,  10,0xFF,0xFF,   5,0xFF,   6},
    {0xFF,0xFF,0xFF,  11,   7,0xFF,   6,0xFF}
};

/* the six faces as cyclic corner runs, for the 2D cut computation */
static const uint8_t TSDF_FACE_CORNERS[6][4] = {
    {0,1,2,3},  /* bottom (z = 0) */
    {4,5,6,7},  /* top    (z = 1) */
    {0,1,5,4},  /* front  (y = 0) */
    {1,2,6,5},  /* right  (x = 1) */
    {2,3,7,6},  /* back   (y = 1) */
    {3,0,4,7}   /* left   (x = 0) */
};

/* ══════════════════════════════════════════════════════════════
 *  Grid bounds + field fusion
 * ══════════════════════════════════════════════════════════════ */

void qtsq_tsdf_grid_bounds(const qtsq_tsdf_camera_t *cams, int num_maps,
                           float near_d, float far_d,
                           float center[3], float half_extent[3]){
    int k;
    for (k=0;k<3;k++){ center[k]=0.0f; half_extent[k]=0.0f; }
    if (!cams || !center || !half_extent || num_maps<=0 ||
        near_d<=0.0f || near_d>=far_d) return;

    float mn[3]={FLT_MAX,FLT_MAX,FLT_MAX}, mx[3]={-FLT_MAX,-FLT_MAX,-FLT_MAX};
    int any=0;
    for (int c=0;c<num_maps;c++){
        const qtsq_tsdf_camera_t *cam=&cams[c];
        if (cam->width<=0 || cam->height<=0 ||
            !(cam->fx>0.0f) || !(cam->fy>0.0f)) continue;
        /* pixel-corner rays at near/far depth sweep the frustum box */
        float ndx[2]={ (0.5f-cam->cx)/cam->fx,
                       (cam->width-0.5f-cam->cx)/cam->fx };
        float ndy[2]={ (0.5f-cam->cy)/cam->fy,
                       (cam->height-0.5f-cam->cy)/cam->fy };
        for (int a=0;a<2;a++){
            for (int b=0;b<2;b++){
                float d0[3];
                for (k=0;k<3;k++)
                    d0[k]=cam->forward[k]+ndx[a]*cam->right[k]+ndy[b]*cam->up[k];
                for (int s=0;s<2;s++){
                    float d = s ? far_d : near_d;
                    for (k=0;k<3;k++){
                        float p = cam->pos[k]+d*d0[k];
                        if (p<mn[k]) mn[k]=p;
                        if (p>mx[k]) mx[k]=p;
                    }
                    any=1;
                }
            }
        }
    }
    if (!any) return;
    for (k=0;k<3;k++){
        center[k]=0.5f*(mn[k]+mx[k]);
        half_extent[k]=0.5f*(mx[k]-mn[k]);
    }
}

/* one voxel: averaged SDF + accumulated weight (8 bytes — the whole field) */
typedef struct { float sdf, weight; } tsdf_voxel_t;

static size_t tsdf_voxel_index(int n, int x, int y, int z){
    return ((size_t)z*n + y)*n + x;
}

static tsdf_voxel_t *tsdf_field_alloc(int n){
    return (tsdf_voxel_t*)calloc((size_t)n*n*n, sizeof(tsdf_voxel_t));
}

/* Ray every voxel through every camera: constant weight 1.0 per measurement,
 * SDF = depth(voxel) − measured depth, kept only within trunc of the surface
 * and averaged over the cameras that saw the voxel. Voxels no camera
 * measured stay at weight 0. */
static void tsdf_fuse(tsdf_voxel_t *f, int n, float voxel, const float gmin[3],
                      const float *const *depth_maps,
                      const qtsq_tsdf_camera_t *cams, int num_maps,
                      int depth_w, int depth_h, float trunc){
    for (int zi=0;zi<n;zi++){
        float pz = gmin[2]+((float)zi+0.5f)*voxel;
        for (int yi=0;yi<n;yi++){
            float py = gmin[1]+((float)yi+0.5f)*voxel;
            for (int xi=0;xi<n;xi++){
                float px = gmin[0]+((float)xi+0.5f)*voxel;
                float acc=0.0f, w=0.0f;
                for (int c=0;c<num_maps;c++){
                    const qtsq_tsdf_camera_t *cam=&cams[c];
                    float vx=px-cam->pos[0], vy=py-cam->pos[1], vz=pz-cam->pos[2];
                    float d = vx*cam->forward[0]+vy*cam->forward[1]+
                              vz*cam->forward[2];
                    if (!(d>0.0f)) continue;              /* behind the camera */
                    float ndx = (vx*cam->right[0]+vy*cam->right[1]+
                                 vz*cam->right[2])/d;
                    float ndy = (vx*cam->up[0]+vy*cam->up[1]+
                                 vz*cam->up[2])/d;
                    float pxf = cam->fx*ndx+cam->cx;
                    float pyf = cam->fy*ndy+cam->cy;
                    if (pxf<0.0f || pyf<0.0f) continue;
                    int ix=(int)floorf(pxf), iy=(int)floorf(pyf);
                    if (ix>=depth_w || iy>=depth_h) continue;  /* outside image */
                    float m = depth_maps[c][(size_t)iy*depth_w+ix];
                    if (!(m>0.0f)) continue;              /* no hit on this ray */
                    float diff = d-m;
                    if (diff>trunc || diff<-trunc) continue;  /* outside the band */
                    acc += diff; w += 1.0f;
                }
                if (w>0.0f){
                    tsdf_voxel_t *v=&f[tsdf_voxel_index(n,xi,yi,zi)];
                    v->sdf = acc/w;
                    v->weight = w;
                }
            }
        }
    }
}

static void tsdf_finalize(tsdf_voxel_t *f, int n, float trunc){
    size_t nvox=(size_t)n*n*n;
    for (size_t i=0;i<nvox;i++){
        if (f[i].weight>0.0f){
            if (f[i].sdf> trunc) f[i].sdf= trunc;
            if (f[i].sdf<-trunc) f[i].sdf=-trunc;
        }
    }
}

/* ══════════════════════════════════════════════════════════════
 *  Marching cubes — generated per cell, no lookup tables
 * ══════════════════════════════════════════════════════════════ */

/* Sample the averaged field at fractional grid coords (x,y,z) by trilinear
 * interpolation; corners with zero weight (never measured) contribute
 * nothing. Returns 0 if nothing measurable is in range. */
static int tsdf_field_sample(const tsdf_voxel_t *f, int n,
                             float x, float y, float z, float *out){
    int nn = n-1;
    if (x<0.0f || y<0.0f || z<0.0f || x>(float)nn ||
        y>(float)nn || z>(float)nn) return 0;
    int x0=(int)floorf(x), y0=(int)floorf(y), z0=(int)floorf(z);
    int x1 = x0<nn ? x0+1 : nn;   /* clamp the far corner at the top edge */
    int y1 = y0<nn ? y0+1 : nn;
    int z1 = z0<nn ? z0+1 : nn;
    float fx=x-x0, fy=y-y0, fz=z-z0;
    float s=0.0f, wsum=0.0f;
    for (int dz=0;dz<2;dz++){
        float cz = dz ? fz : 1.0f-fz;
        int zc = dz ? z1 : z0;
        for (int dy=0;dy<2;dy++){
            float cy = dy ? fy : 1.0f-fy;
            int yc = dy ? y1 : y0;
            for (int dx=0;dx<2;dx++){
                float c = (dx ? fx : 1.0f-fx)*cy*cz;
                int xc = dx ? x1 : x0;
                const tsdf_voxel_t *v=&f[tsdf_voxel_index(n,xc,yc,zc)];
                if (v->weight>0.0f){ s += c*v->sdf; wsum += c; }
            }
        }
    }
    if (wsum<=0.0f) return 0;
    *out = s/wsum;
    return 1;
}

/* Central-difference gradient of the field at world position p, normalized.
 * Returns 0 if any sample is unavailable or the gradient is degenerate. */
static int tsdf_gradient(const tsdf_voxel_t *f, int n, float voxel,
                         const float gmin[3], const float p[3], float g[3]){
    float gx=(p[0]-gmin[0])/voxel-0.5f;
    float gy=(p[1]-gmin[1])/voxel-0.5f;
    float gz=(p[2]-gmin[2])/voxel-0.5f;
    float sp[3], sm[3];
    if (!tsdf_field_sample(f,n,gx+1.0f,gy,  gz,  &sp[0])) return 0;
    if (!tsdf_field_sample(f,n,gx-1.0f,gy,  gz,  &sm[0])) return 0;
    if (!tsdf_field_sample(f,n,gx,  gy+1.0f,gz,  &sp[1])) return 0;
    if (!tsdf_field_sample(f,n,gx,  gy-1.0f,gz,  &sm[1])) return 0;
    if (!tsdf_field_sample(f,n,gx,  gy,  gz+1.0f,&sp[2])) return 0;
    if (!tsdf_field_sample(f,n,gx,  gy,  gz-1.0f,&sm[2])) return 0;
    g[0]=(sp[0]-sm[0])/(2.0f*voxel);
    g[1]=(sp[1]-sm[1])/(2.0f*voxel);
    g[2]=(sp[2]-sm[2])/(2.0f*voxel);
    float len=sqrtf(g[0]*g[0]+g[1]*g[1]+g[2]*g[2]);
    if (!(len>1e-9f)) return 0;
    g[0]/=len; g[1]/=len; g[2]/=len;
    return 1;
}

/* world position of the zero crossing on cube edge e */
static void tsdf_edge_cross(const float pos[8][3], const float sdf[8],
                            int e, float out[3]){
    int a=TSDF_EDGE_CORNERS[e][0], b=TSDF_EDGE_CORNERS[e][1];
    float t = 0.5f;
    if (sdf[b]!=sdf[a]) t=(0.0f-sdf[a])/(sdf[b]-sdf[a]);
    if (t<0.0f) t=0.0f; else if (t>1.0f) t=1.0f;
    for (int k=0;k<3;k++) out[k]=pos[a][k]+t*(pos[b][k]-pos[a][k]);
}

typedef struct { uint8_t p, q; } tsdf_segment_t;  /* endpoints = cube edges */

/* 2D cut of one cube face: the face's crossing points (cube edge indices
 * whose corners differ in sign — always 0, 2 or 4 of them) joined into 0, 1
 * or 2 segments. The ambiguous alternating pattern is resolved with the
 * bilinear asymptotic decider: if the saddle value lies on the inside
 * (positive), the inside region connects through the face centre. */
static int tsdf_face_segments(const float sdf[8], int face,
                              tsdf_segment_t seg[2]){
    const uint8_t *fc = TSDF_FACE_CORNERS[face];
    uint8_t pts[4];
    int npts=0;
    for (int i=0;i<4;i++){
        int a=fc[i], b=fc[(i+1)%4];
        if ((sdf[a]>0.0f)!=(sdf[b]>0.0f))
            pts[npts++] = TSDF_EDGE_OF[a][b];
    }
    if (npts==2){ seg[0].p=pts[0]; seg[0].q=pts[1]; return 1; }
    if (npts==4){
        float v0=sdf[fc[0]], v1=sdf[fc[1]], v2=sdf[fc[2]], v3=sdf[fc[3]];
        float denom = v0-v1+v2-v3;
        int inside0 = v0>0.0f;                     /* fc0/fc2 are the inside pair */
        int central = 0;                           /* symmetric saddle: isolate */
        if (denom!=0.0f && (v0*v2-v1*v3)/denom>0.0f) central=1;
        if (central==inside0){
            seg[0].p=pts[0]; seg[0].q=pts[1];
            seg[1].p=pts[2]; seg[1].q=pts[3];
        } else {
            seg[0].p=pts[3]; seg[0].q=pts[0];
            seg[1].p=pts[1]; seg[1].q=pts[2];
        }
        return 2;
    }
    return 0;
}

/* ── mesh builder: two growable arrays, doubling capacity ── */
typedef struct {
    float *verts, *norms;
    uint32_t nv, vcap;
    uint32_t *tris;
    uint32_t nt, tcap;
} tsdf_builder_t;

static int tsdf_builder_grow_verts(tsdf_builder_t *b){
    if (b->nv<b->vcap) return 0;
    uint32_t nc = b->vcap ? b->vcap*2u : 4096u;
    if (nc<b->vcap) nc = 0x40000000u;
    float *vp=(float*)realloc(b->verts,(size_t)nc*3*sizeof(float));
    if (!vp) return -1;
    b->verts = vp;
    float *np=(float*)realloc(b->norms,(size_t)nc*3*sizeof(float));
    if (!np) return -1;
    b->norms = np;
    b->vcap = nc;
    return 0;
}

/* returns the new vertex id, or -1 on allocation failure */
static int tsdf_builder_add_vert(tsdf_builder_t *b, const float p[3],
                                 const float norm[3]){
    if (tsdf_builder_grow_verts(b)!=0) return -1;
    float *v=b->verts+(size_t)b->nv*3, *n=b->norms+(size_t)b->nv*3;
    v[0]=p[0]; v[1]=p[1]; v[2]=p[2];
    n[0]=norm[0]; n[1]=norm[1]; n[2]=norm[2];
    return (int)(b->nv++);
}

static int tsdf_builder_add_tri(tsdf_builder_t *b,
                                uint32_t a, uint32_t c, uint32_t d){
    if (b->nt>=b->tcap){
        uint32_t nc = b->tcap ? b->tcap*2u : 4096u;
        if (nc<b->tcap) nc = 0x40000000u;
        uint32_t *tp=(uint32_t*)realloc(b->tris,(size_t)nc*3*sizeof(uint32_t));
        if (!tp) return -1;
        b->tris = tp;
        b->tcap = nc;
    }
    uint32_t *t=b->tris+(size_t)b->nt*3;
    t[0]=a; t[1]=c; t[2]=d;
    b->nt++;
    return 0;
}

static void tsdf_builder_free(tsdf_builder_t *b){
    free(b->verts); free(b->norms); free(b->tris);
    memset(b,0,sizeof *b);
}

/* Extract one voxel cell. Skips cells with any unmeasured corner (weight 0)
 * so the mesh never bleeds into empty regions, and uniform cells. Returns 0
 * (possibly zero triangles), -1 on allocation or topology error. */
static int tsdf_cell_mesh(const tsdf_voxel_t *f, int n,
                          int xi, int yi, int zi,
                          const float gmin[3], float voxel,
                          tsdf_builder_t *b){
    float sdf[8], pos[8][3];
    int inside[8], cntin=0;
    for (int c=0;c<8;c++){
        int cx=xi+TSDF_CORNER_DXYZ[c][0];
        int cy=yi+TSDF_CORNER_DXYZ[c][1];
        int cz=zi+TSDF_CORNER_DXYZ[c][2];
        const tsdf_voxel_t *v=&f[tsdf_voxel_index(n,cx,cy,cz)];
        if (v->weight==0.0f) return 0;             /* empty corner: no mesh */
        sdf[c]=v->sdf;
        inside[c] = sdf[c]>0.0f;
        cntin += inside[c];
        pos[c][0]=gmin[0]+((float)cx+0.5f)*voxel;
        pos[c][1]=gmin[1]+((float)cy+0.5f)*voxel;
        pos[c][2]=gmin[2]+((float)cz+0.5f)*voxel;
    }
    if (cntin==0 || cntin==8) return 0;            /* no zero crossing */

    /* cut segments across the six faces */
    tsdf_segment_t seg[12];
    int nseg=0;
    for (int face=0;face<6;face++){
        tsdf_segment_t tmp[2];
        int k=tsdf_face_segments(sdf,face,tmp);
        for (int i=0;i<k;i++) seg[nseg+i]=tmp[i];
        nseg += k;
    }
    if (nseg==0) return 0;                         /* impossible, but cheap */

    /* stitch the segments into closed loops: every crossing point sits on
     * exactly two segments (one per adjacent face), so the walk is forced */
    uint8_t seg_used[12];
    memset(seg_used,0,sizeof seg_used);
    for (int s0=0;s0<nseg;s0++){
        if (seg_used[s0]) continue;
        uint8_t loop[13];
        int nloop=0;
        uint8_t start=seg[s0].p, cur=seg[s0].q;
        seg_used[s0]=1;
        loop[nloop++]=start;
        while (cur!=start){
            if (nloop>12) return -1;               /* topology error */
            loop[nloop++] = cur;
            int next=-1;
            for (int i=0;i<nseg;i++){
                if (!seg_used[i] && (seg[i].p==cur || seg[i].q==cur)){
                    next=i; break;
                }
            }
            if (next<0) return -1;                 /* broken loop */
            seg_used[next]=1;
            cur = (seg[next].p==cur) ? seg[next].q : seg[next].p;
        }
        int nlooplen=nloop;
        if (nlooplen<3) return -1;                 /* degenerate loop */

        /* fan the loop from its first crossing point into triangles */
        uint32_t evid[12];
        for (int e=0;e<12;e++) evid[e]=UINT32_MAX;
        for (int i=1;i<=nlooplen-2;i++){
            int es[3] = { loop[0], loop[i], loop[i+1] };
            uint32_t ids[3];
            for (int j=0;j<3;j++){
                int e=es[j];
                if (evid[e]==UINT32_MAX){
                    float p[3], g[3];
                    float norm[3]={0.0f,0.0f,0.0f};
                    tsdf_edge_cross(pos,sdf,e,p);
                    /* outward normal = negated SDF gradient; a degenerate
                     * gradient stays zero and is fixed up from the triangle
                     * normal after extraction */
                    if (tsdf_gradient(f,n,voxel,gmin,p,g)){
                        norm[0]=-g[0]; norm[1]=-g[1]; norm[2]=-g[2];
                    }
                    int id=tsdf_builder_add_vert(b,p,norm);
                    if (id<0) return -1;
                    evid[e]=(uint32_t)id;
                }
                ids[j]=evid[e];
            }
            if (tsdf_builder_add_tri(b,ids[0],ids[1],ids[2])!=0) return -1;
        }
    }
    return 0;
}

/* ══════════════════════════════════════════════════════════════
 *  Reconstruction entry point
 * ══════════════════════════════════════════════════════════════ */

int qtsq_tsdf_reconstruct(const float *const *depth_maps,
                          const qtsq_tsdf_camera_t *cams, int num_maps,
                          int depth_w, int depth_h,
                          int grid_n, float voxel, float trunc,
                          qtsq_mesh_t *out){
    if (!out) return -1;
    memset(out,0,sizeof *out);
    if (!depth_maps || !cams || num_maps<=0 || depth_w<=0 || depth_h<=0)
        return -1;
    for (int c=0;c<num_maps;c++) if (!depth_maps[c]) return -1;
    if (grid_n<=0) grid_n=96;
    if (grid_n<2 || grid_n>128) return -1;         /* 128^3 = 2 M voxels */
    if (!(voxel>0.0f)) voxel=0.01f;
    if (!(trunc>0.0f)) trunc=4.0f*voxel;

    /* grid centred on the union of camera frustum slices (0.3 m..5 m along
     * forward), sized grid_n*voxel */
    float center[3], half[3], gmin[3];
    qtsq_tsdf_grid_bounds(cams,num_maps,0.3f,5.0f,center,half);
    float span=(float)grid_n*voxel;
    for (int k=0;k<3;k++) gmin[k]=center[k]-0.5f*span;

    tsdf_voxel_t *f=tsdf_field_alloc(grid_n);
    if (!f) return -1;
    tsdf_fuse(f,grid_n,voxel,gmin,depth_maps,cams,num_maps,
              depth_w,depth_h,trunc);
    tsdf_finalize(f,grid_n,trunc);

    tsdf_builder_t b;
    memset(&b,0,sizeof b);
    int rc=0;
    for (int zi=0;zi<grid_n-1 && rc==0;zi++)
        for (int yi=0;yi<grid_n-1 && rc==0;yi++)
            for (int xi=0;xi<grid_n-1;xi++)
                if (tsdf_cell_mesh(f,grid_n,xi,yi,zi,gmin,voxel,&b)!=0){
                    rc=-1; break;
                }

    if (rc==0){
        /* vertices whose SDF-gradient normal was degenerate fall back to the
         * average of their (non-degenerate) surrounding triangle normals */
        for (uint32_t t=0;t<b.nt;t++){
            uint32_t i0=b.tris[t*3], i1=b.tris[t*3+1], i2=b.tris[t*3+2];
            const float *v0=b.verts+(size_t)i0*3;
            const float *v1=b.verts+(size_t)i1*3;
            const float *v2=b.verts+(size_t)i2*3;
            float e1[3]={v1[0]-v0[0],v1[1]-v0[1],v1[2]-v0[2]};
            float e2[3]={v2[0]-v0[0],v2[1]-v0[1],v2[2]-v0[2]};
            float nf[3]={ e1[1]*e2[2]-e1[2]*e2[1],
                          e1[2]*e2[0]-e1[0]*e2[2],
                          e1[0]*e2[1]-e1[1]*e2[0] };
            float len=sqrtf(nf[0]*nf[0]+nf[1]*nf[1]+nf[2]*nf[2]);
            if (!(len>1e-12f)) continue;           /* degenerate triangle */
            nf[0]/=len; nf[1]/=len; nf[2]/=len;
            for (int j=0;j<3;j++){
                float *nn=b.norms+(size_t)b.tris[t*3+j]*3;
                if (nn[0]==0.0f && nn[1]==0.0f && nn[2]==0.0f){
                    nn[0]=nf[0]; nn[1]=nf[1]; nn[2]=nf[2];
                }
            }
        }

        if (b.nt>0){
            out->vertices = b.verts;
            out->nv = b.nv;
            out->normals = b.norms;
            out->tris = b.tris;
            out->nt = b.nt;
        }
        memset(&b,0,sizeof b);
    }
    tsdf_builder_free(&b);
    free(f);
    return rc;
}

void qtsq_mesh_free(qtsq_mesh_t *m){
    if (!m) return;
    free(m->vertices);
    free(m->tris);
    free(m->normals);
    memset(m,0,sizeof *m);
}

/* ══════════════════════════════════════════════════════════════
 *  ASCII PLY writer
 * ══════════════════════════════════════════════════════════════ */

int qtsq_mesh_write_ply(const char *path, const qtsq_mesh_t *m){
    if (!path || !m) return -1;
    FILE *fp = fopen(path,"w");
    if (!fp) return -1;
    fprintf(fp,"ply\nformat ascii 1.0\ncomment QTSQ TSDF mesh\n");
    fprintf(fp,"element vertex %u\n",(unsigned)m->nv);
    fprintf(fp,"property float x\nproperty float y\nproperty float z\n");
    fprintf(fp,"property float nx\nproperty float ny\nproperty float nz\n");
    fprintf(fp,"element face %u\n",(unsigned)m->nt);
    fprintf(fp,"property list uchar int vertex_indices\nend_header\n");
    for (uint32_t i=0;i<m->nv;i++){
        const float *v=m->vertices+(size_t)i*3;
        const float *n = m->normals ? m->normals+(size_t)i*3 : v;
        fprintf(fp,"%g %g %g %g %g %g\n",v[0],v[1],v[2],n[0],n[1],n[2]);
    }
    for (uint32_t t=0;t<m->nt;t++)
        fprintf(fp,"3 %u %u %u\n",(unsigned)m->tris[t*3],
                (unsigned)m->tris[t*3+1],(unsigned)m->tris[t*3+2]);
    int rc = ferror(fp) ? -1 : 0;
    if (fclose(fp)!=0) rc = -1;
    return rc;
}

/* ══════════════════════════════════════════════════════════════
 *  Binary mesh stream — .qtsq container payload
 * ══════════════════════════════════════════════════════════════ */

int qtsq_mesh_pack(const qtsq_mesh_t *m, uint8_t **out_data,
                   size_t *out_size){
    if (!m || !out_data || !out_size || !m->vertices ||
        m->nv==0 || !m->tris || m->nt==0 || !m->normals) return -1;
    size_t nv3=(size_t)m->nv*3, nt3=(size_t)m->nt*3;
    size_t vbytes=nv3*sizeof(float);                     /* verts + normals */
    size_t tbytes=nt3*sizeof(uint32_t);
    if (nv3/3!=(size_t)m->nv || vbytes/sizeof(float)!=nv3 ||
        nt3/3!=(size_t)m->nt || tbytes/sizeof(uint32_t)!=nt3)
        return -1;                                       /* size_t overflow */
    size_t total=12+vbytes;                              /* 4 magic + 2 u32 */
    if (total<vbytes) return -1;
    total+=tbytes;
    if (total<tbytes) return -1;
    total+=vbytes;
    if (total<vbytes) return -1;
    uint8_t *buf=(uint8_t*)malloc(total);
    if (!buf) return -1;
    buf[0]='M'; buf[1]='E'; buf[2]='S'; buf[3]='H';
    memcpy(buf+4, &m->nv, 4);
    memcpy(buf+8, m->vertices, vbytes);
    memcpy(buf+8+vbytes, &m->nt, 4);
    memcpy(buf+12+vbytes, m->tris, tbytes);
    memcpy(buf+12+vbytes+tbytes, m->normals, vbytes);
    *out_data = buf; *out_size = total;
    return 0;
}

int qtsq_mesh_unpack(const uint8_t *data, size_t size, qtsq_mesh_t *out){
    if (!data || !out || size<12) return -1;
    if (data[0]!='M' || data[1]!='E' || data[2]!='S' || data[3]!='H')
        return -1;                                       /* bad magic */
    uint32_t nv;
    memcpy(&nv, data+4, 4);
    if (nv==0) return -1;
    size_t nv3=(size_t)nv*3, vbytes=nv3*sizeof(float);
    if (nv3/3!=(size_t)nv || vbytes/sizeof(float)!=nv3) return -1;
    size_t nt_off=8+vbytes;                              /* u32 nt field */
    if (nt_off<vbytes) return -1;                        /* size_t overflow */
    if (size<nt_off+4) return -1;                        /* truncated */
    uint32_t nt;
    memcpy(&nt, data+nt_off, 4);
    if (nt==0) return -1;
    size_t nt3=(size_t)nt*3, tbytes=nt3*sizeof(uint32_t);
    if (nt3/3!=(size_t)nt || tbytes/sizeof(uint32_t)!=nt3) return -1;
    size_t total=nt_off+4;
    if (total<nt_off) return -1;                         /* size_t overflow */
    total+=tbytes;
    if (total<tbytes) return -1;
    total+=vbytes;
    if (total<vbytes) return -1;
    if (size<total) return -1;                           /* truncated */

    qtsq_mesh_t r;
    memset(&r,0,sizeof r);
    r.vertices=(float*)malloc(vbytes);
    if (!r.vertices) return -1;
    r.tris=(uint32_t*)malloc(tbytes);
    if (!r.tris){ free(r.vertices); return -1; }
    r.normals=(float*)malloc(vbytes);
    if (!r.normals){
        free(r.vertices); free(r.tris); return -1;
    }
    memcpy(r.vertices, data+8, vbytes);
    memcpy(r.tris, data+nt_off+4, tbytes);
    memcpy(r.normals, data+nt_off+4+tbytes, vbytes);
    r.nv = nv; r.nt = nt;
    *out = r;
    return 0;
}
