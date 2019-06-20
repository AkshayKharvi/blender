#include "usd_hierarchy_iterator.h"
#include "usd_writer_abstract.h"
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

USDHierarchyIterator::USDHierarchyIterator(Depsgraph *depsgraph, pxr::UsdStageRefPtr stage)
    : AbstractHierarchyIterator(depsgraph), stage(stage)
{
}

void USDHierarchyIterator::delete_object_writer(AbstractHierarchyWriter *writer)
{
  delete static_cast<USDAbstractWriter *>(writer);
}

std::string USDHierarchyIterator::get_id_name(const ID *const id) const
{
  BLI_assert(id != nullptr);
  std::string name(id->name + 2);
  return pxr::TfMakeValidIdentifier(name);
}

AbstractHierarchyWriter *USDHierarchyIterator::create_xform_writer(const HierarchyContext &context)
{
  printf(
      "\033[32;1mCREATE\033[0m %s at %s\n", context.object->id.name, context.export_path.c_str());

  USDExporterContext usd_export_context = {depsgraph, stage, pxr::SdfPath(context.export_path)};
  return new USDTransformWriter(usd_export_context);
}

AbstractHierarchyWriter *USDHierarchyIterator::create_data_writer(const HierarchyContext &context)
{
  USDExporterContext usd_export_context = {depsgraph, stage, pxr::SdfPath(context.export_path)};
  USDAbstractWriter *data_writer = nullptr;

  switch (context.object->type) {
    case OB_MESH:
      data_writer = new USDMeshWriter(usd_export_context);
      break;
    default:
      printf("USD-\033[34mXFORM-ONLY\033[0m object %s  type=%d (no data writer)\n",
             context.object->id.name,
             context.object->type);
      return nullptr;
  }

  if (!data_writer->is_supported()) {
    printf("USD-\033[34mXFORM-ONLY\033[0m object %s  type=%d (data writer rejects the data)\n",
           context.object->id.name,
           context.object->type);
    delete data_writer;
    return nullptr;
  }

  return data_writer;
}
