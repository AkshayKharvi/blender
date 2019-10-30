/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * Copyright 2019, Blender Foundation.
 *
 */

/** \file
 * \ingroup draw
 */

#include "DRW_engine.h"
#include "DRW_render.h"
#include "BLI_listbase.h"
#include "BLI_linklist.h"
#include "lanpr_all.h"
#include "DRW_render.h"
#include "BKE_object.h"
#include "DNA_mesh_types.h"
#include "DNA_camera_types.h"
#include "GPU_immediate.h"
#include "GPU_immediate_util.h"
#include "GPU_framebuffer.h"
#include "DNA_lanpr_types.h"
#include "DEG_depsgraph_query.h"
#include "GPU_draw.h"
#include "GPU_batch.h"
#include "GPU_framebuffer.h"
#include "GPU_shader.h"
#include "GPU_uniformbuffer.h"
#include "GPU_viewport.h"
#include "bmesh.h"

extern struct LANPR_SharedResource lanpr_share;

int _TNS_colOffsets[] = {-1, 0, 1, 1, 1, 0, -1, -1};
int _TNS_rowOffsets[] = {-1, -1, -1, 0, 1, 1, 1, 0};

int _TNS_Deviates[8][8] = {{0, 1, 2, 3, 4, 3, 2, 1},
                           {1, 0, 1, 2, 3, 4, 3, 2},
                           {2, 1, 0, 1, 2, 3, 4, 3},
                           {3, 2, 1, 0, 1, 2, 3, 4},
                           {4, 3, 2, 1, 0, 1, 2, 3},
                           {3, 4, 3, 2, 1, 0, 1, 2},
                           {2, 3, 4, 3, 2, 1, 0, 1},
                           {1, 2, 3, 4, 3, 2, 1, 0}};

#define TNS_CLAMP_TEXTURE_W(t, col) \
  { \
    if (col >= t->width) \
      col = t->width - 1; \
    if (col < 0) \
      col = 0; \
  }

#define TNS_CLAMP_TEXTURE_H(t, row) \
  { \
    if (row >= t->height) \
      row = t->height - 1; \
    if (row < 0) \
      row = 0; \
  }

#define TNS_CLAMP_TEXTURE_CONTINUE(t, col, row) \
  { \
    if (col >= t->width) \
      continue; \
    if (col < 0) \
      continue; \
    if (row >= t->height) \
      continue; \
    if (row < 0) \
      continue; \
  }

static LANPR_TextureSample *lanpr_any_uncovered_samples(LANPR_PrivateData *pd)
{
  return BLI_pophead(&pd->pending_samples);
}

int lanpr_direction_deviate(int From, int To)
{
  return _TNS_Deviates[From - 1][To - 1];
}

int lanpr_detect_direction(LANPR_PrivateData *pd, int col, int row, int LastDirection)
{
  int Deviate[9] = {100};
  int MinDeviate = 0;
  int i;
  LANPR_TextureSample *ts;

  for (i = 0; i < 8; i++) {
    TNS_CLAMP_TEXTURE_CONTINUE(pd, (_TNS_colOffsets[i] + col), (_TNS_rowOffsets[i] + row));
    if (ts = pd->sample_table[(_TNS_colOffsets[i] + col) +
                              (_TNS_rowOffsets[i] + row) * pd->width]) {
      if (!LastDirection) {
        return i + 1;
      }
      Deviate[i + 1] = lanpr_direction_deviate(i, LastDirection);
      if (!MinDeviate || Deviate[MinDeviate] > Deviate[i + 1]) {
        MinDeviate = i + 1;
      }
    }
  }

  return MinDeviate;
}

