import re

def parse(filename):
    """Parse an experiment file and return a list of experiment dicts."""
    with open(filename, 'r') as f:
        lines = [line.strip() for line in f]

    exp_configs = {}
    exps = []
    for elem in lines:
        if elem == "" or elem.startswith('#'):
            continue

        tokens = elem.split()
        if len(tokens) >= 2 and tokens[1] == '=':
            # Config variable assignment
            exp_configs[tokens[0]] = ' '.join(tokens[2:])
        else:
            # Experiment declaration
            name = tokens[0]
            args = []
            for token in tokens[1:]:
                if token.startswith('$'):
                    var = re.sub(r'[\$\(\)]', '', token)
                    if var in exp_configs:
                        args.append(exp_configs[var])
                    else:
                        raise SystemExit(f"{var} is not defined before exp {name}")
                else:
                    args.append(token)
            exps.append({"NAME": name, "KNOBS": ' '.join(args)})

    return exps
