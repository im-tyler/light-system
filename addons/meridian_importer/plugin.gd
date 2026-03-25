@tool
extends EditorPlugin

var importer: EditorImportPlugin


func _enter_tree() -> void:
	importer = preload("res://addons/meridian_importer/vgeo_import_plugin.gd").new()
	add_import_plugin(importer)


func _exit_tree() -> void:
	if importer != null:
		remove_import_plugin(importer)
		importer = null
