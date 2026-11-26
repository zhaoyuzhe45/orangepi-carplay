from pathlib import Path
root = Path(__file__).resolve().parents[1]
source = (root / "src/iap2d.c").read_text()
bench = source[source.index("if (bench_endpoint[0])"):source.index("if (host_mode)", source.index("if (bench_endpoint[0])"))]
assert "begin_vehicle_session();" in bench, "bench needs the real vehicle-state binding before connection"
assert bench.index("begin_vehicle_session();") < bench.index("ncm_bridge_bench_start("), "bind before AirPlay client captures generation"
assert 'publish_control_status("carplay", 0);' in bench, "bench must publish current received/applied state"
assert "carplay_vehicle_state_end_session" in bench, "bench stop invalidates its vehicle generation"
print("PASS explicit AirPlay bench vehicle lifetime")
