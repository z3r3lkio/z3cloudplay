from pathlib import Path
for p in Path('src/runtime').rglob('*'):
    if p.is_file() and p.suffix in ('.cpp','.h','.hpp','.cc'):
        old=p.read_text(encoding='utf-8')
        new=old.replace('playden','z3cloudplay').replace('Playden','Z3CloudPlay').replace('PLAYDEN','Z3CLOUDPLAY')
        if new!=old:
            p.write_text(new,encoding='utf-8')
