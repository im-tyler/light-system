extends SceneTree


func _initialize() -> void:
	var resource_path := "res://build/external_fuzz.vgeo"
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with("--resource="):
			resource_path = arg.trim_prefix("--resource=")

	var resource := load(resource_path)
	if resource == null:
		push_error("Failed to load imported resource: %s" % resource_path)
		quit(2)
		return

	print("resource_path=%s" % resource_path)
	print("resource_class=%s" % resource.get_class())
	print("asset_id=%s" % str(resource.get("asset_id")))
	print("source_vertices=%s" % str(resource.get("source_vertices")))
	print("source_triangles=%s" % str(resource.get("source_triangles")))
	print("clusters=%s" % str(resource.get("clusters")))
	print("lod_clusters=%s" % str(resource.get("lod_clusters")))
	print("page_dependencies=%s" % str(resource.get("page_dependencies")))
	quit()
