"""Translate preprocessed NetHack K&R definitions into C99 parameter lists.

Only function headers change; bodies and the guest's types are left intact.
Clang is used solely as a preprocessor, never as the guest C compiler.
"""
import re


def split_commas(text):
    parts, start, depth = [], 0, 0
    for i, ch in enumerate(text):
        if ch in '([': depth += 1
        elif ch in ')]': depth -= 1
        elif ch == ',' and not depth:
            parts.append(text[start:i].strip()); start = i + 1
    parts.append(text[start:].strip())
    return parts


def normalize(text):
    pattern = re.compile(r'\b([A-Za-z_]\w*)\s*\(\s*([A-Za-z_]\w*(?:\s*,\s*[A-Za-z_]\w*)*)\s*\)\s*\n([^{}]*?;)\s*\{')
    def replace(match):
        name, names, declarations = match.groups()
        if name in ['if','while','for','switch','return','sizeof']: return match[0]
        names = [n.strip() for n in names.split(',')]
        parameters = {}
        for declaration in declarations.split(';'):
            if not declaration.strip(): continue
            pieces = split_commas(declaration)
            hits = list(re.finditer(r'\b(' + '|'.join(map(re.escape,names)) + r')\b', pieces[0]))
            hits = [h for h in hits if not re.search(r'\b(?:struct|union|enum)\s*$',pieces[0][:h.start()])]
            found = hits[0] if hits else None
            if not found:
                raise ValueError(f'{name}: unrecognized parameter declaration {declaration!r}')
            prefix = pieces[0][:found.start()]
            base = re.sub(r'[\s*(]+$', '', prefix).strip()
            # Pull pointer/function declarator punctuation out of the base type.
            first = pieces[0][len(base):].strip()
            for piece in [first, *pieces[1:]]:
                hits = list(re.finditer(r'\b(' + '|'.join(map(re.escape,names)) + r')\b', piece))
                hits = [h for h in hits if not re.search(r'\b(?:struct|union|enum)\s*$',piece[:h.start()])]
                hit = hits[0] if hits else None
                if not hit: raise ValueError(f'{name}: missing parameter in {piece!r}')
                parameters[hit[1]] = base + ' ' + piece
        if set(parameters) != set(names):
            raise ValueError(f'{name}: parameter mismatch {names!r} / {parameters!r}')
        return name + '(' + ', '.join(parameters[n] for n in names) + ')\n{'
    return pattern.sub(replace, text)


def cast_opvars(text):
    """Honor lev_comp's variadic format on hosts where int and long differ.

    Upstream grammar actions pass a mix of long-valued parser fields and int
    constants to 'i'. They happen to share argument slots on common hosts.
    C16 packs arguments by their actual width, so make that contract explicit.
    """
    edits = []
    for match in re.finditer(r'\badd_opvars\s*\(',text):
        start = match.end()
        depth, quote, escaped, parts, part = 1, None, False, [], start
        for i in range(start,len(text)):
            ch = text[i]
            if quote:
                if escaped: escaped = False
                elif ch == '\\': escaped = True
                elif ch == quote: quote = None
                continue
            if ch in "\"'": quote = ch; continue
            if ch in '([': depth += 1
            elif ch in ')]':
                depth -= 1
                if depth == 0:
                    parts.append(text[part:i].strip()); end = i; break
            elif ch == ',' and depth == 1:
                parts.append(text[part:i].strip()); part = i + 1
        if len(parts) < 2 or not re.fullmatch(r'"[ilcrmMOsv o]*"',parts[1]): continue
        formats = parts[1].strip('"').replace(' ','')
        if len(formats) != len(parts) - 2: raise ValueError('add_opvars argument count mismatch')
        for j,(fmt,arg) in enumerate(zip(formats,parts[2:]),2):
            typ = 'int' if fmt in 'io' else 'const char *' if fmt in 'sv' else 'long'
            parts[j] = '('+typ+')('+arg+')'
        edits.append((start,end,', '.join(parts)))
    for start,end,replacement in reversed(edits): text = text[:start]+replacement+text[end:]
    return text