LANPR_LineStrip *lanpr_create_line_strip(LANPR_PrivateData *pd)
{
  LANPR_LineStrip *ls = BLI_mempool_calloc(lanpr_share.mp_line_strip);
  return ls;
}
LANPR_LineStripPoint *lanpr_append_point(
    LANPR_PrivateData *pd, LANPR_LineStrip *ls, real X, real Y, real Z)
{
  LANPR_LineStripPoint *lsp = BLI_mempool_calloc(lanpr_share.mp_line_strip_point);

  lsp->P[0] = X;
  lsp->P[1] = Y;
  lsp->P[2] = Z;

  BLI_addtail(&ls->points, lsp);

  ls->point_count++;

  return lsp;
}
LANPR_LineStripPoint *lanpr_push_point(
    LANPR_PrivateData *pd, LANPR_LineStrip *ls, real X, real Y, real Z)
{
  LANPR_LineStripPoint *lsp = BLI_mempool_calloc(lanpr_share.mp_line_strip_point);

  lsp->P[0] = X;
  lsp->P[1] = Y;
  lsp->P[2] = Z;

  BLI_addhead(&ls->points, lsp);

  ls->point_count++;

  return lsp;
}

void lanpr_destroy_line_strip(LANPR_PrivateData *pd, LANPR_LineStrip *ls)
{
  LANPR_LineStripPoint *lsp;
  while (lsp = BLI_pophead(&ls->points)) {
    BLI_mempool_free(lanpr_share.mp_line_strip_point, lsp);
  }
  BLI_mempool_free(lanpr_share.mp_line_strip, ls);
}

void lanpr_remove_sample(LANPR_PrivateData *pd, int row, int col)
{
  LANPR_TextureSample *ts;
  ts = pd->sample_table[row * pd->width + col];
  pd->sample_table[row * pd->width + col] = NULL;

  BLI_remlink(&pd->pending_samples, ts);
  ts->prev = NULL;
  ts->next = NULL;
  BLI_addtail(&pd->erased_samples, ts);
}

void lanpr_grow_snake_r(LANPR_PrivateData *pd,
                        LANPR_LineStrip *ls,
                        LANPR_LineStripPoint *ThisP,
                        int Direction)
{
  LANPR_LineStripPoint *NewP = ThisP, *p2;
  int Length = 5;
  int l = 0;
  int Deviate, Dir = Direction, NewDir;
  int AddPoint;
  int TX = NewP->P[0], TY = NewP->P[1];

  while (NewDir = lanpr_detect_direction(pd, TX, TY, Dir)) {
    AddPoint = 0;
    Deviate = lanpr_direction_deviate(NewDir, Dir);
    Dir = NewDir;

    l++;
    TX += _TNS_colOffsets[NewDir - 1];
    TY += _TNS_rowOffsets[NewDir - 1];

    if (Deviate < 2) {
      lanpr_remove_sample(pd, TY, TX);
    }
    else if (Deviate < 3) {
      lanpr_remove_sample(pd, TY, TX);
      AddPoint = 1;
    }
    else {
      lanpr_remove_sample(pd, TY, TX);
      return;
    }

    if (AddPoint || l == Length) {
      p2 = lanpr_append_point(pd, ls, TX, TY, 0);
      NewP = p2;
      l = 0;
    }
  }
  if (TX != ThisP->P[0] || TY != ThisP->P[1]) {
    lanpr_append_point(pd, ls, TX, TY, 0);
  }
}

void lanpr_grow_snake_l(LANPR_PrivateData *pd,
                        LANPR_LineStrip *ls,
                        LANPR_LineStripPoint *ThisP,
                        int Direction)
{
  LANPR_LineStripPoint *NewP = ThisP, *p2;
  int Length = 5;
  int l = 0;
  int Deviate, Dir = Direction, NewDir;
  int AddPoint;
  int TX = NewP->P[0], TY = NewP->P[1];

  while (NewDir = lanpr_detect_direction(pd, TX, TY, Dir)) {
    AddPoint = 0;
    Deviate = lanpr_direction_deviate(NewDir, Dir);
    Dir = NewDir;

    l++;
    TX += _TNS_colOffsets[NewDir - 1];
    TY += _TNS_rowOffsets[NewDir - 1];

    if (Deviate < 2) {
      lanpr_remove_sample(pd, TY, TX);
    }
    else if (Deviate < 4) {
      lanpr_remove_sample(pd, TY, TX);
      AddPoint = 1;
    }
    else {
      lanpr_remove_sample(pd, TY, TX);
      return;
    }

    if (AddPoint || l == Length) {
      p2 = lanpr_push_point(pd, ls, TX, TY, 0);
      NewP = p2;
      l = 0;
    }
  }
  if (TX != ThisP->P[0] || TY != ThisP->P[1]) {
    lanpr_push_point(pd, ls, TX, TY, 0);
  }
}

