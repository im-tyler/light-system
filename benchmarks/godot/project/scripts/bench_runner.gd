extends Node3D

# Stock-Godot side of the light-system benchmark harness.
# Mirrors meridian_vk_bootstrap's non-interactive benchmark mode:
#   - camera framing replicates build_camera_frame_data() in
#     prototype/src/vk_bootstrap.mm (bounds-derived static camera,
#     55 deg vertical fov, near = 0.01 * radius, far = 8 * radius)
#   - prints a GODOT_BENCHMARK summary line with the same fields as
#     the renderer's MERIDIAN_BENCHMARK line (median/p99/avg ms, avg fps)
#   - default measured-frame count matches the renderer's 120 presents
#     minus its 3 skipped warmup frames

const DEFAULT_FRAMES := 117
const DEFAULT_WARMUP := 30
const ORBIT_FRAMES := 600.0

var asset_path := ""
var scene_id := ""
var out_path := ""
var camera_mode := "static"
var shadows_on := true
var warmup_frames := DEFAULT_WARMUP
var measured_frames := DEFAULT_FRAMES
var frame_index := 0
var last_tick_us := 0
var samples_ms: Array[float] = []
var primitive_samples: Array[int] = []
var mesh_instance_count := 0
var aggregate_aabb := AABB()
var aabb_valid := false
var camera_radius := 1.0
var orbit_center := Vector3.ZERO
var bench_camera: Camera3D


func _ready() -> void:
	var args := _parse_user_args()
	asset_path = args.get("scene", "")
	scene_id = args.get("scene_id", "")
	out_path = args.get("out", "")
	camera_mode = args.get("camera", camera_mode)
	shadows_on = args.get("shadows", "on") == "on"
	warmup_frames = int(args.get("warmup", str(DEFAULT_WARMUP)))
	measured_frames = int(args.get("frames", str(DEFAULT_FRAMES)))

	if asset_path.is_empty():
		push_error("missing --scene=<asset path> user argument")
		get_tree().quit(2)
		return
	if scene_id.is_empty():
		scene_id = asset_path.get_file().get_basename()
	if measured_frames <= 0:
		push_error("measured frame count must be positive")
		get_tree().quit(2)
		return

	DisplayServer.window_set_vsync_mode(DisplayServer.VSYNC_DISABLED)

	_setup_world()
	if not _load_asset():
		get_tree().quit(3)
		return
	_place_camera()
	_setup_shadow_distance()
	last_tick_us = Time.get_ticks_usec()
	set_process(true)


func _process(_delta: float) -> void:
	frame_index += 1
	var now_us := Time.get_ticks_usec()
	var frame_ms := float(now_us - last_tick_us) / 1000.0
	last_tick_us = now_us

	if frame_index > warmup_frames:
		samples_ms.append(frame_ms)
		primitive_samples.append(
			RenderingServer.get_rendering_info(
				RenderingServer.RENDERING_INFO_TOTAL_PRIMITIVES_IN_FRAME))

	if camera_mode == "orbit":
		_update_orbit_camera()

	if frame_index >= warmup_frames + measured_frames:
		_write_results()
		get_tree().quit()


func _parse_user_args() -> Dictionary:
	var parsed := {}
	for arg in OS.get_cmdline_user_args():
		if not arg.contains("="):
			continue
		var parts := arg.split("=", false, 1)
		parsed[parts[0].trim_prefix("--")] = parts[1]
	return parsed


func _setup_world() -> void:
	var sun := DirectionalLight3D.new()
	sun.name = "BenchSun"
	sun.rotation_degrees = Vector3(-45.0, 35.0, 0.0)
	sun.light_energy = 2.0
	sun.shadow_enabled = shadows_on
	add_child(sun)

	var environment := WorldEnvironment.new()
	var env := Environment.new()
	env.background_mode = Environment.BG_COLOR
	env.background_color = Color(0.08, 0.09, 0.11)
	environment.environment = env
	add_child(environment)


func _load_asset() -> bool:
	var target_root: Node3D
	var lower := asset_path.to_lower()
	if lower.ends_with(".glb") or lower.ends_with(".gltf"):
		var doc := GLTFDocument.new()
		var state := GLTFState.new()
		var err := doc.append_from_file(asset_path, state)
		if err != OK:
			push_error("GLTF load failed (%d): %s" % [err, asset_path])
			return false
		target_root = doc.generate_scene(state)
	elif lower.ends_with(".obj"):
		var resource := load(asset_path)
		if resource == null:
			push_error("OBJ load failed (is it imported?): %s" % asset_path)
			return false
		if resource is PackedScene:
			target_root = (resource as PackedScene).instantiate() as Node3D
		elif resource is Mesh:
			var mesh_instance := MeshInstance3D.new()
			mesh_instance.mesh = resource as Mesh
			target_root = Node3D.new()
			target_root.add_child(mesh_instance)
		else:
			push_error("Unsupported OBJ import result: %s" % resource.get_class())
			return false
	else:
		var resource := load(asset_path)
		if resource is PackedScene:
			target_root = (resource as PackedScene).instantiate() as Node3D
		elif resource is Mesh:
			var mesh_instance := MeshInstance3D.new()
			mesh_instance.mesh = resource as Mesh
			target_root = Node3D.new()
			target_root.add_child(mesh_instance)
		else:
			push_error("Unsupported asset type: %s" % asset_path)
			return false

	if target_root == null:
		push_error("Failed to instantiate asset root")
		return false

	add_child(target_root)
	_recompute_bounds(target_root)
	if not aabb_valid:
		push_error("No mesh instances found in asset")
		return false

	return true


