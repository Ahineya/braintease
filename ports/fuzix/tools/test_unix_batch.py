#!/usr/bin/env python3
"""Exercise the six Unix ports on the host or real kernels, on disposable disks."""
import argparse,calendar,datetime,os,re,shlex,subprocess,sys,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[3]
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--target',choices=('host','native','ripple','x64'),default='host')
p.add_argument('--x64-boot',type=Path,default=ROOT/'build/x86_64-boot/normal',help='existing virtio-root x64 boot build, used read-only')
p.add_argument('--match',help='run only case labels matching this regular expression')
a=p.parse_args(); programs=('tree','getopt','cron','at','atrun','cal'); cases=[]
with tempfile.TemporaryDirectory(prefix='unix-batch-',dir='/tmp') as tmp:
    work=Path(tmp).resolve(); base=str(work/'u') if a.target=='host' else '/tmp/u'
    B=shlex.quote(base)
    fixture=[]
    def file(path,text,mode=0o100600):fixture.append(dict(name=base+'/'+path,text=text,mode=mode))
    def directory(path,mode=0o40700):fixture.append(dict(name=(base+'/'+path).rstrip('/'),directory=True,mode=mode))
    def case(label,command,expected=None,status=0,error=None,pattern=None):
        cases.append((label,command,expected,status,error,pattern))
    directory('');directory('tree');directory('tree/dir')
    file('tree/a','a');file('tree/z','z');file('tree/.hidden','hidden');file('tree/dir/b','b')
    fixture += [dict(name=base+'/tree/link',symlink='dir'),dict(name=base+'/tree/broken',symlink='absent')]
    directory("cwd 'quoted")
    file('job','echo ran >> '+B+'/result\n')
    file('job-env','echo "$MESSAGE"\npwd\numask\n')
    file('job-mask','echo MASK_OK\n')
    file('job-fail','echo FAILING\nexit 7\n')
    fixed=datetime.datetime(2026,10,21,12,34,tzinfo=datetime.timezone.utc)
    epoch=int(fixed.timestamp())
    cron=[('34 12 * * *','match'),('0 0 * * *','no'),('*/2 12 * 10 *','step'),
          ('1,34,59 12 * * *','list'),('30-40/2 12 * * *','range'),
          ('34 12 1 * 3','dow-or'),('34 12 21 * 0','dom-or'),
          ('34 12 * * 0','no-sunday'),('34 12 1 * *','no-dom')]
    file('crontab',''.join(f'{spec} echo {tag} >> {B}/hits\n' for spec,tag in cron))
    file('empty-cron','# No scheduled commands\n')
    file('long-cron','* * * * * '+'x'*3000+'\n')
    for i,spec in enumerate(('60 * * * *','* 24 * * *','* * 0 * *','* * * 13 *','* * * * 8','*/0 * * * *','9-3 * * * *','1,,2 * * * *','* * * * *')):
        file(f'bad-cron{i}',spec+'\n')
    file('unsafe-cron','* * * * * echo unsafe\n',0o100666)
    file('cron-fail','* * * * * exit 7\n')
    file('cron-sunday','0 0 * * 7 echo SUNDAY\n')
    file('service-cron','* * * * * echo CRON_SERVICE >> '+B+'/service-result\n')
    # Exact tree output also proves links (including cycles) are not descended.
    tree=base+'/tree\n|-- a\n|-- broken -> absent\n|-- dir\n|   `-- b\n|-- link -> dir\n`-- z\n\n1 directories, 5 files\n'
    case('sorted physical tree',f'tree {B}/tree',tree)
    case('tree depth',f'tree -L 1 {B}/tree',tree.replace('|   `-- b\n','').replace('5 files','4 files'))
    case('directory filter',f'tree -d {B}/tree',base+'/tree\n`-- dir\n\n1 directories\n')
    case('hidden files',f'tree -a {B}/tree',pattern=r'\|-- \.hidden\n')
    case('tree missing',f'tree {B}/missing',status=1,error='missing')
    for arg in ('0','-1','65536','bad'):
        case('invalid depth '+arg,f'tree -L {shlex.quote(arg)}',expected='',status=2,error='usage:')
    args=['-ab',"two ' words",'--','',"$(echo nope); *"]
    canonical="'-a' '-b' 'two '\\'' words' '--' '' '$(echo nope); *'\n"
    case('quoted getopt',shlex.join(['getopt','ab:',*args]),canonical)
    case('empty option set',"getopt '' -- ''", "'--' ''\n")
    case('missing getopt spec','getopt',expected='',status=2,error='usage:')
    case('unknown option',"getopt a -ax",expected='',status=1,error='option')
    case('missing option value',"getopt ab: -ab",expected='',status=1,error='argument')
    case('quoted empty positional values',"set -- a '' b; for x in \"$@\"; do echo \"<$x>\"; done",'<a>\n<>\n<b>\n')
    case('quoted empty concatenation',"set -- ''; echo \"pre$@post\"",'prepost\n')
    case('set clears arguments',"set -- one two; set --; echo \"$#\"",'0\n')
    case('set preserves flags',"set -u; set -- -a; echo \"$1\"; echo \"$-\"",pattern=r'-a\n.*u')
    case('shell getopt roundtrip',"eval set -- `getopt ab: -a -b \"two ' words\" -- '' 'x;*'`; for x in \"$@\"; do echo \"<$x>\"; done", "<-a>\n<-b>\n<two ' words>\n<-->\n<>\n<x;*>\n")
    for year,month in ((2024,2),(2025,2),(2000,2),(1900,2),(2026,10),(9999,12),(1,1)):
        # Python is proleptic Gregorian; use it only after the BSD 1752 reform.
        if year>=1900:
            cal=calendar.TextCalendar(firstweekday=6).formatmonth(year,month).rstrip().splitlines()
            expected='\n'.join([cal[0].strip(),*cal[1:]])
            case(f'calendar {month}/{year}',f'cal {month} {year}',expected+'\n')
        else:case('early calendar',f'cal {month} {year}',pattern='January 1')
    case('British calendar reform','cal 9 1752',pattern=r'1  2 14 15 16')
    case('Julian calendar','cal -j 2 2024',pattern='60')
    case('whole year','cal 2026',pattern='December')
    for args in ('13 2026','2 0','2 10000','2 2026junk','bad'):
        case('invalid calendar '+args,'cal '+args,expected='',status=1,error='illegal')
    case('cron validation',f'cron -t -c {B}/crontab',expected='')
    for i in range(9):case(f'bad cron {i}',f'cron -t -c {B}/bad-cron{i}',expected='',status=1,error='invalid schedule')
    case('bounded cron line',f'cron -t -c {B}/long-cron',expected='',status=1)
    case('unsafe config rejected',f'cron -t -c {B}/unsafe-cron',expected='',status=1)
    case('cron matching',f'cron --once --time {epoch} --no-at -c {B}/crontab; cat {B}/hits', 'match\nstep\nlist\nrange\ndow-or\ndom-or\n')
    case('job exit propagated',f'cron --once --time {epoch} --no-at -c {B}/cron-fail',expected='',status=1,error='job')
    sunday=int(datetime.datetime(2026,10,25,tzinfo=datetime.timezone.utc).timestamp())
    case('Sunday alias',f'cron --once --time {sunday} --no-at -c {B}/cron-sunday','SUNDAY\n')
    case('explicit time needs once',f'cron --time {epoch}',expected='',status=2,error='usage:')
    case('empty at queue',f'at -s {B}/spool -l',expected='')
    case('queue and execute once',f'at -s {B}/spool -f {B}/job now; atrun -s {B}/spool; atrun -s {B}/spool; cat {B}/result',pattern=r'job j[0-9-]+\nran\n$')
    case('completed queue empty',f'at -s {B}/spool -l',expected='')
    case('future queue and cancel',f'set -- `at -s {B}/spool -f {B}/job now + 2 days`; at -s {B}/spool -l; at -s {B}/spool -r "$2"; at -s {B}/spool -l',pattern=r'^j[0-9-]+ [0-9-]+ [0-9:]+\n$')
    case('future jobs not run',f'at -s {B}/later -f {B}/job now + 1 day; atrun -s {B}/later; at -s {B}/later -l',pattern=r'job (j[0-9-]+)\n\1 ')
    case('atomic competing runners',f'at -s {B}/race -f {B}/job now; atrun -s {B}/race & atrun -s {B}/race & wait; cat {B}/result',pattern=r'job j[0-9-]+\nran\nran\n$')
    case('capture cwd env and umask',f'cd {B}/"cwd \'quoted"; MESSAGE="a \' quote; *"; export MESSAGE; umask 027; at -s {B}/env -f {B}/job-env now; atrun -s {B}/env; cat {B}/env/output/*',pattern=r'job j[0-9-]+\na \' quote; \*\n'+re.escape(base+"/cwd 'quoted")+r'\n0?027\n$')
    case('failing at job',f'at -s {B}/fail -f {B}/job-fail now; atrun -s {B}/fail',status=1,pattern=r'^job j[0-9-]+\n$')
    case('failed job retained output',f'cat {B}/fail/output/*',pattern=r'FAILING\natrun: job failed')
    for args in ('','25:00','12:60','now + -1 days','now + 99999999999 days','-t 202502300000','-t 197001010000','-r ../../etc/crontab'):
        case('invalid at '+args,f'at -s {B}/spool '+args,expected='',status=1 if args.startswith('-r') else 2)
    case('absolute leap date',f'at -s {B}/date -t 202802291230 -f {B}/job; at -s {B}/date -l',pattern=r'2028-02-29 12:30:00')
    case('bad absolute leap date',f'at -s {B}/date -t 202902291230 -f {B}/job',expected='',status=2)
    case('DST gap rejected',f'TZ=EST5EDT,M3.2.0/2,M11.1.0/2 at -s {B}/date -t 202803120230 -f {B}/job',expected='',status=2)
    case('alternate local timezone',f'TZ=ABC-3 at -s {B}/zone -t 202802291230 -f {B}/job; TZ=ABC-3 at -s {B}/zone -l',pattern=r'2028-02-29 12:30:00')
    case('relative spool rejected','at -s relative -l',expected='',status=1)
    case('linked spool rejected',f'ln -s spool {B}/linked; at -s {B}/linked -l',expected='',status=1)
    directory('malformed');directory('malformed/jobs');directory('malformed/running');directory('malformed/output')
    file('malformed/jobs/j0000000000-00000-00000','# riplix-at 1 123\necho WRONG\n')
    case('malformed queue header',f'atrun -s {B}/malformed',expected='',status=1)
    case('private queue with restrictive umask',f'(umask 777; at -s {B}/mask -f {B}/job-mask now); atrun -s {B}/mask; cat {B}/mask/output/*',pattern=r'job j[0-9-]+\nMASK_OK\n$')
    case('long spool rejected','at -s /'+'x'*223+' -l',expected='',status=1)
    case('unsafe spool',f'at -s {B}/unsafe -l',expected='',status=1)
    directory('unsafe',0o40777)
    file('cron.ini','[service]\ntype=simple\nprogram=/bin/cron\nrestart=no\n[arguments]\n1=-f\n2=-c\n3='+base+'/service-cron\n4=-s\n5='+base+'/service-at\n')
    # Host uses its own /bin, so the end-to-end supervisor/atrun dispatch check
    # belongs on the two actual OS targets.
    if a.target!='host':
        directory('definitions');fixture.append(dict(name=base+'/definitions/cron.ini',symlink='../cron.ini'))
        # Definitions cannot be symlinks in svcd; install the regular example.
        fixture[-1]=dict(name=base+'/definitions/cron.ini',text=next(f['text'] for f in fixture if f['name']==base+'/cron.ini'),mode=0o100600)
        case('cron through supervisor',f'svcd --config {B}/definitions --runtime {B}/run > {B}/svcd.log 2>&1 & supervisor=$!; sleep 1; svc --runtime {B}/run start cron; sleep 2; svc --runtime {B}/run status; svc --runtime {B}/run shutdown; wait $supervisor; cat {B}/service-result',pattern=r'CRON_SERVICE\n$')
    if a.match: cases=[c for c in cases if re.search(a.match,c[0])]
    if a.target=='host':
        (ROOT/'build/unix-tools/host').mkdir(parents=True,exist_ok=True)
        for n in programs:
            d='V7/cmd' if n in ('cron','at','atrun') else 'util'
            sources=[str(ROOT/f'ports/fuzix/upstream/Applications/{d}/{n}.c')]
            if d=='V7/cmd':sources.append(str(ROOT/'ports/fuzix/ripple/schedule.c'))
            subprocess.run(['/usr/bin/clang','--ld-path=/Library/Developer/CommandLineTools/usr/bin/ld','-std=gnu11','-Wall','-Wextra','-Werror','-O2',*sources,'-o',str(ROOT/'build/unix-tools/host'/n)],check=True)
        for f in fixture:
            dest=Path(f['name']);dest.parent.mkdir(parents=True,exist_ok=True)
            if f.get('directory'):dest.mkdir(exist_ok=True);dest.chmod(f['mode']&0o777)
            elif 'symlink' in f:dest.symlink_to(f['symlink'])
            else:dest.write_text(f['text']);dest.chmod(f['mode']&0o777)
        env=dict(os.environ,PATH=str(ROOT/'build/unix-tools/host')+':/usr/bin:/bin',TZ='UTC0')
        guest=None
    else:
        sys.path.insert(0,str(ROOT/'ports/tls/tests'));from guest import Guest
        for i,(_,command,*_) in enumerate(cases):
            fixture.append(dict(name=f'/tmp/case{i}',text='TZ=UTC0; export TZ\n( '+command+f' ) > /tmp/o 2> /tmp/e\necho UNIX_STATUS:$?\ncat /tmp/o\necho UNIX_STDERR\ncat /tmp/e\n'))
        ext='fxe' if a.target in ('native','x64') else 'rxe'
        build=ROOT/('build/unix-tools/x64/apps' if a.target=='x64' else 'fuse-x/build/apps' if a.target=='native' else 'build/fuzix')
        binaries={n:build/(n+'.'+ext) for n in (*programs,'mkdir','chmod','sleep','ln','pwd')}
        services=build if a.target=='x64' else ROOT/('fuse-x/build/apps' if a.target=='native' else 'build/services/ripple')
        binaries.update({n:services/(n+'.'+ext) for n in ('svcd','svc')})
        if a.target=='x64':
            sys.path.insert(0,str(ROOT/'tools/x86_64'));from unix_check import UnixGuest
            guest=UnixGuest(work,a.x64_boot,build,fixture,len(cases))
        else:guest=Guest(work,a.target,binaries,fixture)
    try:
        for i,(label,command,expected,status,error,pattern) in enumerate(cases):
            if guest:
                result=guest.shell(f'sh /tmp/case{i}',seconds=180).split('\n',1)[1]
                m=re.search(r'UNIX_STATUS:(\d+)\n',result);assert m,(label,result)
                code=int(m[1]);stdout,stderr=result[m.end():].split('UNIX_STDERR\n',1)
                stderr=stderr.rsplit(guest.prompt.decode(),1)[0]
            else:
                r=subprocess.run(['/bin/sh','-c',command],capture_output=True,text=True,env=env,timeout=30)
                code,stdout,stderr=r.returncode,r.stdout,r.stderr
            if label=='cron matching':
                stdout=''.join(sorted(stdout.splitlines(keepends=True)))
                expected=''.join(sorted(expected.splitlines(keepends=True)))
            if label.startswith('calendar '):
                lines=[line.rstrip() for line in stdout.rstrip().splitlines()]
                lines[0]=lines[0].strip();stdout='\n'.join(lines)+'\n'
            assert code==status and (expected is None or stdout==expected) and (not error or error in stderr) and (not pattern or re.search(pattern,stdout)),(label,code,stdout,stderr,expected,pattern)
            print(a.target+': '+label,flush=True)
    finally:
        if guest:
            (ROOT/f'build/unix-tools/{a.target}-tests.log').write_bytes(guest.log)
            guest.close()
if a.target=='native' and not a.match:
    sys.path.insert(0,str(ROOT/'fuse-x'))
    from test_support import record_runtime
    record_runtime(ROOT/'fuse-x/build',(*programs,'crond'),'passed: Unix utility and scheduler behavior, supervisor integration')
print(f'UNIX_BATCH_OK {a.target}: {len(cases)} cases')