int lanpr_reverse_direction(int From)
{
  From -= 4;
  if (From <= 0) {
    From += 8;
  }
  return From;
}

void lanpr_texture_to_ndc(int x, int y, int w, int h, float *x_ndc, float *y_ndc)
{
  *x_ndc = interpf(1, -1, (float)x / (float)w);
  *y_ndc = interpf(1, -1, (float)y / (float)h);
}

void lanpr_count_drawing_elements(LANPR_PrivateData *pd,
                                  int *vert_count,
                                  int *index_adjacent_count)
{
  int v_count = 0;
  int e_count = 0;
  LANPR_LineStrip *ls;
  for (ls = (LANPR_LineStrip *)(pd->line_strips.first); ls; ls = (ls->next)) {
    v_count += (ls->point_count);
    e_count += ((ls->point_count - 1) * 4);
  }
  *vert_count = v_count;
  *index_adjacent_count = e_count;
}

GPUBatch *lanpr_get_snake_batch(LANPR_PrivateData *pd)
{
  LANPR_LineStrip *ls;
  LANPR_LineStripPoint *lsp, *plsp;
  int i;
  float *Verts;
  float *Lengths;
  float TotalLength = 0;
  int v_count, e_count;

  lanpr_count_drawing_elements(pd, &v_count, &e_count);

  Verts = MEM_callocN(sizeof(float) * v_count * 2, "Verts buffer pre alloc");
  Lengths = MEM_callocN(sizeof(float) * v_count * 2, "Length buffer pre alloc");

  GPUIndexBufBuilder elb;
  GPU_indexbuf_init_ex(&elb, GPU_PRIM_LINES_ADJ, e_count, v_count);

  int vert_offset = 0;

  for (ls = (LANPR_LineStrip *)(pd->line_strips.first); ls; ls = (ls->next)) {
    for (i = 0; i < ls->point_count - 1; i++) {
      int v1 = i + vert_offset - 1;
      int v2 = i + vert_offset;
      int v3 = i + vert_offset + 1;
      int v4 = i + vert_offset + 2;
      if (v1 < 0) {
        v1 = 0;
      }
      if (v4 >= v_count) {
        v4 = v_count - 1;
      }
      GPU_indexbuf_add_line_adj_verts(&elb, v1, v2, v3, v4);
    }

    i = 0;
    float xf, yf;
    TotalLength = 0;
    for (lsp = (LANPR_LineStripPoint *)(ls->points.first); lsp; lsp = (lsp->next)) {
      lanpr_texture_to_ndc(lsp->P[0], lsp->P[1], pd->width, pd->height, &xf, &yf);
      Verts[vert_offset * 2 + i * 2 + 0] = xf;
      Verts[vert_offset * 2 + i * 2 + 1] = yf;
      if (plsp = (LANPR_LineStripPoint *)(lsp->prev)) {
        TotalLength += tMatDist2v(plsp->P, lsp->P);
        Lengths[(vert_offset + i) * 2] = TotalLength;
      }
      i++;
    }

    ls->total_length = TotalLength;
    i = 0;
    for (lsp = (LANPR_LineStripPoint *)(ls->points.first); lsp; lsp = (lsp->next)) {
      if (plsp = (LANPR_LineStripPoint *)(lsp->prev)) {
        Lengths[(vert_offset + i) * 2 + 1] = ls->total_length - Lengths[(vert_offset + i) * 2];
      }
      i++;
    }

    vert_offset += (ls->point_count);
  }

  static GPUVertFormat format = {0};
  static struct {
    uint pos, uvs;
  } attr_id;
  if (format.attr_len == 0) {
    attr_id.pos = GPU_vertformat_attr_add(&format, "pos", GPU_COMP_F32, 2, GPU_FETCH_FLOAT);
    attr_id.uvs = GPU_vertformat_attr_add(&format, "uvs", GPU_COMP_F32, 2, GPU_FETCH_FLOAT);
  }

  GPUVertBuf *vbo = GPU_vertbuf_create_with_format(&format);
  GPU_vertbuf_data_alloc(vbo, v_count);

  for (int i = 0; i < v_count; ++i) {
    GPU_vertbuf_attr_set(vbo, attr_id.pos, i, &Verts[i * 2]);
    GPU_vertbuf_attr_set(vbo, attr_id.uvs, i, &Lengths[i * 2]);
  }

  MEM_freeN(Verts);
  MEM_freeN(Lengths);

  return GPU_batch_create_ex(
      GPU_PRIM_LINES_ADJ, vbo, GPU_indexbuf_build(&elb), GPU_USAGE_STATIC | GPU_BATCH_OWNS_VBO);
}

