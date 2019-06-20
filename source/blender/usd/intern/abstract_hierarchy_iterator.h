#ifndef __USD__ABSTRACT_HIERARCHY_ITERATOR_H__
#define __USD__ABSTRACT_HIERARCHY_ITERATOR_H__

#include <map>
#include <string>
#include <set>

struct Base;
struct Depsgraph;
struct DupliObject;
struct ID;
struct Object;
struct ViewLayer;

class AbstractHierarchyWriter {
 public:
  virtual void write(Object *object_eval) = 0;
  // TODO: add function like unused_during_iteration() that's called when a writer was previously
  // created, but wasn't used this iteration.
};

struct HierarchyContext {
  /* Determined during hierarchy iteration: */
  Object *object;
  Object *export_parent;
  bool xform_only;

  /* Determined during writer creation: */
  std::string export_path;  // Hierarchical path, such as "/grandparent/parent/objectname".
  AbstractHierarchyWriter *parent_writer;  // The parent of this object during the export.

  // For making the struct insertable into a std::set<>.
  bool operator<(const HierarchyContext &other) const
  {
    return object < other.object;
  }
};

class AbstractHierarchyIterator {
 public:
  typedef std::map<std::string, AbstractHierarchyWriter *> WriterMap;

 protected:
  // Mapping from object to its children, as should be exported.
  std::map<Object *, std::set<HierarchyContext>> export_graph;
  std::set<Object *> xform_onlies;

  Depsgraph *depsgraph;
  WriterMap writers;

 public:
  explicit AbstractHierarchyIterator(Depsgraph *depsgraph);
  virtual ~AbstractHierarchyIterator();

  void iterate();
  const WriterMap &writer_map() const;
  void release_writers();

 private:
  void visit_object(Base *base, Object *object, Object *export_parent, bool xform_only);
  void make_writers(Object *parent_object,
                    const std::string &parent_path,
                    AbstractHierarchyWriter *parent_writer);

  std::string get_object_name(const Object *const object) const;

  AbstractHierarchyWriter *get_writer(const std::string &name);

 protected:
  /* Not visiting means not exporting and also not expanding its duplis. */
  virtual bool should_visit_object(const Base *const base, bool is_duplicated) const;
  virtual bool should_visit_duplilink(const DupliObject *const link) const;

  virtual AbstractHierarchyWriter *create_xform_writer(const HierarchyContext &context) = 0;
  virtual AbstractHierarchyWriter *create_data_writer(const HierarchyContext &context) = 0;

  virtual void delete_object_writer(AbstractHierarchyWriter *writer) = 0;

  virtual std::string get_id_name(const ID *const id) const = 0;
  virtual std::string path_concatenate(const std::string &parent_path,
                                       const std::string &child_path) const;
};

#endif /* __USD__ABSTRACT_HIERARCHY_ITERATOR_H__ */
