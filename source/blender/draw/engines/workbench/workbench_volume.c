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
 * Copyright 2018, Blender Foundation.
 */

/** \file
 * \ingroup draw_engine
 */

#include "workbench_private.h"

#include "DNA_fluid_types.h"
#include "DNA_modifier_types.h"
#include "DNA_object_force_types.h"
#include "DNA_volume_types.h"

#include "BLI_rand.h"
#include "BLI_dynstr.h"
#include "BLI_string_utils.h"

#include "BKE_fluid.h"
#include "BKE_global.h"
#include "BKE_object.h"
#include "BKE_volume.h"

#include "GPU_draw.h"

enum {
  VOLUME_SH_SLICE = 0,
  VOLUME_SH_COBA,
  VOLUME_SH_CUBIC,
  VOLUME_SH_SMOKE,
};

#define VOLUME_SH_MAX (1 << (VOLUME_SH_SMOKE + 1))

static struct {
  struct GPUShader *volume_sh[VOLUME_SH_MAX];
  struct GPUShader *volume_coba_sh;
  struct GPUTexture *dummy_tex;
  struct GPUTexture *dummy_shadow_tex;
  struct GPUTexture *dummy_coba_tex;
} e_data = {{NULL}};

extern char datatoc_workbench_volume_vert_glsl[];
extern char datatoc_workbench_volume_frag_glsl[];
extern char datatoc_common_view_lib_glsl[];
extern char datatoc_gpu_shader_common_obinfos_lib_glsl[];

static GPUShader *volume_shader_get(bool slice, bool coba, bool cubic, bool smoke)
{
  int id = 0;
  id += (slice) ? (1 << VOLUME_SH_SLICE) : 0;
  id += (coba) ? (1 << VOLUME_SH_COBA) : 0;
  id += (cubic) ? (1 << VOLUME_SH_CUBIC) : 0;
  id += (smoke) ? (1 << VOLUME_SH_SMOKE) : 0;

  if (!e_data.volume_sh[id]) {
    DynStr *ds = BLI_dynstr_new();

    if (slice) {
      BLI_dynstr_append(ds, "#define VOLUME_SLICE\n");
    }
    if (coba) {
      BLI_dynstr_append(ds, "#define USE_COBA\n");
    }
    if (cubic) {
      BLI_dynstr_append(ds, "#define USE_TRICUBIC\n");
    }
    if (smoke) {
      BLI_dynstr_append(ds, "#define VOLUME_SMOKE\n");
    }

    char *defines = BLI_dynstr_get_cstring(ds);
    BLI_dynstr_free(ds);

    char *libs = BLI_string_joinN(datatoc_common_view_lib_glsl,
                                  datatoc_gpu_shader_common_obinfos_lib_glsl);

    e_data.volume_sh[id] = DRW_shader_create_with_lib(datatoc_workbench_volume_vert_glsl,
                                                      NULL,
                                                      datatoc_workbench_volume_frag_glsl,
                                                      libs,
                                                      defines);

    MEM_freeN(libs);
    MEM_freeN(defines);
  }

  return e_data.volume_sh[id];
}