void lanpr_snake_prepare_cache(LANPR_PrivateData *pd)
{
  if (pd->line_result_8bit) {
    MEM_freeN(pd->line_result_8bit);
  }
  pd->line_result_8bit = 0;
  if (pd->line_result) {
    MEM_freeN(pd->line_result);
  }
  pd->line_result = 0;
  lanpr_share.mp_sample = BLI_mempool_create(sizeof(LANPR_TextureSample), 0, 512, BLI_MEMPOOL_NOP);
  lanpr_share.mp_line_strip = BLI_mempool_create(sizeof(LANPR_LineStrip), 0, 512, BLI_MEMPOOL_NOP);
  lanpr_share.mp_line_strip_point = BLI_mempool_create(
      sizeof(LANPR_LineStripPoint), 0, 1024, BLI_MEMPOOL_NOP);
}
void lanpe_sanke_free_cache(LANPR_PrivateData *pd)
{
  if (pd->line_result_8bit) {
    MEM_freeN(pd->line_result_8bit);
  }
  pd->line_result_8bit = 0;
  if (pd->line_result) {
    MEM_freeN(pd->line_result);
  }
  pd->line_result = 0;

  BLI_mempool_destroy(lanpr_share.mp_line_strip);
  BLI_mempool_destroy(lanpr_share.mp_line_strip_point);
  BLI_mempool_destroy(lanpr_share.mp_sample);
}
void lanpr_snake_free_readback_data(LANPR_PrivateData *pd)
{
  if (pd->line_result_8bit) {
    MEM_freeN(pd->line_result_8bit);
  }
  pd->line_result_8bit = 0;
  if (pd->line_result) {
    MEM_freeN(pd->line_result);
  }
  pd->line_result = 0;
}

