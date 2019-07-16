#include "../usd.h"

#include "usd_hierarchy_iterator.h"
#include "usd_writer_abstract.h"
#include "usd_writer_camera.h"
#include "usd_writer_hair.h"
#include "usd_writer_mesh.h"
#include "usd_writer_transform.h"

#include <string>

#include <pxr/base/tf/stringUtils.h>

extern "C" {
#include "BKE_anim.h"

#include "BLI_assert.h"

#include "DEG_depsgraph_query.h"

#include "DNA_ID.h"
#include "DNA_layer_types.h"
#include "DNA_object_types.h"
}

USDHierarchyIterator::USDHierarchyIterator(Depsgraph *depsgraph,
                                           pxr::UsdStageRefPtr stage,
                                           const USDExportParams &params)
    : AbstractHierarchyIterator(depsgraph), stage(stage), params(params)
{
}

bool USDHierarchyIterator::should_export_object(const Object *object) const
{
  if (params.selected_objects_only && (object->base_flag & BASE_SELECTED) == 0) {
    return false;
  }
  if (params.visible_objects_only && (object->base_flag & BASE_VISIBLE) == 0) {
    return false;
  }
  return true;
}

void USDHierarchyIterator::delete_object_writer(AbstractHierarchyWriter *writer)
{
  delete static_cast<USDAbstractWriter *>(writer);
}

std::string USDHierarchyIterator::make_valid_name(const std::string &name) const
{
  return pxr::TfMakeValidIdentifier(name);
}

void USDHierarchyIterator::set_export_frame(float frame_nr)
{
  // The USD stage is already set up to have FPS timecodes per frame.
  export_time = pxr::UsdTimeCode(frame_nr);
}

const pxr::UsdTimeCode &USDHierarchyIterator::get_export_time_code() const
{
  return export_time;
}

USDExporterContext USDHierarchyIterator::create_usd_export_context(const HierarchyContext &context)
{
  return USDExporterContext{depsgraph, stage, pxr::SdfPath(context.export_path), this, params};
}

AbstractHierarchyWriter *USDHierarchyIterator::create_xform_writer(const HierarchyContext &context)
{
  // printf(
  //     "\033[32;1mCREATE\033[0m %s at %s\n", context.object->id.name,
  //     context.export_path.c_str());
  return new USDTransformWriter(create_usd_export_context(context));
}

AbstractHierarchyWriter *USDHierarchyIterator::create_data_writer(const HierarchyContext &context)
{
  USDExporterContext usd_export_context = create_usd_export_context(context);
  USDAbstractWriter *data_writer = nullptr;

  switch (context.object->type) {
    case OB_MESH:
      data_writer = new USDMeshWriter(usd_export_context);
      break;
    case OB_CAMERA:
      data_writer = new USDCameraWriter(usd_export_context);
      break;

    case OB_EMPTY:
    case OB_CURVE:
    case OB_SURF:
    case OB_FONT:
    case OB_MBALL:
    case OB_LAMP:
    case OB_SPEAKER:
    case OB_LIGHTPROBE:
    case OB_LATTICE:
    case OB_ARMATURE:
    case OB_GPENCIL:
      // printf("USD-\033[34mXFORM-ONLY\033[0m object %s  type=%d (no data writer)\n",
      //        context.object->id.name,
      //        context.object->type);
      return nullptr;
    case OB_TYPE_MAX:
      BLI_assert(!"OB_TYPE_MAX should not be used");
      return nullptr;
  }

  if (!data_writer->is_supported(context.object)) {
    // printf("USD-\033[34mXFORM-ONLY\033[0m object %s  type=%d (data writer rejects the data)\n",
    //        context.object->id.name,
    //        context.object->type);
    delete data_writer;
    return nullptr;
  }

  return data_writer;
}

AbstractHierarchyWriter *USDHierarchyIterator::create_hair_writer(const HierarchyContext &context)
{
  if (!params.export_hair) {
    return nullptr;
  }
  return new USDHairWriter(create_usd_export_context(context));
}

AbstractHierarchyWriter *USDHierarchyIterator::create_particle_writer(const HierarchyContext &)
{
  return nullptr;
}
