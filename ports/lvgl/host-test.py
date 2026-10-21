#!/usr/bin/env python3
"""Build the same adapted LVGL/UI with a byte-addressed compiler for comparison."""
from concurrent.futures import ThreadPoolExecutor
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import importlib.util
import tempfile
from adapt import sources
ROOT=Path(__file__).resolve().parents[2]
PORT=Path(__file__).resolve().parent
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--build', type=Path, default=ROOT/'build/lvgl')
parser.add_argument('--bench', action='store_true')
args=parser.parse_args()
build=args.build.resolve()
folder=build/'host';folder.mkdir(exist_ok=True)
reference_home=folder/'reference-home';reference_home.mkdir(exist_ok=True)
reference_env=dict(os.environ,HOME=str(reference_home))
config=folder/'config';config.mkdir(exist_ok=True)
(config/'lv_conf.h').write_bytes((build/'config/lv_conf.h').read_bytes())
flags=['-std=c99','-O1','-g','-fsanitize=undefined','-fno-sanitize-recover=all','-DLV_CONF_INCLUDE_SIMPLE=1',
       '-I'+str(build/'src'),'-I'+str(config),'-I'+str(PORT),'-I'+str(ROOT/'ports/graphics/include')]
if sys.platform=='darwin':flags+=['-isysroot','/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX15.5.sdk']
compiler='/usr/bin/clang' if sys.platform=='darwin' else 'cc'
hasher=hashlib.sha256(repr(flags).encode()+(config/'lv_conf.h').read_bytes())
for path in sorted((build/'src').rglob('*.h')):hasher.update(path.read_bytes())
for path in sorted(PORT.glob('*.h')):hasher.update(path.read_bytes())
prefix=hasher.digest()
def compile(path):
    embedded={'table-test.c':'ui-data.c','app-test.c':'ui-app.c',
              'forms-test.c':'ui-forms.c','plot-component-test.c':'ui-plot.c',
              'tabs-test.c':'ui-tabs.c','editor-workflow-test.c':'editor.c'}.get(path.name)
    digest=hashlib.sha256(prefix+path.read_bytes()+((PORT/embedded).read_bytes() if embedded else b'')).hexdigest()[:16]
    out=folder/(path.stem+'-'+digest+'.o')
    if not out.exists():
        p=subprocess.run([compiler,*flags,'-c',str(path),'-o',str(out)],capture_output=True,text=True)
        if p.returncode:raise RuntimeError(p.stderr)
    return out
with ThreadPoolExecutor(max_workers=4) as pool:
    objects=list(pool.map(compile,[*sources(build/'src'),*[PORT/'fonts'/f'font-{n}.c' for n in json.loads((PORT/'fonts/generated.json').read_text())['sizes']],PORT/'fonts/symbols.c',PORT/'icons/data.c',PORT/'ui-scale.c',PORT/'ui.c',PORT/'ui-data.c',PORT/'ui-app.c',PORT/'ui-forms.c',PORT/'browser.c',PORT/'ui-tabs.c',PORT/'plot.c',PORT/'plot-raster.c',PORT/'ui-plot.c',PORT/'ui-editor.c',PORT/'ui-document.c',ROOT/'libraries/text/text.c',ROOT/'libraries/unicode/unicode.c',PORT/'host-gx.c']))
for app, names in [('lvcalc',('calculator.c','calc.c')),('lvselect',('selector.c',)),('lvgallery',('gallery.c',)),('lvdata',('data-viewer.c','data-plot.c','csv.c')),('lvedit',('editor.c',))]:
    extra=[compile(PORT/name) for name in names]
    subprocess.run([compiler,*flags,*map(str,objects+extra),'-lm','-o',str(folder/app)],check=True)
if args.bench:
    extra=[compile(PORT/name) for name in ('scroll-bench.c','fill-check.c','font-check.c')]
    subprocess.run([compiler,*flags,*map(str,objects+extra),'-lm','-o',str(folder/'lvbench')],check=True)
for scale in (1,2):
    subprocess.run([str(folder/'lvcalc')],env=dict(reference_env,LVGL_SCALE=str(scale),LVCALC_REFERENCE=str(folder/('reference.ppm' if scale==1 else 'reference-2x.ppm'))),check=True)
subprocess.run([compiler,*flags,str(PORT/'model-test.c'),str(PORT/'calc.c'),'-o',str(folder/'model-test')],check=True)
subprocess.run([str(folder/'model-test')],check=True)
spec=importlib.util.spec_from_file_location('fixtures',PORT/'selector-fixtures.py')
fixtures=importlib.util.module_from_spec(spec);spec.loader.exec_module(fixtures)
fixture_path,_=fixtures.prepare()
for scale in (1,2):
    subprocess.run([str(folder/'lvselect'),str(fixture_path)],env=dict(reference_env,LVGL_SCALE=str(scale),LVCALC_REFERENCE=str(folder/('selector.ppm' if scale==1 else 'selector-2x.ppm'))),check=True)
