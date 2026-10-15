#!/usr/bin/env python3
# Kit H spec from kit F's: kit G inputs, runtime and shell at prospero-win REVISION, and the opengl-zink capability.
# Usage: make_spec.py REVISION
# Z3CLOUDPLAY_KIT_DIR: where kit H is staged (default build/kit-h). Z3CLOUDPLAY_KIT_F: kit F's build (default build/kit-f).
import json, os, sys
from pathlib import Path
z3cloudplay = Path(__file__).resolve().parents[2]
here = Path(os.environ.get('Z3CLOUDPLAY_KIT_DIR', z3cloudplay / 'build/kit-h'))
kit_f = Path(os.environ.get('Z3CLOUDPLAY_KIT_F', z3cloudplay / 'build/kit-f'))
revision = sys.argv[1]
text = json.dumps(json.loads((kit_f / 'kit-spec.json').read_text()))
text = text.replace(str(kit_f), str(here))
spec = json.loads(text)
spec['capabilities'] = sorted(set(spec['capabilities']) | {'opengl-zink'})
spec['runtime']['artifact_revision'] = f'{revision[:7]}-kit-h-r1'
spec['runtime']['wine_revision'] = revision
spec['sources']['wine']['revision'] = revision
spec['sources']['wrapper']['revision'] = revision
spec['template']['preparation']['id'] = f'hl2-fresh-v16-neutral-{revision[:7]}'
shell = here / f'inputs/titles/any-{revision[:7]}'
spec['variants'][0].update(id=f'any-title-{revision[:7]}', path=str(shell), wrapper_revision=revision,
                           helper_manifest=str(shell / 'lapy-manifest.json'),
                           helper_protocol=str(here / 'inputs/lapy_elevation_protocol.h'),
                           native_build_manifest=str(shell / 'native-title-build.json'))
(here / 'kit-spec.json').write_text(json.dumps(spec, indent=1) + '\n')
print(json.dumps({k: spec[k] for k in ('capabilities', 'runtime')}, indent=1))