void workbench_volume_engine_init(void)
{
  if (!e_data.dummy_tex) {
    float zero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float one[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    e_data.dummy_tex = GPU_texture_create_3d(1, 1, 1, GPU_RGBA8, zero, NULL);
    e_data.dummy_shadow_tex = GPU_texture_create_3d(1, 1, 1, GPU_RGBA8, one, NULL);
    e_data.dummy_coba_tex = GPU_texture_create_1d(1, GPU_RGBA8, zero, NULL);
  }
}

void workbench_volume_engine_free(void)
{
  for (int i = 0; i < VOLUME_SH_MAX; i++) {
    DRW_SHADER_FREE_SAFE(e_data.volume_sh[i]);
  }
  DRW_TEXTURE_FREE_SAFE(e_data.dummy_tex);
  DRW_TEXTURE_FREE_SAFE(e_data.dummy_shadow_tex);
  DRW_TEXTURE_FREE_SAFE(e_data.dummy_coba_tex);
}

void workbench_volume_cache_init(WORKBENCH_Data *vedata)
{
  vedata->psl->volume_pass = DRW_pass_create(
      "Volumes", DRW_STATE_WRITE_COLOR | DRW_STATE_BLEND_ALPHA_PREMUL | DRW_STATE_CULL_FRONT);
}

static void workbench_volume_modifier_cache_populate(WORKBENCH_Data *vedata,
                                                     Object *ob,
                                                     ModifierData *md)
{
  FluidModifierData *mmd = (FluidModifierData *)md;
  FluidDomainSettings *mds = mmd->domain;
  WORKBENCH_PrivateData *wpd = vedata->stl->g_data;
  WORKBENCH_EffectInfo *effect_info = vedata->stl->effects;
  DefaultTextureList *dtxl = DRW_viewport_texture_list_get();
  DRWShadingGroup *grp = NULL;

  /* Don't try to show liquid domains here */
  if (!mds->fluid || !(mds->type == FLUID_DOMAIN_TYPE_GAS)) {
    return;
  }

  wpd->volumes_do = true;
  if (mds->use_coba) {
    GPU_create_smoke_coba_field(mmd);
  }
  else if (!(mds->flags & FLUID_DOMAIN_USE_NOISE)) {
    GPU_create_smoke(mmd, 0);
  }
  else if (mds->flags & FLUID_DOMAIN_USE_NOISE) {
    GPU_create_smoke(mmd, 1);
  }

  if ((!mds->use_coba && (mds->tex_density == NULL && mds->tex_color == NULL)) ||
      (mds->use_coba && mds->tex_field == NULL)) {
    return;
  }

  const bool use_slice = (mds->slice_method == FLUID_DOMAIN_SLICE_AXIS_ALIGNED &&
                          mds->axis_slice_method == AXIS_SLICE_SINGLE);
  const bool cubic_interp = (mds->interp_method == VOLUME_INTERP_CUBIC);
  GPUShader *sh = volume_shader_get(use_slice, mds->use_coba, cubic_interp, true);

  if (use_slice) {
    float invviewmat[4][4];
    DRW_view_viewmat_get(NULL, invviewmat, true);

    const int axis = (mds->slice_axis == SLICE_AXIS_AUTO) ?
                         axis_dominant_v3_single(invviewmat[2]) :
                         mds->slice_axis - 1;
    float dim[3];
    BKE_object_dimensions_get(ob, dim);
    /* 0.05f to achieve somewhat the same opacity as the full view.  */
    float step_length = max_ff(1e-16f, dim[axis] * 0.05f);

    grp = DRW_shgroup_create(sh, vedata->psl->volume_pass);
    DRW_shgroup_uniform_float_copy(grp, "slicePosition", mds->slice_depth);
    DRW_shgroup_uniform_int_copy(grp, "sliceAxis", axis);
    DRW_shgroup_uniform_float_copy(grp, "stepLength", step_length);
    DRW_shgroup_state_disable(grp, DRW_STATE_CULL_FRONT);
  }
  else {
    double noise_ofs;
    BLI_halton_1d(3, 0.0, effect_info->jitter_index, &noise_ofs);
    float dim[3], step_length, max_slice;
    float slice_ct[3] = {mds->res[0], mds->res[1], mds->res[2]};
    mul_v3_fl(slice_ct, max_ff(0.001f, mds->slice_per_voxel));
    max_slice = max_fff(slice_ct[0], slice_ct[1], slice_ct[2]);
    BKE_object_dimensions_get(ob, dim);
    invert_v3(slice_ct);
    mul_v3_v3(dim, slice_ct);
    step_length = len_v3(dim);

    grp = DRW_shgroup_create(sh, vedata->psl->volume_pass);
    DRW_shgroup_uniform_vec4(grp, "viewvecs[0]", (float *)wpd->viewvecs, 3);
    DRW_shgroup_uniform_int_copy(grp, "samplesLen", max_slice);
    DRW_shgroup_uniform_float_copy(grp, "stepLength", step_length);
    DRW_shgroup_uniform_float_copy(grp, "noiseOfs", noise_ofs);
    DRW_shgroup_state_enable(grp, DRW_STATE_CULL_FRONT);
  }

  if (mds->use_coba) {
    DRW_shgroup_uniform_texture(grp, "densityTexture", mds->tex_field);
    DRW_shgroup_uniform_texture(grp, "transferTexture", mds->tex_coba);
  }
  else {
    static float white[3] = {1.0f, 1.0f, 1.0f};
    bool use_constant_color = ((mds->active_fields & FLUID_DOMAIN_ACTIVE_COLORS) == 0 &&
                               (mds->active_fields & FLUID_DOMAIN_ACTIVE_COLOR_SET) != 0);
    DRW_shgroup_uniform_texture(
        grp, "densityTexture", (mds->tex_color) ? mds->tex_color : mds->tex_density);
    DRW_shgroup_uniform_texture(grp, "shadowTexture", mds->tex_shadow);
    DRW_shgroup_uniform_texture(
        grp, "flameTexture", (mds->tex_flame) ? mds->tex_flame : e_data.dummy_tex);
    DRW_shgroup_uniform_texture(
        grp, "flameColorTexture", (mds->tex_flame) ? mds->tex_flame_coba : e_data.dummy_coba_tex);
    DRW_shgroup_uniform_vec3(
        grp, "activeColor", (use_constant_color) ? mds->active_color : white, 1);
  }
  DRW_shgroup_uniform_texture_ref(grp, "depthBuffer", &dtxl->depth);
  DRW_shgroup_uniform_float_copy(grp, "densityScale", 10.0f * mds->display_thickness);

  if (use_slice) {
    DRW_shgroup_call(grp, DRW_cache_quad_get(), ob);
  }
  else {
    DRW_shgroup_call(grp, DRW_cache_cube_get(), ob);
  }

  BLI_addtail(&wpd->smoke_domains, BLI_genericNodeN(mmd));
}

static void work_volume_material_color(WORKBENCH_PrivateData *wpd, Object *ob, float color[3])
{
  WORKBENCH_MaterialData material_template;
  Material *ma = BKE_object_material_get(ob, 1);
  int color_type = workbench_material_determine_color_type(wpd, NULL, ob, false);
  workbench_material_update_data(wpd, ob, ma, &material_template, color_type);
  copy_v3_v3(color, material_template.base_color);
}

static void workbench_volume_object_cache_populate(WORKBENCH_Data *vedata, Object *ob)
{
  /* Create 3D textures. */
  Volume *volume = ob->data;
  BKE_volume_load(volume, G.main);
  VolumeGrid *volume_grid = BKE_volume_grid_active_get(volume);
  if (volume_grid == NULL) {
    return;
  }
  DRWVolumeGrid *grid = DRW_volume_batch_cache_get_grid(volume, volume_grid);
  if (grid == NULL) {
    return;
  }

  WORKBENCH_PrivateData *wpd = vedata->stl->g_data;
  WORKBENCH_EffectInfo *effect_info = vedata->stl->effects;
  DefaultTextureList *dtxl = DRW_viewport_texture_list_get();

  wpd->volumes_do = true;

  /* Create shader. */
  GPUShader *sh = volume_shader_get(false, false, false, false);

  /* Compute color. */
  float color[3];
  work_volume_material_color(wpd, ob, color);

  /* Combined texture to object, and object to world transform. */
  float texture_to_world[4][4];
  mul_m4_m4m4(texture_to_world, ob->obmat, grid->texture_to_object);

  /* Compute world space dimensions for step size. */
  float world_size[3];
  mat4_to_size(world_size, texture_to_world);
  abs_v3(world_size);

  /* Compute step parameters. */
  double noise_ofs;
  BLI_halton_1d(3, 0.0, effect_info->jitter_index, &noise_ofs);
  float step_length, max_slice;
  float slice_ct[3] = {grid->resolution[0], grid->resolution[1], grid->resolution[2]};
  mul_v3_fl(slice_ct, max_ff(0.001f, 5.0f));
  max_slice = max_fff(slice_ct[0], slice_ct[1], slice_ct[2]);
  invert_v3(slice_ct);
  mul_v3_v3(slice_ct, world_size);
  step_length = len_v3(slice_ct);

  /* Set uniforms. */
  DRWShadingGroup *grp = DRW_shgroup_create(sh, vedata->psl->volume_pass);
  DRW_shgroup_uniform_vec4(grp, "viewvecs[0]", (float *)wpd->viewvecs, 3);
  DRW_shgroup_uniform_int_copy(grp, "samplesLen", max_slice);
  DRW_shgroup_uniform_float_copy(grp, "stepLength", step_length);
  DRW_shgroup_uniform_float_copy(grp, "noiseOfs", noise_ofs);
  DRW_shgroup_state_enable(grp, DRW_STATE_CULL_FRONT);

  DRW_shgroup_uniform_texture(grp, "densityTexture", grid->texture);
  /* TODO: implement shadow texture, see manta_smoke_calc_transparency. */
  DRW_shgroup_uniform_texture(grp, "shadowTexture", e_data.dummy_shadow_tex);
  DRW_shgroup_uniform_vec3_copy(grp, "activeColor", color);

  DRW_shgroup_uniform_texture_ref(grp, "depthBuffer", &dtxl->depth);
  DRW_shgroup_uniform_float_copy(grp, "densityScale", volume->display.density_scale);

  /* DRW_shgroup_call_obmat is not working here, and also does not
   * support culling, so we hack around it like this. */
  float backup_obmat[4][4], backup_imat[4][4];
  copy_m4_m4(backup_obmat, ob->obmat);
  copy_m4_m4(backup_imat, ob->imat);
  copy_m4_m4(ob->obmat, texture_to_world);
  invert_m4_m4(ob->imat, texture_to_world);
  DRW_shgroup_call(grp, DRW_cache_cube_get(), ob);
  copy_m4_m4(ob->obmat, backup_obmat);
  copy_m4_m4(ob->imat, backup_imat);
}

void workbench_volume_cache_populate(WORKBENCH_Data *vedata,
                                     Scene *UNUSED(scene),
                                     Object *ob,
                                     ModifierData *md)
{
  if (md == NULL) {
    workbench_volume_object_cache_populate(vedata, ob);
  }
  else {
    workbench_volume_modifier_cache_populate(vedata, ob, md);
  }
}

void workbench_volume_smoke_textures_free(WORKBENCH_PrivateData *wpd)
{
  /* Free Smoke Textures after rendering */
  /* XXX This is a waste of processing and GPU bandwidth if nothing
   * is updated. But the problem is since Textures are stored in the
   * modifier we don't want them to take precious VRAM if the
   * modifier is not used for display. We should share them for
   * all viewport in a redraw at least. */
  for (LinkData *link = wpd->smoke_domains.first; link; link = link->next) {
    FluidModifierData *mmd = (FluidModifierData *)link->data;
    GPU_free_smoke(mmd);
  }
  BLI_freelistN(&wpd->smoke_domains);
}