subprocess.run([compiler,*flags,str(PORT/'browser-test.c'),str(PORT/'browser.c'),'-o',str(folder/'browser-test')],check=True)
limit=build/'limit-fixtures';limit.mkdir(exist_ok=True)
for i in range(1025):(limit/f'cap-{i:04}').touch()
subprocess.run([str(folder/'browser-test'),str(fixture_path),str(limit)],check=True)

for scale in (1,2):
    subprocess.run([str(folder/'lvgallery')],env=dict(reference_env,LVGL_SCALE=str(scale),LVCALC_REFERENCE=str(folder/('gallery.ppm' if scale==1 else 'gallery-2x.ppm'))),check=True)

for scale in (1,2):
    subprocess.run([str(folder/'lvgallery')],env=dict(reference_env,LVGL_SCALE=str(scale),LVGL_REFERENCE_CHECKED='1',LVCALC_REFERENCE=str(folder/f'gallery-checked-{scale}x.ppm')),check=True)
    for page in (1,2):
        subprocess.run([str(folder/'lvgallery')],env=dict(reference_env,LVGL_SCALE=str(scale),LVGL_REFERENCE_PAGE=str(page),LVCALC_REFERENCE=str(folder/f'gallery-page-{page}-{scale}x.ppm')),check=True)
    subprocess.run([str(folder/'lvgallery')],env=dict(reference_env,LVGL_SCALE=str(scale),LVGL_REFERENCE_PAGE='3',LVCALC_REFERENCE=str(folder/f'gallery-icons-{scale}x.ppm')),check=True)
    subprocess.run([str(folder/'lvgallery')],env=dict(reference_env,LVGL_SCALE=str(scale),LVGL_REFERENCE_PAGE='4',LVCALC_REFERENCE=str(folder/f'gallery-data-{scale}x.ppm')),check=True)
    subprocess.run([str(folder/'lvgallery')],env=dict(reference_env,LVGL_SCALE=str(scale),LVGL_REFERENCE_PAGE='5',LVCALC_REFERENCE=str(folder/f'gallery-workspace-{scale}x.ppm')),check=True)
    for page, name in ((6,'plots'), (7,'forms'), (8,'editor')):
        subprocess.run([str(folder/'lvgallery')],env=dict(reference_env,LVGL_SCALE=str(scale),LVGL_REFERENCE_PAGE=str(page),LVCALC_REFERENCE=str(folder/f'gallery-{name}-{scale}x.ppm')),check=True)
    subprocess.run([str(folder/'lvgallery')],env=dict(reference_env,LVGL_SCALE=str(scale),LVGL_REFERENCE_PAGE='4',LVGL_REFERENCE_DIALOG='1',LVCALC_REFERENCE=str(folder/f'gallery-dialog-{scale}x.ppm')),check=True)

editor_home=folder/'editor-home';editor_home.mkdir(exist_ok=True)
for scale in (1,2):
    subprocess.run([str(folder/'lvedit'),str(PORT/'examples/editor-demo.c')],env=dict(reference_env,HOME=str(editor_home),LVGL_SCALE=str(scale),LVCALC_REFERENCE=str(folder/f'editor-{scale}x.ppm')),check=True)
    for kind in ('open','save'):
        for small in (False,True):
            name=f'editor-dialog-{kind}-{scale}x'+('-small' if small else '')
            env=dict(reference_env,HOME=str(editor_home),LVGL_SCALE=str(scale),
                     LVGL_REFERENCE_FILE_DIALOG=kind,LVCALC_REFERENCE=str(folder/(name+'.ppm')))
            if small:env['LVGL_REFERENCE_SIZE']='540x360'
            subprocess.run([str(folder/'lvedit'),str(PORT/'examples/editor-demo.c')],env=env,check=True)

