#!/usr/bin/env python3
"""Turn original AST expectations into portable cases and check the C executable."""
import json
from pathlib import Path
import subprocess
import sys

PORT = Path(__file__).resolve().parents[1]
CORPORA = ('corpus.jsonl', 'extra.jsonl', 'notebooks.jsonl')


def show(term):
    if term is None:
        return '{}'
    if term['k'] == 'Call':
        if term['h'] is None:
            return '{}'
        return '{' + ' '.join([show(term['h'])] + [show(a) for a in term['a']]) + '}'
    if term['k'] == 'Sym':
        return term['v']
    return json.dumps(term['v'], ensure_ascii=True)


def rules(definitions):
    result = []
    for rule in definitions:
        parts = ['R', json.dumps(rule['name']), show(rule['lhs']), show(rule['rhs'])]
        for field, tag in [('guard', ':guard'), ('scope', ':scope'),
                           ('withPattern', ':with'), ('letPattern', ':let')]:
            if rule.get(field) is not None:
                parts += [tag, json.dumps(rule[field]) if field == 'scope' else show(rule[field])]
        if rule.get('prio'):
            parts += [':prio', str(rule['prio'])]
        if rule.get('innermost'):
            parts += [':innermost']
        result.append('{' + ' '.join(parts) + '}')
    return '{Rules ' + ' '.join(result) + '}'


def corpus():
    lines = []
    for name in CORPORA:
        for line in (PORT / 'tests' / name).read_text().splitlines():
            case = json.loads(line)
            parts = ['Case', rules(case['rules']), show(case['expr']), str(case['maxsteps']),
                     'True' if case['skipprims'] or not case['fold'] else 'False',
                     'True' if case['preserve'] else 'False',
                     'Error' if 'error' in case else 'Result', show(case.get('expected')),
                     'True' if case.get('meta') else 'False', show(case.get('universe'))]
            lines.append('{' + ' '.join(parts) + '}')
    output = PORT / 'tests/corpus.syma'
    output.write_text('\n'.join(lines) + '\n')
    return output


if __name__ == '__main__':
    path = corpus()
    if len(sys.argv) > 1:
        sys.exit(subprocess.run([sys.argv[1], '--check', str(path)]).returncode)
