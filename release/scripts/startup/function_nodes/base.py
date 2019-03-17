import bpy
from bpy.props import *
from . utils.generic import iter_subclasses_recursive
import itertools
from collections import defaultdict

class BaseTree:
    def new_link(self, a, b):
        if a.is_output:
            self.links.new(a, b)
        else:
            self.links.new(b, a)

class FunctionNodeTree(bpy.types.NodeTree, BaseTree):
    bl_idname = "FunctionNodeTree"
    bl_icon = "MOD_DATA_TRANSFER"
    bl_label = "Function Nodes"


class NodeStorage:
    def __init__(self, node):
        self.node = node
        self.set_current_declaration(*node.get_sockets())

    def set_current_declaration(self, inputs, outputs):
        self.inputs_decl = inputs
        self.outputs_decl = outputs

        self.inputs_per_decl = {}
        sockets = iter(self.node.inputs)
        for decl in self.inputs_decl:
            group = tuple(itertools.islice(sockets, decl.amount(self.node)))
            self.inputs_per_decl[decl] = group

        self.outputs_per_decl = {}
        sockets = iter(self.node.outputs)
        for decl in self.outputs_decl:
            group = tuple(itertools.islice(sockets, decl.amount(self.node)))
            self.outputs_per_decl[decl] = group

        self.sockets_per_decl = {}
        self.sockets_per_decl.update(self.inputs_per_decl)
        self.sockets_per_decl.update(self.outputs_per_decl)

        self.decl_per_socket = {}
        self.decl_index_per_socket = {}
        for decl, sockets in self.sockets_per_decl.items():
            for i, socket in enumerate(sockets):
                self.decl_per_socket[socket] = decl
                self.decl_index_per_socket[socket] = i

_storage_per_node = {}

class BaseNode:
    def init(self, context):
        inputs, outputs = self.get_sockets()
        for decl in inputs:
            decl.build(self, self.inputs)
        for decl in outputs:
            decl.build(self, self.outputs)

    def rebuild_and_try_keep_state(self):
        state = self._get_state()
        self.rebuild()
        self._try_set_state(state)

    def rebuild(self):
        self.inputs.clear()
        self.outputs.clear()

        inputs, outputs = self.get_sockets()
        for decl in self.storage.inputs_decl:
            decl.build(self, self.inputs)
        for decl in self.storage.outputs_decl:
            decl.build(self, self.outputs)
        self.storage.set_current_declaration(inputs, outputs)

    def _get_state(self):
        links_per_input = defaultdict(set)
        links_per_output = defaultdict(set)

        for link in self.tree.links:
            if link.from_node == self:
                links_per_output[link.from_socket.identifier].add(link.to_socket)
            if link.to_node == self:
                links_per_input[link.to_socket.identifier].add(link.from_socket)

        return (links_per_input, links_per_output)

    def _try_set_state(self, state):
        tree = self.tree
        for socket in self.inputs:
            for from_socket in state[0][socket.identifier]:
                tree.links.new(socket, from_socket)
        for socket in self.outputs:
            for to_socket in state[1][socket.identifier]:
                tree.links.new(to_socket, socket)

    @property
    def tree(self):
        return self.id_data

    def get_sockets():
        return [], []

    def draw_buttons(self, context, layout):
        self.draw(layout)

    def draw(self, layout):
        pass

    def invoke_function(self,
            layout, function_name, text,
            *, icon="NONE", settings=tuple()):
        assert isinstance(settings, tuple)
        props = layout.operator("fn.node_operator", text=text, icon=icon)
        props.tree_name = self.id_data.name
        props.node_name = self.name
        props.function_name = function_name
        props.settings_repr = repr(settings)

    def draw_socket(self, socket, layout):
        decl = self.storage.decl_per_socket[socket]
        index = self.storage.decl_index_per_socket[socket]
        decl.draw_socket(layout, self, socket, index)

    @classmethod
    def iter_final_subclasses(cls):
        yield from filter(lambda x: issubclass(x, bpy.types.Node), iter_subclasses_recursive(cls))

    def find_input(self, identifier):
        for socket in self.inputs:
            if socket.identifier == identifier:
                return socket
        else:
            return None

    def find_output(self, identifier):
        for socket in self.outputs:
            if socket.identifier == identifier:
                return socket
        else:
            return None

    def find_socket(self, identifier, is_output):
        if is_output:
            return self.find_output(identifier)
        else:
            return self.find_input(identifier)

    def iter_sockets(self):
        yield from self.inputs
        yield from self.outputs

    # Storage
    #########################

    @property
    def storage(self):
        if self not in _storage_per_node:
            _storage_per_node[self] = NodeStorage(self)
        return _storage_per_node[self]



class BaseSocket:
    color = (0, 0, 0, 0)

    def draw_color(self, context, node):
        return self.color

    def draw(self, context, layout, node, text):
        node.draw_socket(self, layout)

    def draw_self(self, layout, node):
        layout.label(text=self.name)

    def get_index(self, node):
        if self.is_output:
            return tuple(node.outputs).index(self)
        else:
            return tuple(node.inputs).index(self)

class FunctionNode(BaseNode):
    pass

class DataSocket(BaseSocket):
    data_type: StringProperty(
        maxlen=64)

    def draw_self(self, layout, node):
        text = self.name
        if not (self.is_linked or self.is_output) and hasattr(self, "draw_property"):
            self.draw_property(layout, node, text)
        else:
            layout.label(text=text)