data_home=folder/'data-home';data_home.mkdir(exist_ok=True)
spec=importlib.util.spec_from_file_location('data_fixtures',PORT/'data-fixtures.py')
data_fixtures=importlib.util.module_from_spec(spec);spec.loader.exec_module(data_fixtures)
data_folder,_=data_fixtures.prepare()
for scale in (1,2):
    subprocess.run([str(folder/'lvdata'),'--no-plot',str(PORT/'examples/measurements.csv')],env=dict(reference_env,HOME=str(data_home),LVGL_SCALE=str(scale),LVCALC_REFERENCE=str(folder/f'data-{scale}x.ppm')),check=True)
    for name in ('large','wide'):
        subprocess.run([str(folder/'lvdata'),'--no-plot',str(data_folder/(name+'.csv'))],env=dict(reference_env,HOME=str(data_home),LVGL_SCALE=str(scale),LVGL_REFERENCE_END='1',LVCALC_REFERENCE=str(folder/f'data-{name}-end-{scale}x.ppm')),check=True)

# Component-level viewport invariants use the same implementation and UBSan.
extra=compile(PORT/'table-test.c')
subprocess.run([compiler,*flags,*map(str,[obj for obj in objects if not obj.name.startswith('ui-data-')]),str(extra),'-lm','-o',str(folder/'table-test')],check=True)
subprocess.run([str(folder/'table-test')],check=True)
(folder/'table-component-result.json').write_text(json.dumps(dict(status='passed',sanitizer='undefined',input_sha256={name:hashlib.sha256((PORT/name).read_bytes()).hexdigest() for name in ('ui-data.c','ui-data.h','table-test.c','host-gx.c')}),indent=2)+'\n')

extra=compile(PORT/'app-test.c')
subprocess.run([compiler,*flags,*map(str,[obj for obj in objects if not obj.name.startswith('ui-app-')]),str(extra),'-lm','-o',str(folder/'app-test')],check=True)
subprocess.run([str(folder/'app-test')],check=True)
(folder/'app-component-result.json').write_text(json.dumps(dict(status='passed',sanitizer='undefined',input_sha256={name:hashlib.sha256((PORT/name).read_bytes()).hexdigest() for name in ('ui-app.c','ui-app.h','ui-data.c','ui-data.h','app-test.c','host-gx.c')}),indent=2)+'\n')

def component(name, source, embedded, inputs, cwd=None):
    extra=compile(PORT/source)
    selected=[obj for obj in objects if not obj.name.startswith(embedded+'-')]
    program=folder/(name+'-test')
    subprocess.run([compiler,*flags,*map(str,[*selected,extra]),'-lm','-o',str(program)],check=True)
    for scale in (1,2):
        subprocess.run([str(program)],cwd=cwd,env=dict(reference_env,HOME=str(cwd or reference_home),LVGL_SCALE=str(scale)),check=True)
    (folder/(name+'-component-result.json')).write_text(json.dumps(dict(status='passed',sanitizer='undefined',scales=[1,2],input_sha256={name:hashlib.sha256((PORT/name).read_bytes()).hexdigest() for name in inputs}),indent=2)+'\n')

forms_fixtures=folder/'forms-fixtures';forms_fixtures.mkdir(exist_ok=True)
for name in ('existing.txt','other.txt'):(forms_fixtures/name).write_text('Forms test fixture.\n')
component('forms','forms-test.c','ui-forms',
          ('ui-forms.c','ui-forms.h','browser.c','browser.h','ui-data.c','ui-data.h','ui.c','forms-test.c','host-gx.c'),forms_fixtures)
component('tabs','tabs-test.c','ui-tabs',
          ('ui-tabs.c','ui-tabs.h','tabs-test.c','ui.c','host-gx.c'))
workflow_object=compile(PORT/'editor-workflow-test.c')
workflow_program=folder/'editor-workflow-test'
subprocess.run([compiler,*flags,*map(str,objects),str(workflow_object),'-lm','-o',str(workflow_program)],check=True)
for scale in (1,2):
    with tempfile.TemporaryDirectory(prefix='graphite-editor-workflow-') as temporary:
        subprocess.run([str(workflow_program),temporary],env=dict(reference_env,HOME=temporary,LVGL_SCALE=str(scale)),check=True,timeout=60)
(folder/'editor-workflow-result.json').write_text(json.dumps(dict(status='passed',sanitizer='undefined',scales=[1,2],input_sha256={name:hashlib.sha256((PORT/name).read_bytes()).hexdigest() for name in ('editor.c','editor-workflow-test.c','ui-editor.c','ui-editor.h','ui-document.c','ui-document.h','ui-tabs.c','ui-tabs.h','host-gx.c')}),indent=2)+'\n')
component('plot','plot-component-test.c','ui-plot',
          ('ui-plot.c','ui-plot.h','plot.c','plot.h','plot-raster.c','plot-raster.h','ui.c','plot-component-test.c','host-gx.c'))