func _setup_shadow_distance() -> void:
	var sun := get_node_or_null(^"BenchSun") as DirectionalLight3D
	if sun != null and shadows_on:
		sun.directional_shadow_max_distance = camera_radius * 3.0


func _recompute_bounds(node: Node) -> void:
	if node is MeshInstance3D:
		var mesh_instance := node as MeshInstance3D
		if mesh_instance.mesh != null:
			mesh_instance_count += 1
			_accumulate_mesh_aabb(mesh_instance)
	for child in node.get_children():
		_recompute_bounds(child)


func _accumulate_mesh_aabb(mesh_instance: MeshInstance3D) -> void:
	var local_aabb := mesh_instance.mesh.get_aabb()
	var global_xform := mesh_instance.global_transform
	for corner in _aabb_corners(local_aabb):
		var world_point := global_xform * corner
		if not aabb_valid:
			aggregate_aabb = AABB(world_point, Vector3.ZERO)
			aabb_valid = true
		else:
			aggregate_aabb = aggregate_aabb.expand(world_point)


func _aabb_corners(aabb: AABB) -> Array[Vector3]:
	var min_point := aabb.position
	var max_point := aabb.position + aabb.size
	return [
		Vector3(min_point.x, min_point.y, min_point.z),
		Vector3(max_point.x, min_point.y, min_point.z),
		Vector3(min_point.x, max_point.y, min_point.z),
		Vector3(max_point.x, max_point.y, min_point.z),
		Vector3(min_point.x, min_point.y, max_point.z),
		Vector3(max_point.x, min_point.y, max_point.z),
		Vector3(min_point.x, max_point.y, max_point.z),
		Vector3(max_point.x, max_point.y, max_point.z),
	]


func _place_camera() -> void:
	var center := aggregate_aabb.get_center()
	var extent := aggregate_aabb.size.abs()
	camera_radius = max(max(extent.x, max(extent.y, extent.z)), 1.0)

	bench_camera = Camera3D.new()
	bench_camera.name = "BenchCamera"
	bench_camera.fov = 55.0
	bench_camera.near = max(0.01, camera_radius * 0.01)
	bench_camera.far = camera_radius * 8.0
	bench_camera.position = center + Vector3(
		camera_radius * 0.4, camera_radius * 0.5, camera_radius * 1.5)
	bench_camera.look_at(center, Vector3.UP)
	bench_camera.current = true
	add_child(bench_camera)
	orbit_center = center


func _update_orbit_camera() -> void:
	if bench_camera == null:
		return
	var t := float(max(frame_index - warmup_frames, 0))
	var angle := TAU * t / ORBIT_FRAMES
	var orbit_radius := camera_radius * 1.5
	var height := camera_radius * 0.5
	bench_camera.position = orbit_center + Vector3(
		sin(angle) * orbit_radius, height, cos(angle) * orbit_radius)
	bench_camera.look_at(orbit_center, Vector3.UP)


func _percentile(sorted_values: Array[float], fraction: float) -> float:
	var index := int(sorted_values.size() * fraction)
	index = clampi(index, 0, sorted_values.size() - 1)
	return sorted_values[index]


func _median_of(values: Array[int]) -> int:
	if values.is_empty():
		return 0
	var sorted_values := values.duplicate()
	sorted_values.sort()
	return sorted_values[sorted_values.size() / 2]


func _write_results() -> void:
	if samples_ms.is_empty():
		push_error("No frame samples collected")
		get_tree().quit(4)
		return
	samples_ms.sort()
	var sample_count := samples_ms.size()
	var median := samples_ms[sample_count / 2]
	var p99 := _percentile(samples_ms, 0.99)
	var total := 0.0
	for value in samples_ms:
		total += value
	var avg := total / float(sample_count)
	var avg_fps := 1000.0 / avg

	var window_size := DisplayServer.window_get_size()
	var result := {
		"scene_id": scene_id,
		"asset_path": asset_path,
		"renderer": RenderingServer.get_current_rendering_method(),
		"vsync": DisplayServer.window_get_vsync_mode(),
		"window": [window_size.x, window_size.y],
		"frames": measured_frames,
		"warmup_frames": warmup_frames,
		"camera": camera_mode,
		"shadows": shadows_on,
		"median_ms": median,
		"p99_ms": p99,
		"avg_ms": avg,
		"avg_fps": avg_fps,
		"samples": sample_count,
		"mesh_instances": mesh_instance_count,
		"primitives_in_frame_median": _median_of(primitive_samples),
	}

	if not out_path.is_empty():
		var file := FileAccess.open(out_path, FileAccess.WRITE)
		if file == null:
			push_error("Failed to open output file: %s" % out_path)
		else:
			file.store_string(JSON.stringify(result, "\t"))

	print("scene_id=%s" % scene_id)
	print("renderer=%s" % result["renderer"])
	print("vsync=%d window=%dx%d" % [result["vsync"], window_size.x, window_size.y])
	print("camera=%s shadows=%s" % [camera_mode, str(shadows_on)])
	print("mesh_instances=%d primitives_in_frame=%d" % [
		mesh_instance_count, result["primitives_in_frame_median"]])
	print("GODOT_BENCHMARK: median_ms=%.2f p99_ms=%.2f avg_ms=%.2f avg_fps=%.1f samples=%d" % [
		median, p99, avg, avg_fps, sample_count])
