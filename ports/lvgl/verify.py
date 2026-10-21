#!/usr/bin/env python3
"""Check the built apps using only fresh offline test images. Never rebuilds product binaries."""
from concurrent.futures import ThreadPoolExecutor,as_completed
from pathlib import Path
import hashlib,json,os,subprocess,sys,time
ROOT=Path(__file__).resolve().parents[2];PORT=Path(__file__).resolve().parent;BUILD=ROOT/'build/lvgl'
# Cases sharing helper binaries or output directories run serially in one group.
GROUPS=[[
 ('calculator','test.py',[]),('calculator-retina','test.py',['--retina']),('density','retina-test.py',[])],
 [('selector','selector-test.py',[]),('selector-retina','selector-test.py',['--retina']),
  ('selector-software','selector-test.py',['--software-upload']),
  ('selector-retina-software','selector-test.py',['--retina','--software-upload']),
  ('selector-rgb565','selector-test.py',['--retina','--rgb565'])],
 [('gallery','gallery-test.py',[]),('gallery-data','gallery-data-test.py',[]),('workspace','workspace-test.py',[]),('gallery-plots-forms','gallery-plot-test.py',[]),('workbench','workbench-test.py',[]),('data','data-test.py',[]),('table-scroll','table-scroll-test.py',[]),('table-fullscreen','table-fullscreen-test.py',[]),('wide-screen','wide-screen-test.py',[]),('csv','csv-model-test.py',[]),('csv-jobs','csv-job-test.py',['--guest']),('mask-raster','mask-raster-test.py',[]),('render-target','render-target-test.py',[])]]
GROUPS.append([('editor','editor-test.py',[]),
               ('desktop','../../fuse-x/test_desktop_mode.py',[
                   '--kernel',os.environ.get('FUSE_X_TEST_KERNEL',str(ROOT/'fuse-x/build/fuse-x')),
                   '--calculator',str(BUILD/'lvcalc.fxe'),'--output',str(BUILD/'desktop-tests')])])
def digest(path):return hashlib.sha256(path.read_bytes()).hexdigest()
def component_reports():
    results=[]
    for name,report_name in (('table-component','table-component'),
                             ('app-component','app-component'),
                             ('forms-component','forms-component'),
                             ('plot-component','plot-component'),
                             ('tabs-component','tabs-component'),
                             ('editor-workflow','editor-workflow'),
                             ('plot-model','plot-model'),
                             ('plot-raster','plot-raster')):
        relative='host/'+report_name+'-result.json'
        path=BUILD/relative
        report=json.loads(path.read_text())
        assert report['status']=='passed',str(path)
        assert report['input_sha256']=={source:digest(PORT/source) for source in report['input_sha256']},name+' report is stale'
        results.append(dict(case=name,status='passed',report=relative))
    return results
