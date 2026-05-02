#!/usr/bin/env python3

import yaml
import argparse
import re
import shlex
import subprocess
import sys
import os
import time

sys.path.append(os.path.dirname(os.path.dirname(__file__)))


class Trace:
    def __init__(self, name, path, workload, category, subcategory):
        self.name = name
        self.path = path
        self.workload = workload
        self.category = category
        self.subcategory = subcategory


class Experiment:
    def __init__(self, name, params):
        self.name = name
        self.params = params


def load_yaml(file_path):
    with open(file_path, "r") as file:
        return yaml.safe_load(file)


def replace_variables(value, definitions):
    pattern = r"\$\((.*?)\)"
    matches = re.findall(pattern, value)
    for match in matches:
        if match not in definitions:
            sys.exit(f"Encountered undefined variable: {match}")
        value = value.replace(f"$({match})", definitions[match])
    return value


def create_traces(data):
    trace_list = []
    for suite, traces in data.items():
        for entry in traces:
            for name, info in entry.items():
                path = info.get("path")
                workload = info.get("workload")
                category = info.get("category")
                subcategory = info.get("subcategory")
                trace_list.append(Trace(name, path, workload, category, subcategory))
    return trace_list


def create_experiments(data):
    definitions = {
        list(d.keys())[0]: list(d.values())[0] for d in data.get("definitions", [])
    }
    experiments = []

    for exp in data.get("experiments", []):
        for name, params in exp.items():
            params = replace_variables(params, definitions)

            experiments.append(Experiment(name, params))

    return experiments


def merge_with_dedup(named_groups, kind, source_flag):
    """Concatenate items from multiple files, aborting if a name appears more than once across all inputs."""
    name_to_files = {}
    for path, items in named_groups:
        for item in items:
            name_to_files.setdefault(item.name, []).append(path)
    dups = {name: files for name, files in name_to_files.items() if len(files) > 1}
    if dups:
        lines = [f"Duplicate {kind} name(s) found across {source_flag} files. Please deconflict:"]
        for name, files in sorted(dups.items()):
            lines.append(f"  {name}: appears in {', '.join(files)}")
        sys.exit("\n".join(lines))
    return [item for _, items in named_groups for item in items]


JOBFILE_PREAMBLE_SLURM = (
    "#!/bin/bash\n"
    "#\n"
    "# This is a jobfile that contains commands to run via slurm.\n"
    "# To launch the jobs, simply source the file as\n"
    "#   source ./<filename>\n"
    "#"
)

JOBFILE_PREAMBLE_LOCAL = (
    "#!/bin/bash\n"
    "#\n"
    "# This is a jobfile that runs ChampSim commands locally on this host.\n"
    "# To launch, simply source the file as\n"
    "#   source ./<filename>\n"
    "# Per-command stdout/stderr are redirected to <trace>_<exp>.out / .err.\n"
    "#"
)


