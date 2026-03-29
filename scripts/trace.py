def parse(filename):
    """Parse a trace list file and return a list of trace dicts."""
    with open(filename, 'r') as f:
        lines = [line.rstrip('\n') for line in f]

    trace_info = []
    rec = None
    for elem in lines:
        if elem != "":
            idx = elem.index('=')
            key = elem[:idx]
            value = elem[idx + 1:]
            if key == "NAME":
                if rec is not None:
                    trace_info.append(rec)
                rec = {}
            if rec is None:
                rec = {}
            rec[key] = value
    if rec is not None:
        trace_info.append(rec)

    return trace_info
