/*
 * ***** BEGIN GPL LICENSE BLOCK *****
 *
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
 * The Original Code is Copyright (C) Blender Foundation.
 * All rights reserved.
 *
 * The Original Code is: all of this file.
 *
 * Contributor(s): Lukas Toenne
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file bvm_eval.cc
 *  \ingroup bvm
 */

#include <cassert>

extern "C" {
#include "BLI_math.h"
#include "BLI_ghash.h"

#include "DNA_ID.h"
#include "DNA_object_types.h"

#include "BKE_bvhutils.h"
#include "BKE_cdderivedmesh.h"
#include "BKE_DerivedMesh.h"
#include "BKE_material.h"
}

#include "bvm_eval.h"
#include "bvm_eval_common.h"
#include "bvm_eval_curve.h"
#include "bvm_eval_math.h"
#include "bvm_eval_mesh.h"
#include "bvm_eval_texture.h"

#include "bvm_util_hash.h"

namespace bvm {

int EvalGlobals::get_id_key(ID *id)
{
	int hash = BLI_ghashutil_strhash(id->name);
	if (id->lib) {
		hash = hash_combine(hash, BLI_ghashutil_strhash(id->lib->name));
	}
	return hash;
}

void EvalGlobals::add_object(Object *ob)
{
	int key = get_id_key((ID *)ob);
	m_objects[key] = ob;
}

PointerRNA EvalGlobals::lookup_object(int key) const
{
	ObjectMap::const_iterator it = m_objects.find(key);
	if (it != m_objects.end()) {
		PointerRNA ptr;
		RNA_id_pointer_create((ID *)it->second, &ptr);
		return ptr;
	}
	else {
		return PointerRNA_NULL;
	}
}

/* ------------------------------------------------------------------------- */

EvalContext::EvalContext()
{
}

EvalContext::~EvalContext()
{
}

/* ------------------------------------------------------------------------- */

static void eval_op_value_float(float *stack, float value, StackIndex offset)
{
	stack_store_float(stack, offset, value);
}

static void eval_op_value_float3(float *stack, float3 value, StackIndex offset)
{
	stack_store_float3(stack, offset, value);
}

static void eval_op_value_float4(float *stack, float4 value, StackIndex offset)
{
	stack_store_float4(stack, offset, value);
}

static void eval_op_value_int(float *stack, int value, StackIndex offset)
{
	stack_store_int(stack, offset, value);
}

static void eval_op_value_matrix44(float *stack, matrix44 value, StackIndex offset)
{
	stack_store_matrix44(stack, offset, value);
}

/* Note: pointer data is not explicitly stored on the stack,
 * this function always creates simply a NULL pointer.
 */
static void eval_op_value_pointer(float *stack, StackIndex offset)
{
	stack_store_pointer(stack, offset, PointerRNA_NULL);
}

/* Note: mesh data is not explicitly stored on the stack,
 * this function always creates simply an empty mesh.
 */
static void eval_op_value_mesh(float *stack, StackIndex offset)
{
	stack_store_mesh(stack, offset, CDDM_new(0, 0, 0, 0, 0));
}

static void eval_op_float_to_int(float *stack, StackIndex offset_from, StackIndex offset_to)
{
	float f = stack_load_float(stack, offset_from);
	stack_store_int(stack, offset_to, (int)f);
}

static void eval_op_int_to_float(float *stack, StackIndex offset_from, StackIndex offset_to)
{
	int i = stack_load_int(stack, offset_from);
	stack_store_float(stack, offset_to, (float)i);
}

static void eval_op_set_float3(float *stack, StackIndex offset_x, StackIndex offset_y, StackIndex offset_z, StackIndex offset_to)
{
	float x = stack_load_float(stack, offset_x);
	float y = stack_load_float(stack, offset_y);
	float z = stack_load_float(stack, offset_z);
	stack_store_float3(stack, offset_to, float3(x, y, z));
}

static void eval_op_set_float4(float *stack, StackIndex offset_x, StackIndex offset_y,
                               StackIndex offset_z, StackIndex offset_w, StackIndex offset_to)
{
	float x = stack_load_float(stack, offset_x);
	float y = stack_load_float(stack, offset_y);
	float z = stack_load_float(stack, offset_z);
	float w = stack_load_float(stack, offset_w);
	stack_store_float4(stack, offset_to, float4(x, y, z, w));
}

static void eval_op_get_elem_float3(float *stack, int index, StackIndex offset_from, StackIndex offset_to)
{
	assert(index >= 0 && index < 3);
	float3 f = stack_load_float3(stack, offset_from);
	stack_store_float(stack, offset_to, f[index]);
}

static void eval_op_get_elem_float4(float *stack, int index, StackIndex offset_from, StackIndex offset_to)
{
	assert(index >= 0 && index < 4);
	float4 f = stack_load_float4(stack, offset_from);
	stack_store_float(stack, offset_to, f[index]);
}

static void eval_op_init_mesh_ptr(float *stack, StackIndex offset, int use_count)
{
	mesh_ptr p(NULL);
	p.set_use_count(use_count);
	stack_store_mesh_ptr(stack, offset, p);
}

static void eval_op_release_mesh_ptr(float *stack, StackIndex offset)
{
	mesh_ptr p = stack_load_mesh_ptr(stack, offset);
	p.decrement_use_count();
	stack_store_mesh_ptr(stack, offset, p);
}

static void eval_op_mix_rgb(float *stack, int mode, StackIndex offset_col_a, StackIndex offset_col_b, StackIndex offset_fac, StackIndex offset_r)
{
	float4 a = stack_load_float4(stack, offset_col_a);
	float4 b = stack_load_float4(stack, offset_col_b);
	float f = stack_load_float(stack, offset_fac);
	
	ramp_blend(mode, a.data(), f, b.data());
	
	stack_store_float4(stack, offset_r, a);
}

static void eval_op_object_lookup(const EvalGlobals *globals, float *stack, int key, StackIndex offset_object)
{
	PointerRNA ptr = globals->lookup_object(key);
	stack_store_pointer(stack, offset_object, ptr);
}

static void eval_op_effector_transform(const EvalGlobals *globals, float *stack, int object_index, StackIndex offset_tfm)
{
	// TODO the way objects are stored in globals has changed a lot, this needs updating
	(void)globals;
	(void)stack;
	(void)object_index;
	(void)offset_tfm;
//	Object *ob = globals->objects[object_index];
//	matrix44 m = matrix44::from_data(&ob->obmat[0][0], matrix44::COL_MAJOR);
//	stack_store_matrix44(stack, offset_tfm, m);
}

static void eval_op_effector_closest_point(float *stack, StackIndex offset_object, StackIndex offset_vector,
                                           StackIndex offset_position, StackIndex offset_normal, StackIndex offset_tangent)
{
	PointerRNA ptr = stack_load_pointer(stack, offset_object);
	if (!ptr.data)
		return;
	Object *ob = (Object *)ptr.data;
	DerivedMesh *dm = object_get_derived_final(ob, false);
	
	float world[4][4];
	SpaceTransform transform;
	unit_m4(world);
	BLI_space_transform_from_matrices(&transform, world, ob->obmat);
	
	float3 vec;
	vec = stack_load_float3(stack, offset_vector);
	BLI_space_transform_apply(&transform, &vec.x);
	
	BVHTreeFromMesh treeData = {NULL};
	bvhtree_from_mesh_looptri(&treeData, dm, 0.0, 2, 6);
	
	BVHTreeNearest nearest;
	nearest.index = -1;
	nearest.dist_sq = FLT_MAX;
	BLI_bvhtree_find_nearest(treeData.tree, &vec.x, &nearest, treeData.nearest_callback, &treeData);
	
	if (nearest.index != -1) {
		float3 pos, nor;
		copy_v3_v3(&pos.x, nearest.co);
		copy_v3_v3(&nor.x, nearest.no);
		BLI_space_transform_invert(&transform, &pos.x);
		BLI_space_transform_invert_normal(&transform, &nor.x);
		
		stack_store_float3(stack, offset_position, pos);
		stack_store_float3(stack, offset_normal, nor);
		// TODO
		stack_store_float3(stack, offset_tangent, float3(0.0f, 0.0f, 0.0f));
	}
}

void EvalContext::eval_instructions(const EvalGlobals *globals, const Function *fn, int entry_point, float *stack) const
{
	EvalKernelData kd;
	kd.context = this;
	kd.function = fn;
	int instr = entry_point;
	
	while (true) {
		OpCode op = fn->read_opcode(&instr);
		
		switch(op) {
			case OP_NOOP:
				break;
			case OP_VALUE_FLOAT: {
				float value = fn->read_float(&instr);
				StackIndex offset = fn->read_stack_index(&instr);
				eval_op_value_float(stack, value, offset);
				break;
			}
			case OP_VALUE_FLOAT3: {
				float3 value = fn->read_float3(&instr);
				StackIndex offset = fn->read_stack_index(&instr);
				eval_op_value_float3(stack, value, offset);
				break;
			}
			case OP_VALUE_FLOAT4: {
				float4 value = fn->read_float4(&instr);
				StackIndex offset = fn->read_stack_index(&instr);
				eval_op_value_float4(stack, value, offset);
				break;
			}
			case OP_VALUE_INT: {
				int value = fn->read_int(&instr);
				StackIndex offset = fn->read_stack_index(&instr);
				eval_op_value_int(stack, value, offset);
				break;
			}
			case OP_VALUE_MATRIX44: {
				matrix44 value = fn->read_matrix44(&instr);
				StackIndex offset = fn->read_stack_index(&instr);
				eval_op_value_matrix44(stack, value, offset);
				break;
			}
			case OP_VALUE_POINTER: {
				StackIndex offset = fn->read_stack_index(&instr);
				eval_op_value_pointer(stack, offset);
				break;
			}
			case OP_VALUE_MESH: {
				StackIndex offset = fn->read_stack_index(&instr);
				eval_op_value_mesh(stack, offset);
				break;
			}
			case OP_FLOAT_TO_INT: {
				StackIndex offset_from = fn->read_stack_index(&instr);
				StackIndex offset_to = fn->read_stack_index(&instr);
				eval_op_float_to_int(stack, offset_from, offset_to);
				break;
			}
			case OP_INT_TO_FLOAT: {
				StackIndex offset_from = fn->read_stack_index(&instr);
				StackIndex offset_to = fn->read_stack_index(&instr);
				eval_op_int_to_float(stack, offset_from, offset_to);
				break;
			}
			case OP_SET_FLOAT3: {
				StackIndex offset_x = fn->read_stack_index(&instr);
				StackIndex offset_y = fn->read_stack_index(&instr);
				StackIndex offset_z = fn->read_stack_index(&instr);
				StackIndex offset_to = fn->read_stack_index(&instr);
				eval_op_set_float3(stack, offset_x, offset_y, offset_z, offset_to);
				break;
			}
			case OP_GET_ELEM_FLOAT3: {
				int index = fn->read_int(&instr);
				StackIndex offset_from = fn->read_stack_index(&instr);
				StackIndex offset_to = fn->read_stack_index(&instr);
				eval_op_get_elem_float3(stack, index, offset_from, offset_to);
				break;
			}
			case OP_SET_FLOAT4: {
				StackIndex offset_x = fn->read_stack_index(&instr);
				StackIndex offset_y = fn->read_stack_index(&instr);
				StackIndex offset_z = fn->read_stack_index(&instr);
				StackIndex offset_w = fn->read_stack_index(&instr);
				StackIndex offset_to = fn->read_stack_index(&instr);
				eval_op_set_float4(stack, offset_x, offset_y, offset_z, offset_w, offset_to);
				break;
			}
			case OP_GET_ELEM_FLOAT4: {
				int index = fn->read_int(&instr);
				StackIndex offset_from = fn->read_stack_index(&instr);
				StackIndex offset_to = fn->read_stack_index(&instr);
				eval_op_get_elem_float4(stack, index, offset_from, offset_to);
				break;
			}
			case OP_INIT_MESH_PTR: {
				StackIndex offset = fn->read_stack_index(&instr);
				int use_count = fn->read_int(&instr);
				eval_op_init_mesh_ptr(stack, offset, use_count);
				break;
			}
			case OP_RELEASE_MESH_PTR: {
				StackIndex offset = fn->read_stack_index(&instr);
				eval_op_release_mesh_ptr(stack, offset);
				break;
			}
			case OP_ADD_FLOAT: {
				StackIndex offset_a = fn->read_stack_index(&instr);
				StackIndex offset_b = fn->read_stack_index(&instr);
				StackIndex offset_r = fn->read_stack_index(&instr);
				eval_op_add_float(stack, offset_a, offset_b, offset_r);
				break;
			}
			case OP_SUB_FLOAT: {
				StackIndex offset_a = fn->read_stack_index(&instr);
				StackIndex offset_b = fn->read_stack_index(&instr);
				StackIndex offset_r = fn->read_stack_index(&instr);
				eval_op_sub_float(stack, offset_a, offset_b, offset_r);
				break;
			}
			case OP_MUL_FLOAT: {
				StackIndex offset_a = fn->read_stack_index(&instr);
				StackIndex offset_b = fn->read_stack_index(&instr);
				StackIndex offset_r = fn->read_stack_index(&instr);
				eval_op_mul_float(stack, offset_a, offset_b, offset_r);
				break;
			}
			case OP_DIV_FLOAT: {
				StackIndex offset_a = fn->read_stack_index(&instr);
				StackIndex offset_b = fn->read_stack_index(&instr);
				StackIndex offset_r = fn->read_stack_index(&instr);
				eval_op_div_float(stack, offset_a, offset_b, offset_r);
				break;
			}
			case OP_SINE: {
				StackIndex offset = fn->read_stack_index(&instr);
				StackIndex offset_r = fn->read_stack_index(&instr);
				eval_op_sine(stack, offset, offset_r);
				break;
			}
			case OP_COSINE: {
				StackIndex offset = fn->read_stack_index(&instr);
				StackIndex offset_r = fn->read_stack_index(&instr);
				eval_op_cosine(stack, offset, offset_r);
				break;
			}
			case OP_TANGENT: {
				StackIndex offset = fn->read_stack_index(&instr);
				StackIndex offset_r = fn->read_stack_index(&instr);
				eval_op_tangent(stack, offset, offset_r);
				break;
			}
			case OP_ARCSINE: {
				StackIndex offset = fn->read_stack_index(&instr);
				StackIndex offset_r = fn->read_stack_index(&instr);
				eval_op_arcsine(stack, offset, offset_r);
				break;
			}
			case OP_ARCCOSINE: {
				StackIndex offset = fn->read_stack_index(&instr);
				StackIndex offset_r = fn->read_stack_index(&instr);
				eval_op_arccosine(stack, offset, offset_r);
				break;
			}
			case OP_ARCTANGENT: {
				StackIndex offset = fn->read_stack_index(&instr);
				StackIndex offset_r = fn->read_stack_index(&instr);
				eval_op_arctangent(stack, offset, offset_r);
				break;
			}
			case OP_POWER: {
				StackIndex offset_a = fn->read_stack_index(&instr);
				StackIndex offset_b = fn->read_stack_index(&instr);
				StackIndex offset_r = fn->read_stack_index(&instr);
				eval_op_power(stack, offset_a, offset_b, offset_r);
				break;
			}
			case OP_LOGARITHM: {
				StackIndex offset_a = fn->read_stack_index(&instr);
				StackIndex offset_b = fn->read_stack_index(&instr);
				StackIndex offset_r = fn->read_stack_index(&instr);
				eval_op_logarithm(stack, offset_a, offset_b, offset_r);
				break;
			}
			case OP_MINIMUM: {
				StackIndex offset_a = fn->read_stack_index(&instr);
				StackIndex offset_b = fn->read_stack_index(&instr);
				StackIndex offset_r = fn->read_stack_index(&instr);
				eval_op_minimum(stack, offset_a, offset_b, offset_r);
				break;
			}
			case OP_MAXIMUM: {
				StackIndex offset_a = fn->read_stack_index(&instr);
				StackIndex offset_b = fn->read_stack_index(&instr);
				StackIndex offset_r = fn->read_stack_index(&instr);
				eval_op_maximum(stack, offset_a, offset_b, offset_r);
				break;
			}
			case OP_ROUND: {
				StackIndex offset = fn->read_stack_index(&instr);
				StackIndex offset_r = fn->read_stack_index(&instr);
				eval_op_round(stack, offset, offset_r);
				break;
			}
			case OP_LESS_THAN: {
				StackIndex offset_a = fn->read_stack_index(&instr);
				StackIndex offset_b = fn->read_stack_index(&instr);
				StackIndex offset_r = fn->read_stack_index(&instr);
				eval_op_less_than(stack, offset_a, offset_b, offset_r);
				break;
			}
			case OP_GREATER_THAN: {
				StackIndex offset_a = fn->read_stack_index(&instr);
				StackIndex offset_b = fn->read_stack_index(&instr);
				StackIndex offset_r = fn->read_stack_index(&instr);
				eval_op_greater_than(stack, offset_a, offset_b, offset_r);
				break;
			}
			case OP_MODULO: {
				StackIndex offset_a = fn->read_stack_index(&instr);
				StackIndex offset_b = fn->read_stack_index(&instr);
				StackIndex offset_r = fn->read_stack_index(&instr);
				eval_op_modulo(stack, offset_a, offset_b, offset_r);
				break;
			}
			case OP_ABSOLUTE: {
				StackIndex offset = fn->read_stack_index(&instr);
				StackIndex offset_r = fn->read_stack_index(&instr);
				eval_op_absolute(stack, offset, offset_r);
				break;
			}
			case OP_CLAMP: {
				StackIndex offset = fn->read_stack_index(&instr);
				StackIndex offset_r = fn->read_stack_index(&instr);
				eval_op_clamp(stack, offset, offset_r);
				break;
			}
			case OP_ADD_FLOAT3: {
				StackIndex offset_a = fn->read_stack_index(&instr);
				StackIndex offset_b = fn->read_stack_index(&instr);
				StackIndex offset_r = fn->read_stack_index(&instr);
				eval_op_add_float3(stack, offset_a, offset_b, offset_r);
				break;
			}
			case OP_SUB_FLOAT3: {
				StackIndex offset_a = fn->read_stack_index(&instr);
				StackIndex offset_b = fn->read_stack_index(&instr);
				StackIndex offset_r = fn->read_stack_index(&instr);
				eval_op_sub_float3(stack, offset_a, offset_b, offset_r);
				break;
			}
			case OP_MUL_FLOAT3: {
				StackIndex offset_a = fn->read_stack_index(&instr);
				StackIndex offset_b = fn->read_stack_index(&instr);
				StackIndex offset_r = fn->read_stack_index(&instr);
				eval_op_mul_float3(stack, offset_a, offset_b, offset_r);
				break;
			}
			case OP_DIV_FLOAT3: {
				StackIndex offset_a = fn->read_stack_index(&instr);
				StackIndex offset_b = fn->read_stack_index(&instr);
				StackIndex offset_r = fn->read_stack_index(&instr);
				eval_op_div_float3(stack, offset_a, offset_b, offset_r);
				break;
			}
			case OP_MUL_FLOAT3_FLOAT: {
				StackIndex offset_a = fn->read_stack_index(&instr);
				StackIndex offset_b = fn->read_stack_index(&instr);
				StackIndex offset_r = fn->read_stack_index(&instr);
				eval_op_mul_float3_float(stack, offset_a, offset_b, offset_r);
				break;
			}
			case OP_DIV_FLOAT3_FLOAT: {
				StackIndex offset_a = fn->read_stack_index(&instr);
				StackIndex offset_b = fn->read_stack_index(&instr);
				StackIndex offset_r = fn->read_stack_index(&instr);
				eval_op_div_float3_float(stack, offset_a, offset_b, offset_r);
				break;
			}
			case OP_AVERAGE_FLOAT3: {
				StackIndex offset_a = fn->read_stack_index(&instr);
				StackIndex offset_b = fn->read_stack_index(&instr);
				StackIndex offset_r = fn->read_stack_index(&instr);
				eval_op_average_float3(stack, offset_a, offset_b, offset_r);
				break;
			}
			case OP_DOT_FLOAT3: {
				StackIndex offset_a = fn->read_stack_index(&instr);
				StackIndex offset_b = fn->read_stack_index(&instr);
				StackIndex offset_r = fn->read_stack_index(&instr);
				eval_op_dot_float3(stack, offset_a, offset_b, offset_r);
				break;
			}
			case OP_CROSS_FLOAT3: {
				StackIndex offset_a = fn->read_stack_index(&instr);
				StackIndex offset_b = fn->read_stack_index(&instr);
				StackIndex offset_r = fn->read_stack_index(&instr);
				eval_op_cross_float3(stack, offset_a, offset_b, offset_r);
				break;
			}
			case OP_NORMALIZE_FLOAT3: {
				StackIndex offset = fn->read_stack_index(&instr);
				StackIndex offset_vec = fn->read_stack_index(&instr);
				StackIndex offset_val = fn->read_stack_index(&instr);
				eval_op_normalize_float3(stack, offset, offset_vec, offset_val);
				break;
			}
			case OP_ADD_MATRIX44: {
				StackIndex offset_a = fn->read_stack_index(&instr);
				StackIndex offset_b = fn->read_stack_index(&instr);
				StackIndex offset_r = fn->read_stack_index(&instr);
				eval_op_add_matrix44(stack, offset_a, offset_b, offset_r);
				break;
			}
			case OP_SUB_MATRIX44: {
				StackIndex offset_a = fn->read_stack_index(&instr);
				StackIndex offset_b = fn->read_stack_index(&instr);
				StackIndex offset_r = fn->read_stack_index(&instr);
				eval_op_sub_matrix44(stack, offset_a, offset_b, offset_r);
				break;
			}
			case OP_MUL_MATRIX44: {
				StackIndex offset_a = fn->read_stack_index(&instr);
				StackIndex offset_b = fn->read_stack_index(&instr);
				StackIndex offset_r = fn->read_stack_index(&instr);
				eval_op_mul_matrix44(stack, offset_a, offset_b, offset_r);
				break;
			}
			case OP_MUL_MATRIX44_FLOAT: {
				StackIndex offset_a = fn->read_stack_index(&instr);
				StackIndex offset_b = fn->read_stack_index(&instr);
				StackIndex offset_r = fn->read_stack_index(&instr);
				eval_op_mul_matrix44_float(stack, offset_a, offset_b, offset_r);
				break;
			}
			case OP_DIV_MATRIX44_FLOAT: {
				StackIndex offset_a = fn->read_stack_index(&instr);
				StackIndex offset_b = fn->read_stack_index(&instr);
				StackIndex offset_r = fn->read_stack_index(&instr);
				eval_op_div_matrix44_float(stack, offset_a, offset_b, offset_r);
				break;
			}
			case OP_NEGATE_MATRIX44: {
				StackIndex offset = fn->read_stack_index(&instr);
				StackIndex offset_r = fn->read_stack_index(&instr);
				eval_op_negate_matrix44(stack, offset, offset_r);
				break;
			}
			case OP_TRANSPOSE_MATRIX44: {
				StackIndex offset = fn->read_stack_index(&instr);
				StackIndex offset_r = fn->read_stack_index(&instr);
				eval_op_transpose_matrix44(stack, offset, offset_r);
				break;
			}
			case OP_INVERT_MATRIX44: {
				StackIndex offset = fn->read_stack_index(&instr);
				StackIndex offset_r = fn->read_stack_index(&instr);
				eval_op_invert_matrix44(stack, offset, offset_r);
				break;
			}
			case OP_ADJOINT_MATRIX44: {
				StackIndex offset = fn->read_stack_index(&instr);
				StackIndex offset_r = fn->read_stack_index(&instr);
				eval_op_adjoint_matrix44(stack, offset, offset_r);
				break;
			}
			case OP_DETERMINANT_MATRIX44: {
				StackIndex offset = fn->read_stack_index(&instr);
				StackIndex offset_r = fn->read_stack_index(&instr);
				eval_op_determinant_matrix44(stack, offset, offset_r);
				break;
			}
			case OP_MUL_MATRIX44_FLOAT3: {
				StackIndex offset_a = fn->read_stack_index(&instr);
				StackIndex offset_b = fn->read_stack_index(&instr);
				StackIndex offset_r = fn->read_stack_index(&instr);
				eval_op_mul_matrix44_float3(stack, offset_a, offset_b, offset_r);
				break;
			}
			case OP_MUL_MATRIX44_FLOAT4: {
				StackIndex offset_a = fn->read_stack_index(&instr);
				StackIndex offset_b = fn->read_stack_index(&instr);
				StackIndex offset_r = fn->read_stack_index(&instr);
				eval_op_mul_matrix44_float4(stack, offset_a, offset_b, offset_r);
				break;
			}
			case OP_MATRIX44_TO_LOC: {
				StackIndex offset_mat = fn->read_stack_index(&instr);
				StackIndex offset_loc = fn->read_stack_index(&instr);
				eval_op_matrix44_to_loc(stack, offset_mat, offset_loc);
				break;
			}
			case OP_MATRIX44_TO_EULER: {
				int order = fn->read_int(&instr);
				StackIndex offset_mat = fn->read_stack_index(&instr);
				StackIndex offset_euler = fn->read_stack_index(&instr);
				eval_op_matrix44_to_euler(stack, order, offset_mat, offset_euler);
				break;
			}
			case OP_MATRIX44_TO_AXISANGLE: {
				StackIndex offset_mat = fn->read_stack_index(&instr);
				StackIndex offset_axis = fn->read_stack_index(&instr);
				StackIndex offset_angle = fn->read_stack_index(&instr);
				eval_op_matrix44_to_axisangle(stack, offset_mat, offset_axis, offset_angle);
				break;
			}
			case OP_MATRIX44_TO_SCALE: {
				StackIndex offset_mat = fn->read_stack_index(&instr);
				StackIndex offset_scale = fn->read_stack_index(&instr);
				eval_op_matrix44_to_scale(stack, offset_mat, offset_scale);
				break;
			}
			case OP_LOC_TO_MATRIX44: {
				StackIndex offset_loc = fn->read_stack_index(&instr);
				StackIndex offset_mat = fn->read_stack_index(&instr);
				eval_op_loc_to_matrix44(stack, offset_loc, offset_mat);
				break;
			}
			case OP_EULER_TO_MATRIX44: {
				int order = fn->read_int(&instr);
				StackIndex offset_euler = fn->read_stack_index(&instr);
				StackIndex offset_mat = fn->read_stack_index(&instr);
				eval_op_euler_to_matrix44(stack, order, offset_euler, offset_mat);
				break;
			}
			case OP_AXISANGLE_TO_MATRIX44: {
				StackIndex offset_axis = fn->read_stack_index(&instr);
				StackIndex offset_angle = fn->read_stack_index(&instr);
				StackIndex offset_mat = fn->read_stack_index(&instr);
				eval_op_axisangle_to_matrix44(stack, offset_axis, offset_angle, offset_mat);
				break;
			}
			case OP_SCALE_TO_MATRIX44: {
				StackIndex offset_scale = fn->read_stack_index(&instr);
				StackIndex offset_mat = fn->read_stack_index(&instr);
				eval_op_scale_to_matrix44(stack, offset_scale, offset_mat);
				break;
			}
			
			case OP_MIX_RGB: {
				int mode = fn->read_int(&instr);
				StackIndex offset_fac = fn->read_stack_index(&instr);
				StackIndex offset_col_a = fn->read_stack_index(&instr);
				StackIndex offset_col_b = fn->read_stack_index(&instr);
				StackIndex offset_r = fn->read_stack_index(&instr);
				eval_op_mix_rgb(stack, mode, offset_col_a, offset_col_b, offset_fac, offset_r);
				break;
			}
			
			case OP_INT_TO_RANDOM: {
				int seed = fn->read_int(&instr);
				StackIndex offset = fn->read_stack_index(&instr);
				StackIndex offset_irandom = fn->read_stack_index(&instr);
				StackIndex offset_frandom = fn->read_stack_index(&instr);
				eval_op_int_to_random(stack, (uint64_t)seed, offset, offset_irandom, offset_frandom);
				break;
			}
			case OP_FLOAT_TO_RANDOM: {
				int seed = fn->read_int(&instr);
				StackIndex offset = fn->read_stack_index(&instr);
				StackIndex offset_irandom = fn->read_stack_index(&instr);
				StackIndex offset_frandom = fn->read_stack_index(&instr);
				eval_op_float_to_random(stack, (uint64_t)seed, offset, offset_irandom, offset_frandom);
				break;
			}
			
			case OP_TEX_PROC_VORONOI: {
				int distance_metric = fn->read_int(&instr);
				int color_type = fn->read_int(&instr);
				StackIndex iMinkowskiExponent = fn->read_stack_index(&instr);
				StackIndex iScale = fn->read_stack_index(&instr);
				StackIndex iNoiseSize = fn->read_stack_index(&instr);
				StackIndex iNabla = fn->read_stack_index(&instr);
				StackIndex iW1 = fn->read_stack_index(&instr);
				StackIndex iW2 = fn->read_stack_index(&instr);
				StackIndex iW3 = fn->read_stack_index(&instr);
				StackIndex iW4 = fn->read_stack_index(&instr);
				StackIndex iPos = fn->read_stack_index(&instr);
				StackIndex oIntensity = fn->read_stack_index(&instr);
				StackIndex oColor = fn->read_stack_index(&instr);
				StackIndex oNormal = fn->read_stack_index(&instr);
				eval_op_tex_proc_voronoi(stack, distance_metric, color_type,
				                         iMinkowskiExponent, iScale, iNoiseSize, iNabla,
				                         iW1, iW2, iW3, iW4, iPos,
				                         oIntensity, oColor, oNormal);
				break;
			}
			case OP_TEX_PROC_CLOUDS: {
				StackIndex iPos = fn->read_stack_index(&instr);
				StackIndex iNabla = fn->read_stack_index(&instr);
				StackIndex iSize = fn->read_stack_index(&instr);
				int iDepth = fn->read_int(&instr);
				int iNoiseBasis = fn->read_int(&instr);
				int iNoiseHard = fn->read_int(&instr);
				StackIndex oIntensity = fn->read_stack_index(&instr);
				StackIndex oColor = fn->read_stack_index(&instr);
				StackIndex oNormal = fn->read_stack_index(&instr);
				eval_op_tex_proc_clouds(stack, iPos, iNabla, iSize,
				                        iDepth, iNoiseBasis, iNoiseHard,
				                        oIntensity, oColor, oNormal);
				break;
			}
			
			case OB_OBJECT_LOOKUP: {
				int key = fn->read_int(&instr);
				StackIndex offset_object = fn->read_stack_index(&instr);
				eval_op_object_lookup(globals, stack, key, offset_object);
				break;
			}
			
			case OP_EFFECTOR_TRANSFORM: {
				int object_index = fn->read_int(&instr);
				StackIndex offset_tfm = fn->read_stack_index(&instr);
				eval_op_effector_transform(globals, stack, object_index, offset_tfm);
				break;
			}
			case OP_EFFECTOR_CLOSEST_POINT: {
				StackIndex offset_object = fn->read_stack_index(&instr);
				StackIndex offset_vector = fn->read_stack_index(&instr);
				StackIndex offset_position = fn->read_stack_index(&instr);
				StackIndex offset_normal = fn->read_stack_index(&instr);
				StackIndex offset_tangent = fn->read_stack_index(&instr);
				eval_op_effector_closest_point(stack, offset_object, offset_vector,
				                               offset_position, offset_normal, offset_tangent);
				break;
			}
			case OP_MESH_LOAD: {
				StackIndex offset_base_mesh = fn->read_stack_index(&instr);
				StackIndex offset_mesh = fn->read_stack_index(&instr);
				eval_op_mesh_load(stack, offset_base_mesh, offset_mesh);
				break;
			}
			case OP_MESH_COMBINE: {
				StackIndex offset_mesh_a = fn->read_stack_index(&instr);
				StackIndex offset_mesh_b = fn->read_stack_index(&instr);
				StackIndex offset_mesh_out = fn->read_stack_index(&instr);
				eval_op_mesh_combine(&kd, stack, offset_mesh_a, offset_mesh_b, offset_mesh_out);
				break;
			}
			case OP_MESH_ARRAY: {
				StackIndex offset_mesh_in = fn->read_stack_index(&instr);
				StackIndex offset_count = fn->read_stack_index(&instr);
				int fn_transform = fn->read_jump_address(&instr);
				StackIndex offset_transform = fn->read_stack_index(&instr);
				StackIndex offset_mesh_out = fn->read_stack_index(&instr);
				StackIndex offset_iteration = fn->read_stack_index(&instr);
				eval_op_mesh_array(globals, &kd, stack,
				                   offset_mesh_in, offset_mesh_out, offset_count,
				                   fn_transform, offset_transform, offset_iteration);
				break;
			}
			case OP_MESH_DISPLACE: {
				StackIndex offset_mesh_in = fn->read_stack_index(&instr);
				int fn_vector = fn->read_jump_address(&instr);
				StackIndex offset_vector = fn->read_stack_index(&instr);
				StackIndex offset_mesh_out = fn->read_stack_index(&instr);
				StackIndex offset_elem_index = fn->read_stack_index(&instr);
				StackIndex offset_elem_loc = fn->read_stack_index(&instr);
				eval_op_mesh_displace(globals, &kd, stack,
				                      offset_mesh_in, offset_mesh_out, fn_vector, offset_vector,
				                      offset_elem_index, offset_elem_loc);
				break;
			}
			
			case OP_CURVE_PATH: {
				StackIndex offset_object = fn->read_stack_index(&instr);
				StackIndex offset_param = fn->read_stack_index(&instr);
				StackIndex offset_loc = fn->read_stack_index(&instr);
				StackIndex offset_dir = fn->read_stack_index(&instr);
				StackIndex offset_nor = fn->read_stack_index(&instr);
				StackIndex offset_rot = fn->read_stack_index(&instr);
				StackIndex offset_radius = fn->read_stack_index(&instr);
				StackIndex offset_weight = fn->read_stack_index(&instr);
				eval_op_curve_path(stack, offset_object, offset_param,
				                   offset_loc, offset_dir, offset_nor,
				                   offset_rot, offset_radius, offset_weight);
				break;
			}
			
			case OP_END:
				return;
			default:
				assert(!"Unknown opcode");
				return;
		}
	}
}

void EvalContext::eval_function(const EvalGlobals *globals, const Function *fn, const void *arguments[], void *results[]) const
{
	float stack[BVM_STACK_SIZE] = {0};
	
	/* initialize input arguments */
	for (int i = 0; i < fn->num_arguments(); ++i) {
		const Argument &arg = fn->argument(i);
		if (arg.stack_offset != BVM_STACK_INVALID) {
			float *value = &stack[arg.stack_offset];
			
			arg.typedesc.copy_value((void *)value, arguments[i]);
		}
	}
	
	eval_instructions(globals, fn, fn->entry_point(), stack);
	
	/* read out return values */
	for (int i = 0; i < fn->num_return_values(); ++i) {
		const Argument &rval = fn->return_value(i);
		float *value = &stack[rval.stack_offset];
		
		rval.typedesc.copy_value(results[i], (void *)value);
	}
}

void EvalContext::eval_expression(const EvalGlobals *globals, const Function *fn, int entry_point, float *stack) const
{
	eval_instructions(globals, fn, entry_point, stack);
}

} /* namespace bvm */
