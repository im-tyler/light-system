extends Node3D

const DEFAULT_FRAMES := 240
const DEFAULT_WARMUP := 30
const DEFAULT_ASSET := "res://benchmarks/scenes/generated/gltf_block_scene.gltf"

var asset_path: String = ""
var scene_id: String = ""
var output_path: String = ""
var benchmark_mode: String = "stock_godot"
var warmup_frames: int = DEFAULT_WARMUP
var measured_frames: int = DEFAULT_FRAMES
var frame_index: int = 0
var accumulated_delta: float = 0.0
var target_root: Node3D
var mesh_instance_count: int = 0
var aabb_valid: bool = false
var aggregate_min := Vector3.ZERO
var aggregate_max := Vector3.ZERO


func _ready() -> void:
	var args := _parse_user_args()
	asset_path = args.get("asset", DEFAULT_ASSET)
	scene_id = args.get("scene_id", asset_path.get_file().get_basename())
	output_path = args.get("output", "")
	benchmark_mode = args.get("mode", benchmark_mode)
	warmup_frames = int(args.get("warmup", str(DEFAULT_WARMUP)))
	measured_frames = int(args.get("frames", str(DEFAULT_FRAMES)))

	if measured_frames <= 0:
		push_error("Measured frame count must be positive")
		get_tree().quit(2)
		return

	_setup_world()
	if not _load_asset():
		get_tree().quit(3)
		return

	_place_camera()
	set_process(true)


func _process(delta: float) -> void:
	frame_index += 1
	if frame_index > warmup_frames:
		accumulated_delta += delta

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
	sun.rotation_degrees = Vector3(-45.0, 35.0, 0.0)
	sun.light_energy = 2.0
	add_child(sun)

	var environment := WorldEnvironment.new()
	var env := Environment.new()
	env.background_mode = Environment.BG_COLOR
	env.background_color = Color(0.08, 0.09, 0.11)
	environment.environment = env
	add_child(environment)


func _load_asset() -> bool:
	var resource := load(asset_path)
	if resource == null:
		push_error("Failed to load asset: %s" % asset_path)
		return false

	if resource is PackedScene:
		target_root = (resource as PackedScene).instantiate() as Node3D
	elif resource is Mesh:
		var mesh_instance := MeshInstance3D.new()
		mesh_instance.mesh = resource as Mesh
		target_root = Node3D.new()
		target_root.add_child(mesh_instance)
	else:
		push_error("Unsupported baseline asset type: %s" % [resource.get_class()])
		return false

	if target_root == null:
		push_error("Failed to instantiate asset root")
		return false

	add_child(target_root)
	_recompute_scene_bounds(target_root)
	return true


func _recompute_scene_bounds(node: Node) -> void:
	if node is MeshInstance3D:
		var mesh_instance := node as MeshInstance3D
		if mesh_instance.mesh != null:
			mesh_instance_count += 1
			_accumulate_mesh_aabb(mesh_instance)

	for child in node.get_children():
		_recompute_scene_bounds(child)


func _accumulate_mesh_aabb(mesh_instance: MeshInstance3D) -> void:
	var local_aabb := mesh_instance.mesh.get_aabb()
	var global_xform := mesh_instance.global_transform
	for corner in _aabb_corners(local_aabb):
		var world_point := global_xform * corner
		if not aabb_valid:
			aggregate_min = world_point
			aggregate_max = world_point
			aabb_valid = true
		else:
			aggregate_min = aggregate_min.min(world_point)
			aggregate_max = aggregate_max.max(world_point)


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
	var camera := Camera3D.new()
	camera.current = true
	add_child(camera)

	var center := Vector3.ZERO
	var extent := Vector3.ONE
	if aabb_valid:
		center = (aggregate_min + aggregate_max) * 0.5
		extent = (aggregate_max - aggregate_min).abs()

	var radius: float = max(max(extent.x, extent.y), extent.z)
	radius = max(radius, 1.0)
	camera.position = center + Vector3(radius * 0.2, radius * 0.85, radius * 1.8)
	camera.look_at(center, Vector3.UP)


func _write_results() -> void:
	var measured_delta: float = max(accumulated_delta, 0.000001)
	var avg_frame_ms: float = (measured_delta / float(measured_frames)) * 1000.0
	var avg_fps: float = float(measured_frames) / measured_delta

	var result := {
		"scene_id": scene_id,
		"asset_path": asset_path,
		"mode": benchmark_mode,
		"frames": measured_frames,
		"warmup_frames": warmup_frames,
		"avg_frame_ms": avg_frame_ms,
		"avg_fps": avg_fps,
		"mesh_instance_count": mesh_instance_count,
		"bounds_min": [aggregate_min.x, aggregate_min.y, aggregate_min.z] if aabb_valid else [],
		"bounds_max": [aggregate_max.x, aggregate_max.y, aggregate_max.z] if aabb_valid else [],
		"renderer": RenderingServer.get_current_rendering_method(),
	}

	if not output_path.is_empty():
		var file := FileAccess.open(output_path, FileAccess.WRITE)
		if file == null:
			push_error("Failed to open output file: %s" % output_path)
		else:
			file.store_string(JSON.stringify(result, "  "))

	print("scene_id=%s" % scene_id)
	print("avg_frame_ms=%f" % avg_frame_ms)
	print("avg_fps=%f" % avg_fps)
	print("mesh_instances=%d" % mesh_instance_count)
