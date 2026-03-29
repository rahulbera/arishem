#!/usr/bin/env python3

import argparse
import os
import sys

import trace
import exp


def main():
    parser = argparse.ArgumentParser(
        usage="%(prog)s --exe <executable> --exp <exp file> --tlist <trace list>"
    )
    parser.add_argument('--tlist', required=True, help='Trace list file')
    parser.add_argument('--exp', required=True, help='Experiment file')
    parser.add_argument('--exe', required=True, help='Executable')
    parser.add_argument('--ncores', default='1', help='Number of cores')
    parser.add_argument('--local', default='0', help='Run locally (1) or via sbatch (0)')
    parser.add_argument('--exclude', dest='exclude_list', default=None, help='Node exclude list')
    parser.add_argument('--include', dest='include_list', default=None, help='Node include list')
    parser.add_argument('--partition', default='cpu_part', help='Slurm partition')
    parser.add_argument('--extra', default=None, help='Extra sbatch arguments')
    args = parser.parse_args()

    pythia_home = os.environ.get('PYTHIA_HOME')
    if not pythia_home:
        sys.exit("$PYTHIA_HOME env variable is not defined.\nHave you sourced setvars.sh?")

    ncores = args.ncores
    if ncores == '0':
        sys.exit("have to supply -ncores")

    local = args.local
    exe = args.exe
    slurm_partition = args.partition
    exclude_list = args.exclude_list
    include_list = args.include_list
    extra = args.extra

    exclude_nodes_list = f"kratos[{exclude_list}]" if exclude_list else ""
    include_nodes_list = f"kratos[{include_list}]" if include_list else ""

    trace_info = trace.parse(args.tlist)
    exp_info = exp.parse(args.exp)

    # Preamble
    if local == '0':
        print("#!/bin/bash -l")
    else:
        print("#!/bin/bash")
    print("#")
    print("#")
    print("#")
    print("# Traces:")
    for t in trace_info:
        print(f"#    {t['NAME']}")
    print("#")
    print("#")
    print("# Experiments:")
    for e in exp_info:
        print(f"#    {e['NAME']}: {e['KNOBS']}")
    print("#")
    print("#")
    print("#")
    print("#")

    for t in trace_info:
        for e in exp_info:
            exp_name = e['NAME']
            exp_knobs = e['KNOBS']
            trace_name = t['NAME']
            trace_input = t['TRACE']
            trace_knobs = t.get('KNOBS', '')

            if local != '0':
                cmdline = (
                    f"{exe} {exp_knobs} {trace_knobs} -traces {trace_input} "
                    f"> {trace_name}_{exp_name}.out 2>&1"
                )
            else:
                slurm_cmd = f"sbatch -p {slurm_partition} --mincpus=1"
                if include_list:
                    slurm_cmd += f" --nodelist={include_nodes_list}"
                if exclude_list:
                    slurm_cmd += f" --exclude={exclude_nodes_list}"
                if extra:
                    slurm_cmd += f" {extra}"
                slurm_cmd += (
                    f" -c {ncores} -J {trace_name}_{exp_name}"
                    f" -o {trace_name}_{exp_name}.out"
                    f" -e {trace_name}_{exp_name}.err"
                )
                cmdline = (
                    f"{slurm_cmd} {pythia_home}/wrapper.sh {exe}"
                    f' "{exp_knobs} {trace_knobs} -traces {trace_input}"'
                )

            # Additional hook replacements
            cmdline = cmdline.replace('$(PYTHIA_HOME)', pythia_home)
            cmdline = cmdline.replace('$(EXP)', exp_name)
            cmdline = cmdline.replace('$(TRACE)', trace_name)
            cmdline = cmdline.replace('$(NCORES)', str(ncores))

            print(cmdline)


if __name__ == '__main__':
    main()
