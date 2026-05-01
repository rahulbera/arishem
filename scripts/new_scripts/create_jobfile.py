#!/usr/bin/env python3

import yaml
import argparse
import re
import sys
import os

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


JOBFILE_PREAMBLE = (
    "#!/bin/bash\n"
    "#\n"
    "# This is a jobfile that contains commands to run via slurm.\n"
    "# To launch the jobs, simple source the file as\n"
    "# source ./<filename>\n"
    "# \n"
    "#"
)


def main():
    parser = argparse.ArgumentParser(
        usage="%(prog)s --exe <executable> --exp <exp file> --tlist <trace list>"
    )
    parser.add_argument('--exe', required=True, help='Executable')
    parser.add_argument("--tlist", required=True, help="Path to the input trace YAML file")
    parser.add_argument("--exp", required=True, help="Path to the input experiment YAML file")
    parser.add_argument("--slurm_part", required=False, default="compute", help="Slurm partition to run on")
    parser.add_argument('--ncores', default='1', help='Number of cores needed for each slurm job')
    parser.add_argument('--exclude', dest='exclude_list', default=None, help='Node exclude list')
    parser.add_argument('--include', dest='include_list', default=None, help='Node include list')
    parser.add_argument('--nodename', default="ntl-zeus", help='Machine name of the compute nodes')
    parser.add_argument('--extra', default=None, help='Extra slurm arguments')
    args = parser.parse_args()

    pythia_home = os.environ.get('PYTHIA_HOME')
    if not pythia_home:
        sys.exit("$PYTHIA_HOME env variable is not defined.\nHave you sourced setvars.sh?")

    trace_data = load_yaml(args.tlist)
    exp_data = load_yaml(args.exp)

    traces = create_traces(trace_data)
    experiments = create_experiments(exp_data)

    exclude_nodes_list = f"{args.nodename}[{args.exclude_list}]" if args.exclude_list else ""
    include_nodes_list = f"{args.nodename}[{args.include_list}]" if args.include_list else ""

    print(JOBFILE_PREAMBLE)
    print("# Traces:")
    for trace in traces:
        print("#\t{}".format(trace.name))
    print("#\n#\n#")
    print("# Experiments:")
    for exp in experiments:
        print(
            "#\t{}: params={}".format(
                exp.name, exp.params
            )
        )
    print("#\n#\n#")

    slurm_preamble = f"sbatch -p {args.slurm_part} --mincpus=1 -c {args.ncores}"
    if args.include_list:
        slurm_preamble += f" --nodelist={include_nodes_list}"
    if args.exclude_list:
        slurm_preamble += f" --exclude={exclude_nodes_list}"
    if args.extra:
        slurm_preamble += f" {args.extra}"
    
    for trace in traces:
        for exp in experiments:
            slurm_cmd = slurm_preamble
            slurm_cmd += (
                f" -J {trace.name}_{exp.name}"
                f" -o {trace.name}_{exp.name}.out"
                f" -e {trace.name}_{exp.name}.err"
            )
            
            cmd = (
                f"{slurm_cmd} {pythia_home}/wrapper.sh {args.exe}"
                f' "{exp.params} {trace.name} -traces {trace.path}"'
            )

            print(cmd)


if __name__ == "__main__":
    main()