def collect(hashes):
    """Collect separately run suites, requiring every report to match this build."""
    reports={
        'calculator':('tests','lvcalc'),'calculator-retina':('tests-2x','lvcalc'),
        'density':('retina-tests',None),'selector':('selector-tests','lvselect'),
        'selector-retina':('selector-tests-2x','lvselect'),
        'selector-software':('selector-tests-software','lvselect'),
        'selector-retina-software':('selector-tests-software-2x','lvselect'),
        'selector-rgb565':('selector-tests-rgb565-2x','lvselect'),
        'gallery':('gallery-tests','lvgallery'),'gallery-data':('gallery-data-tests','lvgallery'),
        'editor':('editor-tests','lvedit'),
        'workspace':('workspace-tests',None),
        'gallery-plots-forms':('gallery-plot-tests','lvgallery'),
        'workbench':('workbench-tests','lvdata'),
        'data':('data-tests','lvdata'),'table-scroll':('table-scroll-tests','lvdata'),'table-fullscreen':('table-fullscreen-tests','lvdata'),'wide-screen':('wide-screen-tests','lvdata')}
    results=[]
    for name,(folder,app) in reports.items():
        path=BUILD/folder/'result.json';report=json.loads(path.read_text())
        assert report['status']=='passed',str(path)
        if app:assert report['executable_sha256']==hashes[app],str(path)+' is from a different build'
        else:assert report['executables']=={app:hashes[app] for app in report['executables']},str(path)
        results.append(dict(case=name,status='passed',report=str(path.relative_to(BUILD))))
    csv=json.loads((BUILD/'csv-model-result.json').read_text());assert csv['status']=='passed'
    assert csv['input_sha256']==csv_sources(),'CSV model report is stale'
    results.append(dict(case='csv',status='passed',report='csv-model-result.json'))
    jobs=json.loads((BUILD/'jobs-agent/result.json').read_text())
    assert jobs['status']=='passed' and jobs['guest_status']=='passed','CSV jobs guest checks missing'
    assert jobs['input_sha256']=={name:digest(PORT/name) for name in jobs['input_sha256']},'CSV jobs report is stale'
    assert jobs['guest_test_sha256']==digest(BUILD/'jobs-agent/csv-job-test.fxe'),'CSV jobs test executable changed'
    results.append(dict(case='csv-jobs',status='passed',report='jobs-agent/result.json'))
    mask=json.loads((BUILD/'mask-raster-tests/result.json').read_text())
    assert mask['status']=='passed' and mask['guest_status']=='passed','A8 raster guest checks missing'
    assert mask['input_sha256']=={name:digest(ROOT/name) for name in mask['input_sha256']},'A8 raster report is stale'
    assert mask['executable_sha256']==digest(BUILD/'mask-raster-tests/mask-check.fxe'),'A8 raster test executable changed'
    kernel=Path(os.environ.get('FUSE_X_TEST_KERNEL',ROOT/'fuse-x/build/fuse-x')).resolve()
    assert mask['kernel_sha256']==digest(kernel),'A8 raster report used a different kernel'
    results.append(dict(case='mask-raster',status='passed',report='mask-raster-tests/result.json'))
    render=json.loads((BUILD/'render-target-tests/result.json').read_text())
    assert render['status']=='passed' and render['guest_status']=='passed','Retained target guest checks missing'
    assert render['input_sha256']=={name:digest(ROOT/name) for name in render['input_sha256']},'Retained target report is stale'
    assert render['executable_sha256']==digest(BUILD/'render-target-tests/render-check.fxe'),'Retained target executable changed'
    assert render['kernel_sha256']==digest(kernel),'Retained target report used a different kernel'
    assert render['lvgl']['status']=='passed','Retained LVGL fallback scene checks missing'
    assert render['lvgl']['executable_sha256']==digest(BUILD/'render-target-tests/render-lvgl-check.fxe'),'Retained LVGL scene executable changed'
    results.append(dict(case='render-target',status='passed',report='render-target-tests/result.json'))
    results+=component_reports()
    editor=json.loads((BUILD/'editor-tests/result.json').read_text())
    assert editor['gallery_executable_sha256']==hashes['lvgallery'],'Editor gallery report is stale'
    for name,relative in (('document-model','document-model/result.json'),
                          ('editor-component','editor-component/result.json'),
                          ('font-data','font-data-tests/result.json')):
        report=json.loads((BUILD/relative).read_text())
        assert report['status']=='passed',name
        assert report['input_sha256']=={source:digest(ROOT/source) for source in report['input_sha256']},name+' report is stale'
        results.append(dict(case=name,status='passed',report=relative))
    desktop=json.loads((BUILD/'desktop-tests/result.json').read_text())
    assert desktop['status']=='passed' and desktop['kernel_sha256']==digest(kernel),'Desktop kernel report is stale'
    assert desktop['calculator_sha256']==hashes['lvcalc'],'Desktop calculator report is stale'
    assert desktop['test_source_sha256']==digest(ROOT/'fuse-x/test_desktop_mode.py'),'Desktop acceptance source changed'
    results.append(dict(case='desktop',status='passed',report='desktop-tests/result.json'))
    scanner=json.loads((BUILD/'browser-scan-tests/result.json').read_text())
    assert scanner['status']=='passed' and scanner['guest_status']=='passed','Browser scanner guest checks missing'
    assert scanner['input_sha256']=={source:digest(ROOT/source) for source in scanner['input_sha256']},'Browser scanner report is stale'
    assert scanner['kernel_sha256']==digest(kernel),'Browser scanner kernel report is stale'
    assert scanner['executable_sha256']==digest(BUILD/'browser-scan-tests/browser-scan.fxe'),'Browser scanner executable changed'
    results.append(dict(case='browser-scan',status='passed',report='browser-scan-tests/result.json'))
    return results
def csv_sources():return {name:digest(PORT/name) for name in ('csv.c','csv.h','csv-test.c','csv-model-test.py')}
def main():
    out=BUILD/'verification';out.mkdir(exist_ok=True);(out/'result.json').unlink(missing_ok=True)
    hashes={app:digest(BUILD/(app+'.fxe')) for app in ('lvcalc','lvselect','lvgallery','lvdata','lvedit')}
    assert hashes==json.loads((BUILD/'build-info.json').read_text())['executables'],'Build metadata does not match binaries'
    if '--collect' in sys.argv:
        # The caller must have run the host reference check for this build.
        report=dict(status='passed',executables=hashes,cases=collect(hashes),collection='Existing individual reports; hashes validated')
        (out/'result.json').write_text(json.dumps(report,indent=2)+'\n')
        print('All individual reports match the current build',flush=True)
        return
    for script in ('document-model-test.py','ui-editor-test.py','font-data-test.py','browser-scan-test.py'):
        with (out/(Path(script).stem+'.log')).open('w') as log:
            subprocess.run([sys.executable,str(PORT/script)],cwd=ROOT,stdout=log,stderr=subprocess.STDOUT,check=True)
    with (out/'host.log').open('w') as log:
        subprocess.run([sys.executable,str(PORT/'host-test.py')],cwd=ROOT,stdout=log,stderr=subprocess.STDOUT,check=True)
    print('Host references and UBSan: passed',flush=True)
    def group(cases):
        results=[]
        for name,script,args in cases:
            start=time.monotonic()
            with (out/(name+'.log')).open('w') as log:
                result=subprocess.run([sys.executable,str(PORT/script),*args],cwd=ROOT,stdout=log,stderr=subprocess.STDOUT)
            row=dict(case=name,status='passed' if result.returncode==0 else 'failed',seconds=round(time.monotonic()-start,3))
            results.append(row);print(name+': '+row['status'],flush=True)
            if result.returncode:raise RuntimeError(name+' failed: '+str(out/(name+'.log')))
        return results
    results=component_reports()
    with ThreadPoolExecutor(max_workers=3) as pool:
        futures=[pool.submit(group,cases) for cases in GROUPS]
        for task in as_completed(futures):results+=task.result()
    assert hashes=={app:digest(BUILD/(app+'.fxe')) for app in hashes},'Product executables changed during testing'
    report=dict(status='passed',executables=hashes,host='UBSan; references for all gallery pages and 1x/2x apps',cases=collect(hashes))
    (out/'result.json').write_text(json.dumps(report,indent=2)+'\n')
    print('All built applications verified; product binaries unchanged',flush=True)
if __name__=='__main__':main()
