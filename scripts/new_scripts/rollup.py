#!/usr/bin/env python3

import yaml
import toml
import argparse
import re
import os
import csv
from multiprocessing import Pool


class Trace:
    def __init__(self, name, path, workload, category, subcategory):
        self.name = name
        self.path = path
        self.workload = workload
        self.category = category
        self.subcategory = subcategory


class Experiment:
    def __init__(self, name, params, pintool_args, scarab_args):
        self.name = name
        self.params = params
        self.pintool_args = pintool_args
        self.scarab_args = scarab_args


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
        for name, details in exp.items():
            params = details.get("params", "")
            pintool_args = details.get("pintool_args", "")
            scarab_args = details.get("scarab_args", "")

            params = replace_variables(params, definitions)
            pintool_args = replace_variables(pintool_args, definitions)
            scarab_args = replace_variables(scarab_args, definitions)

            experiments.append(Experiment(name, params, pintool_args, scarab_args))

    return experiments


def evaluate_expression(expression, values):
    # Safely evaluate the mathematical expression
    for k, v in values.items():
        expression = expression.replace(f"$({k})", str(v))

    # Create a safe environment for evaluation
    allowed_names = {"__builtins__": {}}
    safe_functions = {"abs": abs, "min": min, "max": max, "round": round}
    allowed_names.update(safe_functions)
    
    try:
        result = eval(expression, allowed_names)
        if isinstance(result, float):
            # Limit precision to 6 digits for fractional values
            result = round(result, 6)
        return result
    except ZeroDivisionError:
        return float('inf')  # Handle division by zero gracefully
    except Exception as e:
        raise ValueError(f"Error evaluating expression '{expression}': {e}")


def extract_and_compute(mfile, stats):
    results = []
    for param in mfile:
        for key, value in param.items():
            target = value["target"]
            if target.startswith("$(") and target.endswith(")"):
                # Find all expressions in the form of $(...) in the target
                keys = re.findall(r"\$\((.*?)\)", target)

                # Create a dictionary to hold extracted values
                values = {}
                for key_path in keys:
                    section, param_name = key_path.split(".")
                    try:
                        # Extract the value from the stats dictionary
                        values[key_path] = stats[section][param_name]
                    except KeyError:
                        print(
                            f"Error: The target '{target}' is not valid in the stats TOML file."
                        )
                        return None

                # Evaluate the expression
                result = evaluate_expression(target, values)
                results.append(result)

    return results


def process_trace(trace, experiments, mfile):
    output_data = []
    error_occurred = False  # Flag to indicate if any experiment has an error

    # Process each experiment for this trace
    for exp in experiments:
        # Construct the stat file and stderr file names
        stat_file_name = os.path.join(os.getcwd(), f"{trace}.{exp}/statdb.0.toml")
        stderr_file_name = os.path.join(os.getcwd(), f"{trace}.{exp}.err")

        # Initialize all results as zeros
        result_values = [0] * len(
            mfile
        )  # Assuming the number of metrics is equal to mfile length

        filter_value = 1  # Default to 1 (no error)

        # Check if the stat file exists
        if not os.path.isfile(stat_file_name):
            filter_value = 0  # Set Filter to 0 since the stat file is missing
            error_occurred = True
            output_data.append([trace, exp, filter_value] + result_values)
            continue

        # Check if the stderr file exists and is not empty
        if os.path.isfile(stderr_file_name) and os.path.getsize(stderr_file_name) > 0:
            filter_value = 0  # Set to 0 if stderr file is not empty
            error_occurred = True  # Set flag if there's an error

        # Load the stats from the TOML file
        with open(stat_file_name, "r") as toml_file:
            stats = toml.load(toml_file)

        # Extract metrics for this trace and experiment
        extracted_results = extract_and_compute(mfile, stats)
        if extracted_results is not None:
            output_data.append([trace, exp, filter_value] + extracted_results)

    # If any experiment in the trace had an error, set all to filter value 0
    if error_occurred:
        for row in output_data:
            row[2] = 0  # Set the filter value to 0 for all experiments of this trace

    return output_data


def main():
    parser = argparse.ArgumentParser(description="Process YAML and TOML files.")
    parser.add_argument(
        "--mfile", type=str, required=True, help="The path to the YAML file"
    )
    parser.add_argument(
        "--tlist", required=True, help="Path to the input trace YAML file"
    )
    parser.add_argument(
        "--exp", required=True, help="Path to the input experiment YAML file"
    )
    parser.add_argument(
        "--threads", type=int, default=1, help="Number of threads to use"
    )
    parser.add_argument(
        "-o", "--output", type=str, default="stats.txt", help="Stats output file name"
    )

    args = parser.parse_args()

    trace_data = load_yaml(args.tlist)
    exp_data = load_yaml(args.exp)

    traces = create_traces(trace_data)
    experiments = create_experiments(exp_data)

    trace_names = [trace.name for trace in traces]
    exp_names = [exp.name for exp in experiments]

    with open(args.mfile, "r") as yaml_file:
        mfile = yaml.safe_load(yaml_file)

    # Prepare to use multiprocessing
    with Pool(processes=args.threads) as pool:
        results = pool.starmap(
            process_trace, [(trace, exp_names, mfile) for trace in trace_names]
        )

    # Combine results from all traces
    output_data = [item for sublist in results for item in sublist]

    # Write results to CSV
    with open(args.output, mode="w", newline="") as csv_file:
        writer = csv.writer(csv_file)
        # Write header
        writer.writerow(
            ["Trace", "Experiment", "Filter"] + [key for metric in mfile for key in metric.keys()]
        )
        # Write data rows
        writer.writerows(output_data)

    print(f"Results written to '{args.output}'.")


if __name__ == "__main__":
    main()
