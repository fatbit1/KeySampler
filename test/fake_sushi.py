"""Stand-in for sushi's gRPC control API, built from the sushi_rpc descriptor
extracted from the DSP Card firmware. Lists processors, serves KeySampler's
parameter list (from its TTL) and logs SetParameterValue calls.

usage: python3 fake_sushi.py <sushi_rpc.desc> <keysampler.ttl> <port> <log> [two]
  "two" = also list a second KeySampler (id 9, different Slot) to test
  instance identification.
"""
import re, sys, time
from concurrent import futures
import grpc
from google.protobuf import descriptor_pb2, descriptor_pool, message_factory

desc_path, ttl_path, port, log_path = sys.argv[1:5]
two = len(sys.argv) > 5 and sys.argv[5] == "two"
fdp = descriptor_pb2.FileDescriptorProto()
fdp.ParseFromString(open(desc_path, "rb").read())
pool = descriptor_pool.DescriptorPool()
pool.Add(fdp)
M = lambda n: message_factory.GetMessageClass(pool.FindMessageTypeByName("sushi_rpc." + n))

ttl = open(ttl_path).read()
params = []
for blk in re.findall(r'\[\n\s+a lv2:InputPort, lv2:ControlPort ;(.*?)\n    \]', ttl, re.S):
    g = lambda k: re.search(k + r' ([^;]+) ;', blk).group(1).strip().strip('"')
    params.append(dict(sym=g("lv2:symbol"), name=g("lv2:name"), d=float(g("lv2:default")),
                       lo=float(g("lv2:minimum")), hi=float(g("lv2:maximum"))))
norm = lambda p, v: (v - p["lo"]) / (p["hi"] - p["lo"])
values = {7: [norm(p, p["d"]) for p in params], 9: [norm(p, p["d"]) for p in params]}
values[9][[p["sym"] for p in params].index("slot")] = norm(params[[p["sym"] for p in params].index("slot")], 5)  # other instance: slot 5
log = open(log_path, "w", buffering=1)

def all_processors(req, ctx):
    r = M("ProcessorInfoList")()
    r.processors.add(id=3, label="mda Delay", name="mda_delay_0", parameter_count=6)
    r.processors.add(id=7, label="KeySampler", name="KeySampler_1", parameter_count=len(params))
    if two:
        r.processors.add(id=9, label="KeySampler", name="KeySampler_2", parameter_count=len(params))
    log.write("GetAllProcessors\n")
    return r

def proc_params(req, ctx):
    r = M("ParameterInfoList")()
    for i, p in enumerate(params):
        r.parameters.add(id=i, label=p["name"], name=p["sym"], min_domain_value=p["lo"], max_domain_value=p["hi"])
    return r

def get_value(req, ctx):
    return M("GenericFloatValue")(value=values.get(req.processor_id, [0] * len(params))[req.parameter_id])

def set_value(req, ctx):
    pid, proc, v = req.parameter.parameter_id, req.parameter.processor_id, req.value
    p = params[pid]
    values.setdefault(proc, [0] * len(params))[pid] = v
    log.write(f"Set proc={proc} {p['sym']} norm={v:.4f} domain={p['lo'] + v * (p['hi'] - p['lo']):.3f} t={time.time():.3f}\n")
    return M("GenericVoidValue")()

def h(fn, req):
    return grpc.unary_unary_rpc_method_handler(fn, request_deserializer=M(req).FromString,
                                               response_serializer=lambda m: m.SerializeToString())

server = grpc.server(futures.ThreadPoolExecutor(4))
server.add_generic_rpc_handlers((
    grpc.method_handlers_generic_handler("sushi_rpc.AudioGraphController", {"GetAllProcessors": h(all_processors, "GenericVoidValue")}),
    grpc.method_handlers_generic_handler("sushi_rpc.ParameterController", {
        "GetProcessorParameters": h(proc_params, "ProcessorIdentifier"),
        "GetParameterValue": h(get_value, "ParameterIdentifier"),
        "SetParameterValue": h(set_value, "ParameterValue")}),
))
server.add_insecure_port(f"127.0.0.1:{port}")
server.start()
print("fake sushi listening on", port, flush=True)
server.wait_for_termination()