plot_model=[compile(PORT/name) for name in ('plot-model-test.c','plot.c')]
subprocess.run([compiler,*flags,*map(str,plot_model),'-lm','-o',str(folder/'plot-model-test')],check=True)
subprocess.run([str(folder/'plot-model-test'),str(folder)],check=True)
(folder/'plot-model-result.json').write_text(json.dumps(dict(status='passed',sanitizer='undefined',input_sha256={name:hashlib.sha256((PORT/name).read_bytes()).hexdigest() for name in ('plot.c','plot.h','plot-model-test.c')}),indent=2)+'\n')

plot_raster=[compile(PORT/name) for name in ('plot-raster-test.c','plot-raster.c')]
subprocess.run([compiler,*flags,*map(str,plot_raster),'-o',str(folder/'plot-raster-test')],check=True)
subprocess.run([str(folder/'plot-raster-test')],check=True)
(folder/'plot-raster-result.json').write_text(json.dumps(dict(status='passed',sanitizer='undefined',input_sha256={name:hashlib.sha256((PORT/name).read_bytes()).hexdigest() for name in ('plot-raster.c','plot-raster.h','plot-raster-test.c')}),indent=2)+'\n')

scroll_home=folder/'scroll-home';scroll_home.mkdir(exist_ok=True)
(scroll_home/'.lvdata').write_text('header=1\ndelimiter=0\nwidths=320,320,'+','.join(['144']*62)+'\n')
for scale in (1,2):
    subprocess.run([str(folder/'lvdata'),'--no-plot',str(data_folder/'wide.csv')],env=dict(reference_env,HOME=str(scroll_home),LVGL_SCALE=str(scale),LVGL_REFERENCE_RIGHT='4',LVCALC_REFERENCE=str(folder/f'data-horizontal-{scale}x.ppm')),check=True)

for scale in (1,2):
    subprocess.run([str(folder/'lvdata'),'--no-plot',str(data_folder/'wide.csv')],env=dict(reference_env,HOME=str(data_home),LVGL_SCALE=str(scale),LVGL_REFERENCE_FULLSCREEN='1',LVCALC_REFERENCE=str(folder/f'data-fullscreen-{scale}x.ppm')),check=True)

for scale in (1,2):
    subprocess.run([str(folder/'lvdata'),'--no-plot',str(data_folder/'wide.csv')],env=dict(reference_env,HOME=str(data_home),LVGL_SCALE=str(scale),LVGL_REFERENCE_FULLSCREEN='1',LVGL_REFERENCE_END='1',LVCALC_REFERENCE=str(folder/f'data-fullscreen-end-{scale}x.ppm')),check=True)

wide_home=folder/'wide-screen-home';wide_home.mkdir(exist_ok=True)
(wide_home/'.lvdata').write_text('header=1\ndelimiter=0\nwidths='+','.join(['64']*64)+'\n')
for size in ('1470x900','1920x1080'):
    for scale in (1,2):
        for end in (False,True):
            env=dict(reference_env,HOME=str(wide_home),LVGL_SCALE=str(scale),LVGL_REFERENCE_SIZE=size,LVCALC_REFERENCE=str(folder/(f'data-wide-screen-{size}-{scale}x'+('-end' if end else '')+'.ppm')))
            if end:env['LVGL_REFERENCE_END']='1'
            subprocess.run([str(folder/'lvdata'),'--no-plot',str(data_folder/'wide.csv')],env=env,check=True)

details_home=folder/'details-home';details_home.mkdir(exist_ok=True)
(details_home/'.lvdata').write_text('header=1\ndelimiter=0\ndetails=1\nsplit=700\n')
for scale in (1,2):
    subprocess.run([str(folder/'lvdata'),'--no-plot',str(PORT/'examples/measurements.csv')],env=dict(reference_env,HOME=str(details_home),LVGL_SCALE=str(scale),LVCALC_REFERENCE=str(folder/f'data-details-{scale}x.ppm')),check=True)

plot_home=folder/'plot-home';plot_home.mkdir(exist_ok=True)
(plot_home/'.lvdata').write_text('header=1\ndelimiter=0\nplot=1\nplot_split=450\nplot_x=0\nplot_y=2\nplot_mode=0\n')
for scale in (1,2):
    for mode,name in ((0,'line'),(1,'scatter')):
        (plot_home/'.lvdata').write_text(f'header=1\ndelimiter=0\nplot=1\nplot_split=450\nplot_x=0\nplot_y=2\nplot_mode={mode}\n')
        subprocess.run([str(folder/'lvdata'),'--plot',str(PORT/'examples/measurements.csv')],env=dict(reference_env,HOME=str(plot_home),LVGL_SCALE=str(scale),LVCALC_REFERENCE=str(folder/f'data-plot-{name}-{scale}x.ppm')),check=True)
