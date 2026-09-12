@tool
extends EditorImportPlugin

const VGeoImportResource := preload("res://addons/meridian_importer/vgeo_import_resource.gd")


func _get_importer_name() -> String:
	return "meridian.vgeo_importer"


func _get_visible_name() -> String:
	return "Meridian VGEO"


func _get_recognized_extensions() -> PackedStringArray:
	return PackedStringArray(["vgeo"])


func _get_save_extension() -> String:
	return "res"


func _get_resource_type() -> String:
	return "Resource"


func _get_preset_count() -> int:
	return 1


func _get_preset_name(preset_index: int) -> String:
	return "Default"


func _get_import_order() -> int:
	return 0


func _get_priority() -> float:
	return 1.0


func _get_import_options(path: String, preset_index: int) -> Array[Dictionary]:
	return []


func _import(source_file: String, save_path: String, options: Dictionary, platform_variants: Array[String], gen_files: Array[String]) -> Error:
	var summary_file := "%s.summary.txt" % source_file
	if not FileAccess.file_exists(summary_file):
		push_error("Meridian importer expected summary file beside %s" % source_file)
		return ERR_FILE_NOT_FOUND

	var summary_text := FileAccess.get_file_as_string(summary_file)
	var values := _parse_summary(summary_text)
	# The summary is only valid for the .vgeo generation whose content
	# fingerprint it carries; a stale or mismatched sidecar is rejected
	# instead of silently importing defaulted fields.
	var summary_fingerprint := values.get("content_fingerprint", "")
	if summary_fingerprint == "":
		push_error("Meridian importer summary %s has no content_fingerprint" % summary_file)
		return ERR_FILE_CORRUPT
	var vgeo_fingerprint := _read_vgeo_fingerprint(source_file)
	if vgeo_fingerprint == "" or vgeo_fingerprint != summary_fingerprint:
		push_error("Meridian importer summary %s fingerprint %s does not match %s (%s)" % [
			summary_file, summary_fingerprint, source_file, vgeo_fingerprint])
		return ERR_FILE_CORRUPT
	var resource := VGeoImportResource.new()
	resource.asset_id = values.get("asset_id", "")
	resource.source_asset = values.get("source_asset", "")
	resource.has_fallback = values.get("has_fallback", "false") == "true"
	resource.source_vertices = int(values.get("source_vertices", "0"))
	resource.source_triangles = int(values.get("source_triangles", "0"))
	resource.seam_locked_vertices = int(values.get("seam_locked_vertices", "0"))
	resource.material_sections = int(values.get("material_sections", "0"))
	resource.hierarchy_nodes = int(values.get("hierarchy_nodes", "0"))
	resource.clusters = int(values.get("clusters", "0"))
	resource.pages = int(values.get("pages", "0"))
	resource.lod_groups = int(values.get("lod_groups", "0"))
	resource.lod_clusters = int(values.get("lod_clusters", "0"))
	resource.page_dependencies = int(values.get("page_dependencies", "0"))
	resource.summary_path = summary_file

	return ResourceSaver.save(resource, "%s.%s" % [save_path, _get_save_extension()])


# content_fingerprint lives at the end of the VGEO file header (v6):
# magic[4] + 15 u32 fields + Bounds3f (6 floats) + 12 u64 offsets, then
# the u64 fingerprint. Returns "" when the file is not a readable vgeo.
func _read_vgeo_fingerprint(source_file: String) -> String:
	var file := FileAccess.open(source_file, FileAccess.READ)
	if file == null:
		return ""
	var header := file.get_buffer(4)
	if header.size() < 4 or header.get_string_from_ascii() != "VGEO":
		return ""
	const FINGERPRINT_OFFSET := 4 + 15 * 4 + 6 * 4 + 12 * 8
	file.seek(FINGERPRINT_OFFSET)
	var bytes := file.get_buffer(8)
	if bytes.size() < 8:
		return ""
	var fingerprint := 0
	for i in range(7, -1, -1):
		fingerprint = (fingerprint << 8) | bytes[i]
	return String.num_uint64(fingerprint)


func _parse_summary(summary_text: String) -> Dictionary:
	var result := {}
	for line in summary_text.split("\n"):
		if not line.contains("="):
			continue
		var parts := line.split("=", false, 1)
		result[parts[0]] = parts[1]
	return result
