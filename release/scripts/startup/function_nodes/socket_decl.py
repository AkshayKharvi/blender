import bpy
from bpy.props import *
from dataclasses import dataclass
from . sockets import type_infos, OperatorSocket
from . base import DataSocket
import uuid

class SocketDeclBase:
    def build(self, node, node_sockets):
        raise NotImplementedError()

    def amount(self, node):
        raise NotImplementedError()

    def draw_socket(self, layout, node, socket, index):
        socket.draw_self(layout, self)

    def operator_socket_call(self, node, own_socket, other_socket):
        pass

class FixedSocketDecl(SocketDeclBase):
    def __init__(self, identifier: str, display_name: str, data_type: str):
        self.identifier = identifier
        self.display_name = display_name
        self.data_type = data_type

    def build(self, node, node_sockets):
        return [type_infos.build(
            self.data_type,
            node_sockets,
            self.display_name,
            self.identifier)]

    def amount(self, node):
        return 1

class ListSocketDecl(SocketDeclBase):
    def __init__(self, identifier: str, display_name: str, type_property: str):
        self.identifier = identifier
        self.display_name = display_name
        self.type_property = type_property

    def build(self, node, node_sockets):
        base_type = getattr(node, self.type_property)
        list_type = type_infos.to_list(base_type)
        return [type_infos.build(
            list_type,
            node_sockets,
            self.display_name,
            self.identifier)]

    def amount(self, node):
        return 1

    @classmethod
    def Property(cls):
        return StringProperty(default="Float")

class BaseSocketDecl(SocketDeclBase):
    def __init__(self, identifier: str, display_name: str, type_property: str):
        self.identifier = identifier
        self.display_name = display_name
        self.type_property = type_property

    def build(self, node, node_sockets):
        data_type = getattr(node, self.type_property)
        return [type_infos.build(
            data_type,
            node_sockets,
            self.display_name,
            self.identifier)]

    def amount(self, node):
        return 1

    @classmethod
    def Property(cls):
        return StringProperty(default="Float")


class AnyVariadicDecl(SocketDeclBase):
    def __init__(self, identifier: str, prop_name: str, message: str):
        self.identifier_suffix = identifier
        self.prop_name = prop_name
        self.message = message

    def build(self, node, node_sockets):
        return list(self._build(node, node_sockets))

    def _build(self, node, node_sockets):
        for item in getattr(node, self.prop_name):
            yield type_infos.build(
                item.data_type,
                node_sockets,
                item.name,
                item.identifier_prefix + self.identifier_suffix)
        yield node_sockets.new("fn_OperatorSocket", "Operator")

    def amount(self, node):
        return len(getattr(node, self.prop_name)) + 1

    def draw_socket(self, layout, node, socket, index):
        if isinstance(socket, OperatorSocket):
            layout.label(text=self.message)
        else:
            layout.prop(self.get_collection(node)[index], "display_name", text="")

    def get_collection(self, node):
        return getattr(node, self.prop_name)

    def operator_socket_call(self, node, own_socket, other_socket):
        if not isinstance(other_socket, DataSocket):
            return

        is_output = own_socket.is_output
        data_type = other_socket.data_type

        collection = getattr(node, self.prop_name)
        item = collection.add()
        item.data_type = data_type
        item.display_name = other_socket.name
        item.identifier_prefix = str(uuid.uuid4())

        node.rebuild_and_try_keep_state()

        identifier = item.identifier_prefix + self.identifier_suffix
        new_socket = node.find_socket(identifier, is_output)
        node.tree.new_link(other_socket, new_socket)

    @classmethod
    def Property(cls):
        return CollectionProperty(type=DataTypeGroup)

class DataTypeGroup(bpy.types.PropertyGroup):
    bl_idname = "fn_DataTypeGroup"

    data_type: StringProperty()
    display_name: StringProperty()
    identifier_prefix: StringProperty()
