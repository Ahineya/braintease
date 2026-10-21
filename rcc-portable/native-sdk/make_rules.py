"""Bounded header lists for the guest Make's 2048-cell macro pushback.

Small stamp groups retain complete timestamp dependencies without expanding
hundreds of paths through one macro or a recipe's implicit variables.
"""


def header_rules(headers, directory):
    # A real file, rather than the directory's mtime, stays unchanged when
    # other stamps are created. This avoids spawning mkdir once per group
    # without making every subsequent build look out of date.
    created = directory + '/directory.stamp'
    rules = [created + ':', '\tmkdir -p "' + directory + '"',
             '\t@echo directory > "$@"']
    stamps = []
    continuation = ' \\' + '\n  '
    for at in range(0, len(headers), 12):
        stamp = directory + '/' + str(at // 12) + '.stamp'
        stamps.append(stamp)
        rules += [stamp + ': ' + continuation.join([created] + headers[at:at + 12]),
                  '\t@echo headers > "$@"']
    ready = directory + '/ready'
    rules += [ready + ': ' + ' '.join([created] + stamps), '\t@echo headers > "$@"']
    return ready, rules, [created] + stamps + [ready]