def main():
    parser = argparse.ArgumentParser(
        usage="%(prog)s --exe <executable> --exp <exp file> --tlist <trace list>"
    )
    parser.add_argument('--exe', required=True, help='Executable')
    parser.add_argument("--tlist", required=True, nargs='+', help="Path(s) to one or more trace list YAML files. Traces are concatenated; duplicate trace names across files cause an error.")
    parser.add_argument("--exp", required=True, nargs='+', help="Path(s) to one or more experiment YAML files. Definitions are scoped per file (self-contained); experiments are concatenated; duplicate experiment names across files cause an error.")
    parser.add_argument("--slurm-part", required=False, default="compute", help="Slurm partition to run on")
    parser.add_argument('--ncores', default='1', help='Number of cores needed for each slurm job')
    parser.add_argument('--exclude', dest='exclude_list', default=None, help='Node exclude list')
    parser.add_argument('--include', dest='include_list', default=None, help='Node include list')
    parser.add_argument('--nodename', default="ntl-zeus", help='Machine name of the compute nodes')
    parser.add_argument('--extra', default=None, help='Extra slurm arguments')
    parser.add_argument('--output', '-o', default='jobfile.sh', help='Output jobfile path (default: jobfile.sh in CWD)')
    parser.add_argument('--smoke-test', action='store_true', help='After writing the jobfile, run one ChampSim command locally with reduced warmup/sim instructions to verify correctness')
    parser.add_argument('--smoke-test-idx', type=int, default=0, help='Index into the (trace x experiment) pair list to use for --smoke-test (default: 0)')
    parser.add_argument('--smoke-warmup', type=int, default=1_000_000, help='Warmup instructions used during --smoke-test (default: 1M)')
    parser.add_argument('--smoke-sim', type=int, default=1_000_000, help='Simulation instructions used during --smoke-test (default: 1M)')
    parser.add_argument('--local', action='store_true', help='Emit raw ChampSim commands instead of sbatch lines, so the jobfile runs locally on this host')
    parser.add_argument('--local-parallel', type=int, default=1, help='Max number of local commands to run in parallel when --local is set (default: 1)')
    args = parser.parse_args()

    if args.local_parallel < 1:
        sys.exit(f"--local-parallel must be >= 1 (got {args.local_parallel})")

    trace_groups = [(p, create_traces(load_yaml(p))) for p in args.tlist]
    exp_groups = [(p, create_experiments(load_yaml(p))) for p in args.exp]

    traces = merge_with_dedup(trace_groups, "trace", "--tlist")
    experiments = merge_with_dedup(exp_groups, "experiment", "--exp")

    exclude_nodes_list = f"{args.nodename}[{args.exclude_list}]" if args.exclude_list else ""
    include_nodes_list = f"{args.nodename}[{args.include_list}]" if args.include_list else ""

    slurm_preamble = f"sbatch -p {args.slurm_part} --mincpus=1 -c {args.ncores}"
    if args.include_list:
        slurm_preamble += f" --nodelist={include_nodes_list}"
    if args.exclude_list:
        slurm_preamble += f" --exclude={exclude_nodes_list}"
    if args.extra:
        slurm_preamble += f" {args.extra}"

    preamble = JOBFILE_PREAMBLE_LOCAL if args.local else JOBFILE_PREAMBLE_SLURM

    with open(args.output, "w") as out:
        print(preamble, file=out)
        print("# Traces:", file=out)
        for trace in traces:
            print("#\t{}".format(trace.name), file=out)
        print("#\n#\n#", file=out)
        print("# Experiments:", file=out)
        for exp in experiments:
            print("#\t{}: params={}".format(exp.name, exp.params), file=out)
        print("#\n#\n#", file=out)

        if args.local:
            print(f"\nMAX_PARALLEL={args.local_parallel}\n", file=out)

        for trace in traces:
            for exp in experiments:
                tag = f"{trace.name}_{exp.name}"
                inner = f"{args.exe} {exp.params} {trace.name} -traces {trace.path}"

                if args.local:
                    print(
                        f'(echo "[run] {tag}"; {inner} > {tag}.out 2> {tag}.err) &\n'
                        f'[ "$(jobs -rp | wc -l)" -ge "$MAX_PARALLEL" ] && wait -n',
                        file=out,
                    )
                else:
                    slurm_cmd = (
                        f"{slurm_preamble}"
                        f" -J {tag}"
                        f" -o {tag}.out"
                        f" -e {tag}.err"
                    )
                    print(f"{slurm_cmd} --wrap={shlex.quote(inner)}", file=out)

        if args.local:
            print("\nwait", file=out)

    print(f"Wrote jobfile to {args.output}", file=sys.stderr)

    if args.smoke_test:
        pairs = [(t, e) for t in traces for e in experiments]
        if not pairs:
            sys.exit("--smoke-test: no (trace, experiment) pairs to run.")
        if not (0 <= args.smoke_test_idx < len(pairs)):
            sys.exit(f"--smoke-test-idx {args.smoke_test_idx} out of range [0, {len(pairs)})")
        trace, exp = pairs[args.smoke_test_idx]
        inner = (
            f"{args.exe} {exp.params} {trace.name} -traces {trace.path}"
            f" --warmup_instructions={args.smoke_warmup}"
            f" --simulation_instructions={args.smoke_sim}"
        )
        print(f"[smoke-test] pair #{args.smoke_test_idx}: trace={trace.name}, exp={exp.name}", file=sys.stderr)
        print(f"[smoke-test] command: {inner}", file=sys.stderr)
        start = time.monotonic()
        rc = subprocess.run(shlex.split(inner)).returncode
        elapsed = time.monotonic() - start
        status = "PASSED" if rc == 0 else "FAILED"
        print(f"[smoke-test] {status} (exit={rc}, elapsed={elapsed:.1f}s)", file=sys.stderr)
        sys.exit(rc)


if __name__ == "__main__":
    main()