void lanpr_snake_draw_scene(LANPR_TextureList *txl,
                            LANPR_FramebufferList *fbl,
                            LANPR_PassList *psl,
                            LANPR_PrivateData *pd,
                            SceneLANPR *lanpr,
                            GPUFrameBuffer *DefaultFB,
                            int is_render)
{
  eGPUFrameBufferBits clear_bits = GPU_COLOR_BIT | GPU_DEPTH_BIT;
  float clear_col[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  float clear_depth = 1.0f;
  uint clear_stencil = 0xFF;

  const DRWContextState *draw_ctx = DRW_context_state_get();
  Scene *scene = DEG_get_evaluated_scene(draw_ctx->depsgraph);
  View3D *v3d = draw_ctx->v3d;
  Object *camera;
  if (v3d) {
    RegionView3D *rv3d = draw_ctx->rv3d;
    camera = (rv3d->persp == RV3D_CAMOB) ? v3d->camera : NULL;
  }
  else {
    camera = scene->camera;
  }

  pd->znear = camera ? ((Camera *)camera->data)->clip_start : 0.1;
  pd->zfar = camera ? ((Camera *)camera->data)->clip_end : 100;
  pd->normal_clamp = lanpr->normal_clamp;
  pd->normal_strength = lanpr->normal_strength;
  pd->depth_clamp = lanpr->depth_clamp;
  pd->depth_strength = lanpr->depth_strength;

  GPU_framebuffer_bind(fbl->edge_intermediate);
  DRW_draw_pass(psl->edge_intermediate);

  if ((!lanpr->enable_vector_trace) && (!lanpr->display_thinning_result)) {
    GPU_framebuffer_bind(DefaultFB);
    DRW_multisamples_resolve(txl->depth, txl->edge_intermediate, 1);
    return;
  }

  if (lanpr->display_thinning_result || lanpr->enable_vector_trace) {
    pd->stage = 0;

    GPU_framebuffer_bind(DefaultFB);
    DRW_multisamples_resolve(txl->depth, txl->edge_intermediate, 1);

    GPU_framebuffer_bind(fbl->edge_thinning);
    DRW_draw_pass(psl->edge_thinning);
    GPU_framebuffer_bind(DefaultFB);
    DRW_multisamples_resolve(txl->depth, txl->color, 1);

    pd->stage = 1;
    GPU_framebuffer_bind(fbl->edge_thinning);
    DRW_draw_pass(psl->edge_thinning);
    GPU_framebuffer_bind(DefaultFB);
    DRW_multisamples_resolve(txl->depth, txl->color, 1);

    pd->stage = 0;
    GPU_framebuffer_bind(fbl->edge_thinning);
    DRW_draw_pass(psl->edge_thinning);
    GPU_framebuffer_bind(DefaultFB);
    DRW_multisamples_resolve(txl->depth, txl->color, 1);

    pd->stage = 1;
    GPU_framebuffer_bind(fbl->edge_thinning);
    DRW_draw_pass(psl->edge_thinning);
    GPU_framebuffer_bind(DefaultFB);
    DRW_multisamples_resolve(txl->depth, txl->color, 1);

    if (!lanpr->enable_vector_trace) {
      return;
    }
  }

  int texw = GPU_texture_width(txl->edge_intermediate),
      texh = GPU_texture_height(txl->edge_intermediate);
  ;
  int tsize = texw * texh;
  int recreate = 0;
  if (tsize != pd->width * pd->height) {
    recreate = 1;
  }

  if (recreate || !pd->line_result) {
    if (pd->line_result) {
      MEM_freeN(pd->line_result);
    }
    pd->line_result = MEM_callocN(sizeof(float) * tsize, "Texture readback buffer");

    if (pd->line_result_8bit) {
      MEM_freeN(pd->line_result_8bit);
    }
    pd->line_result_8bit = MEM_callocN(sizeof(unsigned char) * tsize,
                                       "Texture readback buffer 8bit");

    if (pd->sample_table) {
      MEM_freeN(pd->sample_table);
    }
    pd->sample_table = MEM_callocN(sizeof(void *) * tsize, "Texture readback buffer 8bit");

    pd->width = texw;
    pd->height = texh;
  }

  GPU_framebuffer_bind(DefaultFB);
  GPU_framebuffer_read_color(DefaultFB, 0, 0, texw, texh, 1, 0, pd->line_result);

  float sample;
  int h, w;
  for (h = 0; h < texh; h++) {
    for (w = 0; w < texw; w++) {
      int index = h * texw + w;
      if ((sample = pd->line_result[index]) > 0.9) {
        pd->line_result_8bit[index] = 255;
        LANPR_TextureSample *ts = BLI_mempool_calloc(lanpr_share.mp_sample);
        BLI_addtail(&pd->pending_samples, ts);
        pd->sample_table[index] = ts;
        ts->X = w;
        ts->Y = h;
      }
      else {
        pd->sample_table[index] = 0;
      }
    }
  }

  LANPR_TextureSample *ts;
  LANPR_LineStrip *ls;
  LANPR_LineStripPoint *lsp;
  while (ts = lanpr_any_uncovered_samples(pd)) {
    int Direction = 0;
    LANPR_LineStripPoint tlsp = {0};

    tlsp.P[0] = ts->X;
    tlsp.P[1] = ts->Y;

    if (Direction = lanpr_detect_direction(pd, ts->X, ts->Y, Direction)) {
      BLI_addtail(&pd->line_strips, (ls = lanpr_create_line_strip(pd)));
      lsp = lanpr_append_point(pd, ls, ts->X, ts->Y, 0);
      lanpr_remove_sample(pd, ts->Y, ts->X);

      lanpr_grow_snake_r(pd, ls, lsp, Direction);

      lanpr_grow_snake_l(pd, ls, lsp, lanpr_reverse_direction(Direction));
    }
  }

  float use_background_color[4] = {0.0f, 0.0f, 0.0f, 1.0f};

  copy_v3_v3(use_background_color, &scene->world->horr);
  use_background_color[3] = scene->r.alphamode ? 0.0f : 1.0f;

  GPU_framebuffer_bind(DefaultFB);
  GPU_framebuffer_clear(DefaultFB, clear_bits, use_background_color, clear_depth, clear_stencil);

  GPU_framebuffer_bind(fbl->edge_intermediate);
  clear_bits = GPU_COLOR_BIT;
  GPU_framebuffer_clear(
      fbl->edge_intermediate, clear_bits, use_background_color, clear_depth, clear_stencil);

  float *tld = &lanpr->taper_left_distance, *tls = &lanpr->taper_left_strength,
        *trd = &lanpr->taper_right_distance, *trs = &lanpr->taper_right_strength;

  GPUBatch *snake_batch = lanpr_get_snake_batch(pd);

  lanpr_snake_prepare_cache(pd);

  psl->snake_pass = DRW_pass_create("Snake Visualization Pass",
                                    DRW_STATE_WRITE_COLOR | DRW_STATE_WRITE_DEPTH |
                                        DRW_STATE_DEPTH_ALWAYS);
  pd->snake_shgrp = DRW_shgroup_create(lanpr_share.snake_connection_shader, psl->snake_pass);
  DRW_shgroup_uniform_float(pd->snake_shgrp, "line_width", &lanpr->line_thickness, 1);
  DRW_shgroup_uniform_float(pd->snake_shgrp, "taper_l_dist", tld, 1);
  DRW_shgroup_uniform_float(pd->snake_shgrp, "taper_r_dist", tls, 1);
  DRW_shgroup_uniform_float(
      pd->snake_shgrp, "taper_l_strength", (lanpr->flags & LANPR_SAME_TAPER) ? tld : trd, 1);
  DRW_shgroup_uniform_float(
      pd->snake_shgrp, "taper_r_strength", (lanpr->flags & LANPR_SAME_TAPER) ? tls : trs, 1);
  DRW_shgroup_uniform_vec4(pd->snake_shgrp, "line_color", lanpr->line_color, 1);

  DRW_shgroup_call(pd->snake_shgrp, snake_batch, NULL);
  GPU_framebuffer_bind(fbl->edge_intermediate);

  DRW_draw_pass(psl->snake_pass);
  GPU_BATCH_DISCARD_SAFE(snake_batch);

  BLI_mempool_clear(lanpr_share.mp_sample);
  BLI_mempool_clear(lanpr_share.mp_line_strip);
  BLI_mempool_clear(lanpr_share.mp_line_strip_point);

  pd->pending_samples.first = pd->pending_samples.last = 0;
  pd->erased_samples.first = pd->erased_samples.last = 0;
  pd->line_strips.first = pd->line_strips.last = 0;

  GPU_framebuffer_bind(DefaultFB);
  DRW_multisamples_resolve(txl->depth, txl->edge_intermediate, 1);

  lanpe_sanke_free_cache(pd);
}
